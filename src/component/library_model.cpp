#include "library_model.h"
#include "library_presentation_policy.h"

#include "album_session_service.h"
#include "text_util.h"
#include "workspace_notifications.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <string_view>
#include <utility>

namespace nekorox::component {
namespace {

enum Field : std::size_t {
    fieldGenre,
    fieldArtist,
    fieldAlbumArtist,
    fieldAlbumArtists,
    fieldAlbum,
    fieldTitle,
    fieldYear,
    fieldCodec,
    fieldReleaseId,
    fieldDisc,
    fieldTrack,
    fieldRating,
    fieldPlayCount,
    fieldSampleRate,
    fieldBitsPerSample,
    fieldCount,
};

constexpr std::array<const char*, fieldCount> kFieldScripts{
    "$if2(%genre%,Unknown)",
    "$if2(%artist%,Unknown artist)",
    "$if2(%album artist%,$if2(%artist%,Unknown artist))",
    "$if($meta(album artist),$meta_sep(album artist,$char(31)),$if($meta(artist),$meta_sep(artist,$char(31)),Unknown artist))",
    "$if2(%album%,Unknown album)",
    "$if2(%title%,%filename%)",
    "$if2(%date%,)",
    "$upper($if2(%codec%,))",
    "$if3($meta(MUSICBRAINZ_ALBUMID),$meta(MUSICBRAINZ ALBUM ID),)",
    "$if2(%discnumber%,0)",
    "$if2(%tracknumber%,0)",
    "$if2(%rating%,0)",
    "$if2(%play_count%,0)",
    "$if2(%samplerate%,0)",
    "$if2(%__bitspersample%,0)",
};
std::atomic<std::uint64_t> titleformatCalls{0};

class FieldCapture final : public titleformat_hook {
public:
    std::array<std::wstring, fieldCount> values;
    std::array<bool, fieldCount> seen{};
    bool process_field(titleformat_text_out*, const char*, t_size, bool&) override
    { return false; }
    bool process_function(titleformat_text_out*, const char* name, t_size length,
        titleformat_hook_function_params* params, bool& found) override
    {
        if (std::string_view(name, length) != "goden_library_field" ||
            params->get_param_count() != 2) return false;
        const auto index = params->get_param_uint(0);
        if (index >= fieldCount) return false;
        const char* value = nullptr; t_size bytes = 0;
        params->get_param(1, value, bytes);
        values[index] = trimText(utf8ToWide(std::string(value, bytes).c_str()));
        seen[index] = true;
        found = true;
        return true;
    }
};

bool sameDescriptionInfo(const metadb_info_container::ptr& a,
    const metadb_info_container::ptr& b)
{
    if (a == b) return true;
    if (!a.is_valid() || !b.is_valid() || a->stats() != b->stats() ||
        a->isInfoPartial() != b->isInfoPartial()) return false;
    const auto& left = a->info(); const auto& right = b->info();
    return left.get_length() == right.get_length() &&
        left.get_replaygain() == right.get_replaygain() &&
        file_info::g_is_meta_equal(left, right) && file_info::g_is_info_equal(left, right);
}

int parseInt(const std::wstring& text) noexcept
{
    return static_cast<int>(std::wcstol(text.c_str(), nullptr, 10));
}

std::wstring normalizeYear(std::wstring value)
{
    value = trimText(std::move(value));
    if (value.size() >= 4 &&
        std::all_of(value.begin(), value.begin() + 4, [](wchar_t character) {
            return character >= L'0' && character <= L'9';
        })) {
        value.resize(4);
    }
    return value;
}

std::vector<std::wstring> splitMetadataValues(std::wstring value)
{
    constexpr wchar_t separator = 0x1f;
    std::vector<std::wstring> result;
    std::size_t start = 0;
    for (;;) {
        const auto end = value.find(separator, start);
        auto item = trimText(value.substr(start, end - start));
        if (!item.empty() && std::none_of(
                result.begin(), result.end(), [&](const std::wstring& existing) {
                    return compareText(existing, item) == 0;
                })) {
            result.push_back(std::move(item));
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return result;
}

bool trackLess(const LibraryTrack& left, const LibraryTrack& right)
{
    int value = compareText(left.albumArtist, right.albumArtist);
    if (value != 0) return value < 0;
    value = compareText(left.album, right.album);
    if (value != 0) return value < 0;
    if (left.disc != right.disc) return left.disc < right.disc;
    if (left.track != right.track) return left.track < right.track;
    return compareText(left.path, right.path) < 0;
}

std::wstring foldedSearchText(const LibraryTrack& track)
{
    std::wstring text;
    const auto reserve = track.genre.size() + track.artist.size() +
        track.albumArtist.size() + track.album.size() + track.title.size() +
        track.year.size() + 6U;
    text.reserve(reserve);
    const auto append = [&text](const std::wstring& value) {
        if (!text.empty()) text.push_back(0x1f);
        for (const auto character : value) {
            text.push_back(static_cast<wchar_t>(std::towlower(character)));
        }
    };
    append(track.genre);
    append(track.artist);
    append(track.albumArtist);
    append(track.album);
    append(track.title);
    append(track.year);
    return text;
}

std::wstring formatTrackField(
    const metadb_handle_ptr& handle,
    const std::vector<titleformat_object::ptr>& scripts,
    std::size_t field)
{
    pfc::string8_fast output;
    if (field >= scripts.size() || !scripts[field].is_valid() ||
        !handle->format_title(nullptr, output, scripts[field], nullptr)) {
        return {};
    }
    return trimText(utf8ToWide(output.c_str()));
}

LibraryTrack describeTrackWithScripts(
    const metadb_handle_ptr& handle,
    const std::vector<titleformat_object::ptr>& scripts, bool batch = true)
{
    LibraryTrack track;
    if (!handle.is_valid()) return track;
    track.handle = handle;
    FieldCapture capture;
    bool captured = false;
    if (batch && scripts.size() > fieldCount && scripts[fieldCount].is_valid()) {
        pfc::string8_fast unused;
        titleformatCalls.fetch_add(1, std::memory_order_relaxed);
        captured = handle->format_title(&capture, unused, scripts[fieldCount], nullptr) &&
            std::all_of(capture.seen.begin(), capture.seen.end(), [](bool value) { return value; });
    }
    const auto field = [&](std::size_t index) {
        if (captured) return std::move(capture.values[index]);
        if (batch) titleformatCalls.fetch_add(1, std::memory_order_relaxed);
        return formatTrackField(handle, scripts, index);
    };
    track.genre = field(fieldGenre);
    track.artist = field(fieldArtist);
    track.albumArtist = field(fieldAlbumArtist);
    track.albumArtists = splitMetadataValues(
        field(fieldAlbumArtists));
    if (track.albumArtists.empty() && !track.albumArtist.empty()) {
        track.albumArtists.push_back(track.albumArtist);
    }
    track.album = field(fieldAlbum);
    track.title = field(fieldTitle);
    track.year = normalizeYear(field(fieldYear));
    track.codec = field(fieldCodec);
    track.releaseId = field(fieldReleaseId);
    track.disc = parseInt(field(fieldDisc));
    track.track = parseInt(field(fieldTrack));
    track.rating = std::clamp(
        parseInt(field(fieldRating)), 0, 5);
    track.playCount = std::max(
        0, parseInt(field(fieldPlayCount)));
    track.sampleRate = std::max(
        0, parseInt(field(fieldSampleRate)));
    track.bitsPerSample = std::max(
        0, parseInt(field(fieldBitsPerSample)));
    track.durationSeconds = std::max(0.0, handle->get_length());
    track.path = utf8ToWide(handle->get_path());
    track.searchFolded = foldedSearchText(track);
    return track;
}

std::string handleKey(const metadb_handle_ptr& handle)
{
    if (!handle.is_valid()) return {};
    std::string key(handle->get_path());
    key.push_back('\x1f');
    key += std::to_string(handle->get_subsong_index());
    return key;
}

} // namespace

struct LibraryModel::ObserverBundle {
    class LibraryObserver : public library_callback_v2_dynamic_impl_base {
    public:
        void on_items_added(metadb_handle_list_cref handles) override
        {
            LibraryModel::instance().scheduleChanged(handles, false);
        }
        void on_items_removed(metadb_handle_list_cref) override
        {
            LibraryModel::instance().invalidateMembership();
        }
        void on_items_modified_v2(
            metadb_handle_list_cref handles,
            metadb_io_callback_v2_data&) override
        {
            LibraryModel::instance().scheduleChanged(handles, false);
        }
        void on_library_initialized() override
        {
            LibraryModel::instance().invalidateMembership();
        }
    } library;
    class DisplayObserver : public metadb_io_callback_v2_dynamic_impl_base {
    public:
        void on_changed_sorted_v2(metadb_handle_list_cref handles,
            metadb_io_callback_v2_data&, bool fromHook) override
        {
            if (fromHook)
                LibraryModel::instance().scheduleChanged(handles, true);
        }
    } display;
};

LibraryModel& LibraryModel::instance()
{
    static LibraryModel model;
    return model;
}

void LibraryModel::start()
{
    if (started_.exchange(true, std::memory_order_acq_rel)) return;
    ++refreshLifetime_;
    refreshScheduled_ = false;
    if (!observers_) observers_ = std::make_unique<ObserverBundle>();
    compileScripts();
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        stopping_ = false;
        pendingRefresh_.reset();
    }
    worker_ = std::thread(&LibraryModel::workerMain, this);
    changeInbox_.invalidateAll();
    dirty_.store(true, std::memory_order_release);
    refreshPending_.store(false, std::memory_order_release);
    requestRefresh();
}

void LibraryModel::stop()
{
    if (!started_.exchange(false, std::memory_order_acq_rel)) return;
    ++refreshLifetime_;
    refreshScheduled_ = false;
    changeInbox_.stop();
    forceDescriptions_.clear();
    forceAllDescriptions_ = false;
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        stopping_ = true;
        pendingRefresh_.reset();
    }
    workerChanged_.notify_one();
    if (worker_.joinable()) worker_.join();
    observers_.reset();
    scripts_.reset();
    tracksView_.reset();
    sharedSnapshot_.store({}, std::memory_order_release);
    dirty_.store(true, std::memory_order_release);
    refreshPending_.store(false, std::memory_order_release);
}

LibraryModel::~LibraryModel()
{
    stop();
}

void LibraryModel::invalidate()
{
    forceAllDescriptions_ = true;
    forceDescriptions_.clear();
    invalidateMembership();
}

void LibraryModel::invalidateMembership()
{
    dirty_.store(true, std::memory_order_release);
    if (started_.load(std::memory_order_acquire)) {
        changeInbox_.invalidateAll();
        requestRefresh();
    }
    notifyWorkspaceModelChanged();
}

void LibraryModel::invalidateModified(metadb_handle_list_cref handles)
{
    if (handles.get_count() == 0) {
        invalidate();
        return;
    }
    scheduleChanged(handles, true);
}

void LibraryModel::scheduleChanged(metadb_handle_list_cref handles,
    bool forceDescription)
{
    if (!started_.load(std::memory_order_acquire) || handles.get_count() == 0)
        return;
    const auto base = sharedSnapshot_.load(std::memory_order_acquire);
    if (forceDescription && handles.get_count() > 4096) {
        forceAllDescriptions_ = true;
        forceDescriptions_.clear();
    }
    if (!base || !base->tracks || !scripts_ || handles.get_count() > 4096) {
        changeInbox_.invalidateAll();
        if (forceDescription) {
            forceAllDescriptions_ = true;
            forceDescriptions_.clear();
        }
        dirty_.store(true, std::memory_order_release);
        requestRefresh();
        return;
    }
    for (t_size index = 0; index < handles.get_count(); ++index) {
        const auto key = handleKey(handles[index]);
        // Hook updates for playlist-only items must not insert them into Library.
        if (forceDescription && !library_manager::get()->is_item_in_library(handles[index]))
            continue;
        if (forceDescription && !forceAllDescriptions_) {
            if (forceDescriptions_.size() >= 4096) {
                forceAllDescriptions_ = true;
                forceDescriptions_.clear();
            } else forceDescriptions_.insert(key);
        }
        if (!forceDescription && changeInbox_.fullPending()) break;
        if (changeInbox_.modify(key, handles[index]))
            coalescedHandles_.fetch_add(1, std::memory_order_relaxed);
    }
    dirty_.store(true, std::memory_order_release);
    requestRefresh();
}

void LibraryModel::invalidateTrack(const metadb_handle_ptr& handle)
{
    if (!handle.is_valid()) return;
    const auto base=sharedSnapshot_.load(std::memory_order_acquire);
    if (!base || !base->tracks) {invalidate();return;}
    if (!base->handleIndex.contains(handleKey(handle))) return;
    metadb_handle_list items;items.add_item(handle);invalidateModified(items);
}

const std::vector<LibraryTrack>& LibraryModel::tracks()
{
    static const std::vector<LibraryTrack> empty;
    return tracksView_ ? *tracksView_ : empty;
}

std::shared_ptr<const LibrarySnapshot> LibraryModel::sharedSnapshot()
{
    return sharedSnapshot_.load(std::memory_order_acquire);
}

LibraryModel::Diagnostics LibraryModel::diagnostics() const noexcept
{
    return {
        fullRebuilds_.load(std::memory_order_relaxed),
        deltaRebuilds_.load(std::memory_order_relaxed),
        describedTracks_.load(std::memory_order_relaxed),
        publicationDeepCopyBytes_.load(std::memory_order_relaxed),
        indexedNowPlayingHits_.load(std::memory_order_relaxed),
        nowPlayingLinearScans_.load(std::memory_order_relaxed),
        fullJobs_.load(std::memory_order_relaxed),
        deltaJobs_.load(std::memory_order_relaxed),
        coalescedHandles_.load(std::memory_order_relaxed),
        sorts_.load(std::memory_order_relaxed),
        indexBuiltOnWorker_.load(std::memory_order_relaxed),
        publicationUs_.load(std::memory_order_relaxed),
        vectorCloneCount_.load(std::memory_order_relaxed),
        cloneRecords_.load(std::memory_order_relaxed),
        requestsRejectedByLifetime_.load(std::memory_order_relaxed),
    };
}

std::int64_t LibraryModel::diagnosticStaleDescriptions() noexcept
{
    try {
        const auto snapshot = sharedSnapshot();
        if (!snapshot || !snapshot->tracks || !scripts_) return -1;
        std::int64_t stale = 0;
        for (const auto& cached : *snapshot->tracks) {
            // Independent reference path: preserve the fifteen original scripts
            // to catch capture, Unicode, multivalue and provider regressions.
            const auto current = describeTrackWithScripts(cached.handle, *scripts_, false);
            stale += !sameLibraryLayout(cached, current) ||
                cached.rating != current.rating || cached.playCount != current.playCount;
        }
        return stale;
    } catch (...) { return -1; }
}

std::uint64_t LibraryModel::diagnosticTitleformatCalls() const noexcept
{ return titleformatCalls.load(std::memory_order_relaxed); }

void LibraryModel::recordIndexedNowPlayingHit() noexcept
{
    indexedNowPlayingHits_.fetch_add(1, std::memory_order_relaxed);
}

void LibraryModel::recordNowPlayingLinearScan() noexcept
{
    nowPlayingLinearScans_.fetch_add(1, std::memory_order_relaxed);
}

LibraryTrack LibraryModel::describeTrack(const metadb_handle_ptr& handle)
{
    const auto scripts = scripts_;
    return scripts ? describeTrackWithScripts(handle, *scripts) : LibraryTrack{};
}

void LibraryModel::compileScripts()
{
    if (scripts_ && scripts_->size() == fieldCount + 1) return;
    auto scripts = std::make_shared<std::vector<titleformat_object::ptr>>();
    scripts->resize(fieldCount + 1);
    auto compiler = titleformat_compiler::get();
    for (std::size_t index = 0; index < fieldCount; ++index) {
        compiler->compile_safe((*scripts)[index], kFieldScripts[index]);
    }
    std::string combined;
    for (std::size_t index = 0; index < fieldCount; ++index)
        combined += "$goden_library_field(" + std::to_string(index) + "," +
            kFieldScripts[index] + ")";
    // Values are captured through function parameters, never split on a
    // delimiter that album/artist/title metadata could itself contain.
    compiler->compile((*scripts)[fieldCount], combined.c_str());
    scripts_ = std::move(scripts);
}

std::wstring LibraryModel::format(
    const metadb_handle_ptr& handle, std::size_t field) const
{
    pfc::string8_fast output;
    const auto scripts = scripts_;
    if (!scripts || field >= scripts->size() ||
        !(*scripts)[field].is_valid() ||
        !handle->format_title(nullptr, output, (*scripts)[field], nullptr)) {
        return {};
    }
    return trimText(utf8ToWide(output.c_str()));
}

void LibraryModel::requestRefresh()
{
    if (!started_.load(std::memory_order_acquire) || refreshScheduled_) return;
    refreshScheduled_ = true;
    const auto lifetime = refreshLifetime_;
    // Always queued by the SDK, even when called from its main thread. Paint
    // reads an immutable snapshot; it never enumerates the host library.
    fb2k::inMainThread([this, lifetime] {
        if (lifetime != refreshLifetime_ ||
                !started_.load(std::memory_order_acquire)) return;
        refreshScheduled_ = false;
        scheduleRefresh();
    });
}

void LibraryModel::scheduleRefresh()
{
    if (!started_.load(std::memory_order_acquire)) return;
    auto job = changeInbox_.take();
    if (!job) {
        if (changeInbox_.active() && changeInbox_.fullPending())
            supersessionEpoch_.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    RefreshRequest request;
    request.supersessionEpoch = supersessionEpoch_.load(std::memory_order_acquire);
    request.job = std::move(*job);
    request.scripts = scripts_;
    request.base = sharedSnapshot_.load(std::memory_order_acquire);
    request.forceDescriptions = std::move(forceDescriptions_);
    forceDescriptions_.clear();
    request.forceAllDescriptions = std::exchange(forceAllDescriptions_, false);
    if (request.job.full) {
        metadb_handle_list items;
        library_manager::get()->get_all_items(items);
        request.job.handles.reserve(items.get_count());
        for (t_size index = 0; index < items.get_count(); ++index)
            request.job.handles.push_back(items[index]);
    }
    refreshPending_.store(true, std::memory_order_release);
    dirty_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        if (stopping_) return;
        pendingRefresh_ = std::move(request);
    }
    workerChanged_.notify_one();
}

void LibraryModel::workerMain() noexcept
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        RefreshRequest request;
        {
            std::unique_lock<std::mutex> lock(workerMutex_);
            workerChanged_.wait(lock, [this] {
                return stopping_ || pendingRefresh_.has_value();
            });
            if (stopping_) break;
            request = std::move(*pendingRefresh_);
            pendingRefresh_.reset();
        }

        if (!started_.load(std::memory_order_acquire)) return;

        const bool full = request.job.full;
        bool superseded = false;
        const auto cancelled = [&] {
            superseded = superseded || request.supersessionEpoch !=
                supersessionEpoch_.load(std::memory_order_acquire);
            return superseded;
        };
        if (full) fullJobs_.fetch_add(1, std::memory_order_relaxed);
        else deltaJobs_.fetch_add(1, std::memory_order_relaxed);
        bool success = request.scripts != nullptr &&
            (full || (request.base && request.base->tracks));
        auto next = !full &&
                request.base && request.base->tracks
            ? std::make_shared<std::vector<LibraryTrack>>(
                *request.base->tracks)
            : std::make_shared<std::vector<LibraryTrack>>();
        if (!full && request.base && request.base->tracks) {
            vectorCloneCount_.fetch_add(1, std::memory_order_relaxed);
            cloneRecords_.fetch_add(request.base->tracks->size(),
                std::memory_order_relaxed);
            std::uint64_t bytes=request.base->tracks->size()*sizeof(LibraryTrack);
            for(const auto& t:*request.base->tracks){
                if (!started_.load(std::memory_order_acquire)) return;
                if (cancelled()) break;
                for(const auto* s:{&t.genre,&t.artist,&t.albumArtist,&t.album,&t.title,
                        &t.year,&t.codec,&t.releaseId,&t.path,&t.searchFolded})bytes+=s->size()*sizeof(wchar_t);
                for(const auto& s:t.albumArtists)bytes+=sizeof(s)+s.size()*sizeof(wchar_t);
            }
            publicationDeepCopyBytes_.fetch_add(bytes,std::memory_order_relaxed);
        }
        if (full) next->reserve(request.job.handles.size());
        bool sortNeeded = full;
        bool layoutChanged = full;
        for (const auto& handle : request.job.handles) {
            if (!started_.load(std::memory_order_acquire)) return;
            if (cancelled()) break;
            if (!success) break;
            const auto key = handleKey(handle);
            metadb_info_container::ptr info, browse;
            handle->get_browse_info_ref(info, browse);
            const LibraryTrack* cached = nullptr;
            if (request.base && request.base->tracks) {
                const auto found = request.base->handleIndex.find(key);
                if (found != request.base->handleIndex.end() &&
                    found->second < request.base->tracks->size())
                    cached = &(*request.base->tracks)[found->second];
            }
            const bool reuse = cached && info.is_valid() &&
                !request.forceAllDescriptions && !request.forceDescriptions.contains(key) &&
                sameDescriptionInfo(cached->descriptionInfo, info) &&
                sameDescriptionInfo(cached->descriptionBrowse, browse);
            auto described = reuse ? *cached : describeTrackWithScripts(handle, *request.scripts);
            if (!reuse) describedTracks_.fetch_add(1, std::memory_order_relaxed);
            // Keep the pre-format identities. A concurrent metadata replacement
            // cannot be mistaken for the snapshot actually used above.
            described.descriptionInfo = std::move(info);
            described.descriptionBrowse = std::move(browse);
            if (full) {
                next->push_back(std::move(described));
            } else {
                const auto found = request.base->handleIndex.find(key);
                if (found != request.base->handleIndex.end() &&
                        found->second < next->size()) {
                    const auto& previous = (*next)[found->second];
                    layoutChanged=layoutChanged||!sameLibraryLayout(previous,described);
                    sortNeeded = sortNeeded || trackLess(previous, described) ||
                        trackLess(described, previous);
                    (*next)[found->second] = std::move(described);
                } else {
                    next->push_back(std::move(described));
                    sortNeeded = true;
                    layoutChanged = true;
                }
            }
        }
        if (!started_.load(std::memory_order_acquire)) return;
        if (success && !cancelled() && sortNeeded) {
            std::stable_sort(next->begin(), next->end(), trackLess);
            sorts_.fetch_add(1, std::memory_order_relaxed);
        }
        if (!started_.load(std::memory_order_acquire)) return;
        auto published = std::make_shared<LibrarySnapshot>();
        if (success && !cancelled()) {
            published->layoutRevision=(request.base?request.base->layoutRevision:0)+(layoutChanged?1U:0U);
            published->tracks =
                std::shared_ptr<const std::vector<LibraryTrack>>(next);
            published->handleIndex.reserve(next->size());
            for (std::size_t index = 0; index < next->size(); ++index) {
                if (!started_.load(std::memory_order_acquire)) return;
                if (cancelled()) break;
                const auto key = handleKey((*next)[index].handle);
                if (!key.empty())
                    published->handleIndex.emplace(key, index);
            }
            indexBuiltOnWorker_.fetch_add(1, std::memory_order_relaxed);
        }
        if (!started_.load(std::memory_order_acquire)) return;
        fb2k::inMainThread([this, job = request.job, full,
                success, superseded, published, forceAll = request.forceAllDescriptions,
                forced = std::move(request.forceDescriptions)] {
            const auto started = std::chrono::steady_clock::now();
            const auto completion = changeInbox_.complete(job,
                success && started_.load(std::memory_order_acquire), superseded);
            if (!completion.recognized) {
                requestsRejectedByLifetime_.fetch_add(
                    1, std::memory_order_relaxed);
                return;
            }
            refreshPending_.store(false, std::memory_order_release);
            if (!completion.publish) {
                // Membership reconciliation may supersede an in-flight stats
                // update. Its display-provider invalidations still belong to
                // the next authoritative snapshot, even if host info is equal.
                if (forceAll || forced.size() + forceDescriptions_.size() > 4096) {
                    forceAllDescriptions_ = true;
                    forceDescriptions_.clear();
                } else if (!forceAllDescriptions_) {
                    forceDescriptions_.insert(forced.begin(), forced.end());
                }
            }
            if (completion.publish) {
                published->generation =
                    generation_.fetch_add(1, std::memory_order_acq_rel) + 1U;
                tracksView_ = published->tracks;
                const auto immutable =
                    std::static_pointer_cast<const LibrarySnapshot>(published);
                sharedSnapshot_.store(immutable, std::memory_order_release);
                if (full) fullRebuilds_.fetch_add(1, std::memory_order_relaxed);
                else deltaRebuilds_.fetch_add(1, std::memory_order_relaxed);
                AlbumSessionService::instance().onLibrarySnapshotPublished(
                    immutable);
                notifyWorkspaceModelChanged();
            }
            dirty_.store(completion.more || !success,
                std::memory_order_release);
            const auto elapsed = std::chrono::duration_cast<
                std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started).count();
            publicationUs_.fetch_add(static_cast<std::uint64_t>(
                (std::max<std::int64_t>)(0, elapsed)),
                std::memory_order_relaxed);
            if (completion.more && (success || superseded)) requestRefresh();
        });
    }
}

} // namespace nekorox::component
