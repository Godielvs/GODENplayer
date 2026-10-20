#include "album_session_service.h"
#include "telemetry_span.h"

#include "database_service.h"
#include "file_open_service.h"
#include "player_shell_panel.h"
#include "text_util.h"

#include <nekorox/album_session.h>
#include <nekorox/track_identity.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace nekorox::component {
namespace {

constexpr char kPlaylistName[] = "GODENplayer Album Session";
const GUID kPlaylistConfigGuid = {
    0x38c4ba75, 0xa86a, 0x4ae8,
    {0xb8, 0xcb, 0x47, 0xa8, 0x23, 0xec, 0xc2, 0x6b}};
cfg_guid g_albumSessionPlaylistGuid(kPlaylistConfigGuid, GUID_NULL);
std::atomic<std::uint64_t> g_nextSession{1};

std::string handleKey(const metadb_handle_ptr& track)
{
    if (!track.is_valid()) return {};
    std::string key(track->get_path());
    key.push_back('\x1f');
    key += std::to_string(track->get_subsong_index());
    return key;
}

std::string revisionId(const LibraryTrack& track)
{
    try {
        const auto native = track.handle.is_valid()
            ? filesystem::g_get_native_path(track.handle->get_path())
            : pfc::string8{};
        const std::filesystem::path path(track.handle.is_valid()
            ? utf8ToWide(native.c_str()) : track.path);
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        const auto safeSize = error ? 0U : size;
        error.clear();
        const auto modified = std::filesystem::last_write_time(path, error);
        const auto ticks = error ? 0U : static_cast<std::uint64_t>(
            modified.time_since_epoch().count());
        return nekorox::makeTrackId({path,
            track.handle.is_valid() ? track.handle->get_subsong_index() : 0U,
            safeSize, ticks}).value;
    } catch (...) {
        return {};
    }
}

std::wstring sessionPath(const LibraryTrack& track)
{
    if (!track.handle.is_valid()) return track.path;
    try {
        const auto native = filesystem::g_get_native_path(
            track.handle->get_path());
        return utf8ToWide(native.c_str());
    } catch (...) {
        return track.path;
    }
}

bool libraryContains(const std::shared_ptr<const LibrarySnapshot>& library,
    const metadb_handle_ptr& anchor)
{
    return library && anchor.is_valid() &&
        library->handleIndex.contains(handleKey(anchor));
}

} // namespace

struct AlbumSessionService::Observer final : playlist_callback_impl_base,
        message_filter_impl_base {
    Observer()
        : playlist_callback_impl_base(
            playlist_callback::flag_item_ops |
            playlist_callback::flag_on_playlists_removed |
            playlist_callback::flag_on_playback_order_changed) {}

    void on_items_added(t_size playlist, t_size,
        metadb_handle_list_cref, const bit_array&) override
    {
        AlbumSessionService::instance().markOwnedPlaylistModified(playlist);
    }

    void on_items_reordered(t_size playlist, const t_size*, t_size) override
    {
        AlbumSessionService::instance().markOwnedPlaylistModified(playlist);
    }

    void on_items_removed(
        t_size playlist, const bit_array&, t_size, t_size) override
    {
        AlbumSessionService::instance().markOwnedPlaylistModified(playlist);
    }

    void on_items_replaced(t_size playlist, const bit_array&,
        const pfc::list_base_const_t<
            playlist_callback::t_on_items_replaced_entry>&) override
    {
        AlbumSessionService::instance().markOwnedPlaylistModified(playlist);
    }

    void on_playlists_removed(
        const bit_array&, t_size, t_size) override
    {
        AlbumSessionService::instance().validatePlaylistLease();
    }

    void on_playback_order_changed(t_size) override
    {
        AlbumSessionService::instance().revokePlaylistLease();
    }

    bool pretranslate_message(MSG* message) override
    {
        if (message == nullptr || message->message != WM_KEYDOWN ||
            message->wParam != 'I' ||
            (GetKeyState(VK_CONTROL) & 0x8000) == 0 ||
            (GetKeyState(VK_SHIFT) & 0x8000) == 0 ||
            keyboard_shortcut_manager::is_typing_message(message)) {
            return false;
        }
        AlbumSessionService::instance().toggleFromHostContext();
        return true;
    }
};

AlbumSessionService::AlbumSessionService()
{
    snapshot_.store(std::make_shared<AlbumSessionSnapshot>(),
        std::memory_order_release);
}

AlbumSessionService::~AlbumSessionService()
{
    requestId_.fetch_add(1,std::memory_order_acq_rel);
    identityWorker_->shutdownAndDrain();
    if (worker_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(workerMutex_);
            stopping_ = true;
            pendingRequest_.reset();
            waitingLibraryRequest_.reset();
        }
        workerChanged_.notify_one();
        worker_.join();
    }
}

AlbumSessionService& AlbumSessionService::instance()
{
    static AlbumSessionService value;
    return value;
}

void AlbumSessionService::start()
{
    if(identityStopped_){identityWorker_=std::make_unique<LatestWorker>();identityStopped_=false;}
    if (!observer_) observer_ = std::make_unique<Observer>();
    if (!worker_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(workerMutex_);
            stopping_ = false;
            pendingRequest_.reset();
            waitingLibraryRequest_.reset();
        }
        worker_ = std::thread(&AlbumSessionService::workerMain, this);
    }
}

void AlbumSessionService::stop()
{
    requestId_.fetch_add(1, std::memory_order_acq_rel);
    cancelContinuation();
    identityWorker_->shutdownAndDrain();identityStopped_=true;
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        stopping_ = true;
        pendingRequest_.reset();
        waitingLibraryRequest_.reset();
    }
    workerChanged_.notify_one();
    if (worker_.joinable()) worker_.join();
    observer_.reset();
    close();
}

std::shared_ptr<const AlbumSessionSnapshot>
AlbumSessionService::snapshot() const
{
    return snapshot_.load(std::memory_order_acquire);
}

void AlbumSessionService::publish(
    std::shared_ptr<AlbumSessionSnapshot> next)
{
    snapshot_.store(std::move(next), std::memory_order_release);
    notifyPlayerShellsChanged();
}

bool AlbumSessionService::enterForTrack(const metadb_handle_ptr& anchor)
{
    TelemetrySpan span(TelemetrySurface::album,102);
    if (!anchor.is_valid()) return false;
    if (const auto current = snapshot(); current &&
            current->anchor.is_valid() && current->anchor == anchor &&
            current->state != AlbumSessionState::error) {
        return true;
    }
    if (const auto current = snapshot(); current &&
            current->state == AlbumSessionState::ready &&
            std::any_of(current->tracks.begin(), current->tracks.end(),
                [&](const AlbumSessionTrack& track) {
                    return track.handle.is_valid() && track.handle == anchor;
                })) {
        return true;
    }
    const auto requestId = requestId_.fetch_add(1,
        std::memory_order_acq_rel) + 1U;
    cancelContinuation();
    identityWorker_->cancelPending();
    auto loading = std::make_shared<AlbumSessionSnapshot>();
    loading->requestId = requestId;
    loading->sessionId = g_nextSession.fetch_add(1,
        std::memory_order_relaxed);
    loading->revision = 1;
    loading->state = AlbumSessionState::loading;
    loading->anchor = anchor;
    loading->queueExternal = playlist_manager::get()->queue_get_count() != 0;
    const auto library = LibraryModel::instance().sharedSnapshot();
    metadb_handle_list temporaryItems;
    bool isTemporary=false;
    if (temporaryAlbumItems(temporaryItems)) {
        bool belongs = false;
        for (t_size index = 0; index < temporaryItems.get_count(); ++index) {
            if (temporaryItems[index] == anchor) {
                belongs = true;
                break;
            }
        }
        isTemporary=belongs;
        if(!belongs) temporaryItems.remove_all();
    }
    const auto expectedLibrary = !isTemporary &&
        library_manager::get()->is_item_in_library(anchor);
    loading->sourceRevision = library ? library->generation : 0;
    loading->sourceKind = isTemporary
        ? AlbumSessionSourceKind::temporary
        : expectedLibrary ? AlbumSessionSourceKind::library :
        (library &&
            library->handleIndex.contains(handleKey(anchor))
        ? AlbumSessionSourceKind::library
        : (std::string_view(anchor->get_path()).find("://") !=
                std::string_view::npos
            ? AlbumSessionSourceKind::remote
            : AlbumSessionSourceKind::external));
    loading->ownedPlaylistValid = ownedPlaylist() != pfc_infinite;
    loading->playlistLeaseValid = loading->ownedPlaylistValid;
    publish(std::move(loading));
    ResolveRequest request;
    request.requestId = requestId;
    request.anchor = anchor;
    request.library = library;
    request.temporaryItems = std::move(temporaryItems);
    request.temporaryGeneration = temporaryAlbumGeneration();
    request.libraryDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    request.expectedLibrary = expectedLibrary;
    enqueue(std::move(request));
    return true;
}

void AlbumSessionService::enqueue(ResolveRequest request)
{
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        if (stopping_) return;
        if (request.requestId != requestId_.load(std::memory_order_acquire)) return;
        request.resolutionTicket = resolutionTicket_.fetch_add(1, std::memory_order_acq_rel) + 1;
        pendingRequest_.reset();
        waitingLibraryRequest_.reset();
        if (request.expectedLibrary &&
            !libraryContains(request.library, request.anchor)) {
            waitingLibraryRequest_ = std::move(request);
            workerChanged_.notify_one();
            return;
        }
        pendingRequest_ = std::move(request);
    }
    workerChanged_.notify_one();
}

void AlbumSessionService::onLibrarySnapshotPublished(
    std::shared_ptr<const LibrarySnapshot> library)
{
    const auto current=snapshot();
    if(current&&current->state==AlbumSessionState::ready&&current->sourceKind==AlbumSessionSourceKind::library&&
        library&&current->sourceLayoutRevision!=library->layoutRevision&&current->anchor.is_valid()){
        ResolveRequest request;request.requestId=current->requestId;request.anchor=current->anchor;request.library=library;
        request.expectedLibrary=libraryContains(library,current->anchor);
        request.libraryDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        enqueue(std::move(request));return;
    }
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        if (!waitingLibraryRequest_ || stopping_ ||
            waitingLibraryRequest_->requestId != requestId_.load(std::memory_order_acquire)) return;
        waitingLibraryRequest_->library = std::move(library);
        if (libraryContains(waitingLibraryRequest_->library,
                waitingLibraryRequest_->anchor)) {
            pendingRequest_ = std::move(*waitingLibraryRequest_);
            waitingLibraryRequest_.reset();
        }
    }
    workerChanged_.notify_one();
}

void AlbumSessionService::workerMain() noexcept
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        ResolveRequest request;
        {
            std::unique_lock<std::mutex> lock(workerMutex_);
            while (!stopping_ && !pendingRequest_) {
                if (!waitingLibraryRequest_) {
                    workerChanged_.wait(lock, [this] {
                        return stopping_ || pendingRequest_.has_value() ||
                            waitingLibraryRequest_.has_value();
                    });
                    continue;
                }
                const auto deadline = waitingLibraryRequest_->libraryDeadline;
                const auto waitingId = waitingLibraryRequest_->requestId;
                const auto waitingTicket = waitingLibraryRequest_->resolutionTicket;
                if (workerChanged_.wait_until(lock, deadline, [this, waitingId, waitingTicket] {
                        return stopping_ || pendingRequest_.has_value() ||
                            !waitingLibraryRequest_ || waitingLibraryRequest_->requestId != waitingId ||
                            waitingLibraryRequest_->resolutionTicket != waitingTicket;
                    })) continue;
                auto expired = std::move(*waitingLibraryRequest_);
                waitingLibraryRequest_.reset();
                expired.libraryDeadlineExpired = true;
                pendingRequest_ = std::move(expired);
            }
            if (stopping_) break;
            request = std::move(*pendingRequest_);
            pendingRequest_.reset();
        }
        const auto currentResolution = [&] {
            return request.requestId == requestId_.load(std::memory_order_acquire) &&
                request.resolutionTicket == resolutionTicket_.load(std::memory_order_acquire);
        };
        if (!currentResolution()) {
            continue;
        }

        // Playlist ownership is captured on UI; immutable handles and the
        // published titleformat scripts can be described on this worker, as
        // already used for external anchors. Superseded work stops per item.
        request.temporaryTracks.reserve(request.temporaryItems.get_count());
        for(t_size i=0;i<request.temporaryItems.get_count();++i){
            if(!currentResolution()) break;
            request.temporaryTracks.push_back(LibraryModel::instance().describeTrack(request.temporaryItems[i]));
        }
        if(!currentResolution()) continue;
        auto resolvedSnapshot = std::make_shared<AlbumSessionSnapshot>();
        resolvedSnapshot->requestId = request.requestId;
        resolvedSnapshot->state = AlbumSessionState::error;
        resolvedSnapshot->error = L"Album could not be resolved";
        resolvedSnapshot->sourceRevision = request.library
            ? request.library->generation : 0;
        resolvedSnapshot->sourceLayoutRevision=request.library?request.library->layoutRevision:0;

        if (request.expectedLibrary && request.libraryDeadlineExpired) {
            resolvedSnapshot->sourceKind = AlbumSessionSourceKind::library;
            resolvedSnapshot->error =
                L"Library album is still loading; try again";
            if (currentResolution()) {
                fb2k::inMainThread([this, requestId = request.requestId, resolutionTicket = request.resolutionTicket,
                        resolvedSnapshot] {
                    publishResolved(requestId, resolutionTicket, resolvedSnapshot);
                });
            }
            continue;
        }

        const auto anchorKey = handleKey(request.anchor);
        const auto found = request.library
            ? request.library->handleIndex.find(anchorKey)
            : std::unordered_map<std::string, std::size_t>::const_iterator{};
        const auto inLibrary = request.library &&
            found != request.library->handleIndex.end() &&
            request.library->tracks;
        if (!request.temporaryTracks.empty()) {
            const auto& tracks = request.temporaryTracks;
            const auto anchor = std::find_if(tracks.begin(), tracks.end(),
                [&](const auto& track) { return track.handle == request.anchor; });
            const auto anchorIndex = anchor == tracks.end() ? 0U :
                static_cast<std::size_t>(std::distance(tracks.begin(), anchor));
            resolvedSnapshot->state = AlbumSessionState::ready;
            resolvedSnapshot->error.clear();
            resolvedSnapshot->sourceKind = AlbumSessionSourceKind::temporary;
            resolvedSnapshot->sourceRevision = request.temporaryGeneration;
            resolvedSnapshot->album = tracks[anchorIndex].album;
            resolvedSnapshot->albumArtist = tracks[anchorIndex].albumArtist;
            resolvedSnapshot->year = tracks[anchorIndex].year;
            resolvedSnapshot->stableArtworkAnchor =
                sessionPath(tracks[anchorIndex]);
            resolvedSnapshot->albumKey = L"temporary:" +
                std::to_wstring(request.temporaryGeneration);
            resolvedSnapshot->tracks.reserve(tracks.size());
            for (std::size_t index = 0; index < tracks.size(); ++index) {
                if(!currentResolution()) break;
                const auto& source = tracks[index];
                resolvedSnapshot->tracks.push_back({source.handle,
                    source.title, source.artist, {}, {},
                    source.disc, source.track,
                    static_cast<std::uint32_t>(source.handle.is_valid()
                        ? source.handle->get_subsong_index() : 0U)});
            }
            resolvedSnapshot->currentInSession = anchorIndex;
        } else if (inLibrary) {
            const auto& library = *request.library->tracks;
            const auto anchorIndex = found->second;
            std::vector<nekorox::AlbumSessionCandidate> candidates;
            candidates.reserve(library.size());
            for (std::size_t index = 0; index < library.size(); ++index) {
                if(!currentResolution()) break;
                const auto& track = library[index];
                candidates.push_back({track.albumArtist, track.album,
                    track.year, track.releaseId, sessionPath(track), {}, {},
                    track.disc, track.track,
                    static_cast<std::uint32_t>(track.handle.is_valid()
                        ? track.handle->get_subsong_index() : 0U), index});
            }
            if(!currentResolution()) continue;
            const auto selected =
                nekorox::resolveAlbumSession(candidates, anchorIndex);
            if (!selected.empty()) {
                resolvedSnapshot->state = AlbumSessionState::ready;
                resolvedSnapshot->error.clear();
                resolvedSnapshot->sourceKind = AlbumSessionSourceKind::library;
                resolvedSnapshot->album = library[anchorIndex].album;
                resolvedSnapshot->albumArtist =
                    library[anchorIndex].albumArtist;
                resolvedSnapshot->year = nekorox::albumSessionYear(candidates, selected);
                resolvedSnapshot->stableArtworkAnchor =
                    sessionPath(library[anchorIndex]);
                resolvedSnapshot->albumKey =
                    !library[anchorIndex].releaseId.empty()
                    ? L"release:" + library[anchorIndex].releaseId
                    : resolvedSnapshot->albumArtist + L"\x1f" +
                        resolvedSnapshot->album + L"\x1f" +
                        nekorox::albumEditionRoot(
                            resolvedSnapshot->stableArtworkAnchor);
                for (const auto candidateIndex : selected) {
                    if (!currentResolution()) break;
                    const auto sourceIndex = candidates[candidateIndex].sourceIndex;
                    const auto& source = library[sourceIndex];
                    resolvedSnapshot->tracks.push_back({source.handle,
                        source.title, source.artist, {}, {},
                        source.disc, source.track,
                        candidates[candidateIndex].subsong});
                    if (source.handle == request.anchor) {
                        resolvedSnapshot->currentInSession =
                            resolvedSnapshot->tracks.size() - 1U;
                    }
                }
            }
        } else {
            // External/remote anchors stay bounded to the authorized item.
            // No parent-drive or library-wide scan occurs here.
            const auto described =
                LibraryModel::instance().describeTrack(request.anchor);
            resolvedSnapshot->state = AlbumSessionState::ready;
            resolvedSnapshot->error.clear();
            resolvedSnapshot->sourceKind =
                std::string_view(request.anchor->get_path()).find("://") !=
                    std::string_view::npos
                ? AlbumSessionSourceKind::remote
                : AlbumSessionSourceKind::external;
            resolvedSnapshot->album = described.album;
            resolvedSnapshot->albumArtist = described.albumArtist;
            resolvedSnapshot->year = described.year;
            resolvedSnapshot->stableArtworkAnchor = sessionPath(described);
            resolvedSnapshot->albumKey = L"external:" +
                resolvedSnapshot->stableArtworkAnchor;
            resolvedSnapshot->tracks.push_back({request.anchor,
                described.title, described.artist, {}, {},
                described.disc, described.track,
                request.anchor->get_subsong_index()});
            resolvedSnapshot->currentInSession = 0;
        }
        if (!currentResolution()) {
            continue;
        }
        fb2k::inMainThread([this, requestId = request.requestId, resolutionTicket = request.resolutionTicket,
                resolvedSnapshot] {
            publishResolved(requestId, resolutionTicket, resolvedSnapshot);
        });
    }
}

void AlbumSessionService::publishResolved(
    std::uint64_t requestId, std::uint64_t resolutionTicket,
    std::shared_ptr<AlbumSessionSnapshot> resolved)
{
    const auto current = snapshot();
    if (!current || current->requestId != requestId ||
        requestId != requestId_.load(std::memory_order_acquire) ||
        resolutionTicket != resolutionTicket_.load(std::memory_order_acquire)) return;
    if(resolved->sourceKind==AlbumSessionSourceKind::library){
        const auto library=LibraryModel::instance().sharedSnapshot();
        if(library&&library->layoutRevision!=resolved->sourceLayoutRevision){
            ResolveRequest newer;newer.requestId=requestId;newer.anchor=current->anchor;newer.library=library;
            newer.expectedLibrary=libraryContains(library,current->anchor);
            newer.libraryDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            enqueue(std::move(newer));return;
        }
    }
    resolved->sessionId = current->sessionId;
    resolved->anchor = current->anchor;
    resolved->revision = current->revision + 1U;
    resolved->queueExternal = playlist_manager::get()->queue_get_count() != 0;
    resolved->ownedPlaylistValid = ownedPlaylist() != pfc_infinite;
    resolved->playlistLeaseValid = resolved->ownedPlaylistValid;
    resolved->transportOrderLease = current->transportOrderLease;
    resolved->compositionRevision = current->compositionRevision + 1U;
    metadb_handle_ptr playing;
    if (playback_control::get()->get_now_playing(playing) &&
        playing.is_valid()) {
        for (std::size_t index = 0; index < resolved->tracks.size(); ++index) {
            if (resolved->tracks[index].handle == playing) {
                resolved->actualPlayingIndex = index;
                resolved->currentInSession = index;
                break;
            }
        }
        resolved->outsideSession = !resolved->actualPlayingIndex.has_value();
    }
    resolved->policy = current->policy;
    if (resolved->policy.seed == 0) {
        resolved->policy.seed = resolved->sessionId ^ 0x9e3779b97f4a7c15ULL;
    }
    resolved->permutation = nekorox::makeAlbumPermutation(
        resolved->tracks.size(), resolved->currentInSession,
        resolved->policy);
    // Playable metadata never waits for file fingerprinting. Identity retains
    // its content-sensitive format and is enriched on a separate cancellable worker.
    const auto identitySource = resolved;
    publish(std::move(resolved));
    if (identitySource->state != AlbumSessionState::ready) return;
    (void)identityWorker_->submit([this, identitySource, requestId](std::stop_token stop) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        std::vector<std::string> ids;
        ids.reserve(identitySource->tracks.size());
        for (const auto& track : identitySource->tracks) {
            if (stop.stop_requested()) return;
            LibraryTrack source; source.handle = track.handle;
            ids.push_back(revisionId(source));
        }
        if (stop.stop_requested()) return;
        fb2k::inMainThread([this, identitySource, requestId, ids = std::move(ids)]() mutable {
            adoptRevisionIds(identitySource, requestId, std::move(ids));
        });
    });
}

void AlbumSessionService::adoptRevisionIds(
    const std::shared_ptr<const AlbumSessionSnapshot>& identitySource,
    std::uint64_t requestId, std::vector<std::string> ids)
{
    const auto current = snapshot();
    if (!current || current->requestId != requestId ||
        requestId != requestId_.load(std::memory_order_acquire) ||
        current->sessionId != identitySource->sessionId ||
        current->compositionRevision != identitySource->compositionRevision ||
        current->tracks.size() != identitySource->tracks.size() ||
        ids.size() != current->tracks.size()) return;
    auto next = std::make_shared<AlbumSessionSnapshot>(*current);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (current->tracks[i].handle != identitySource->tracks[i].handle) return;
        next->tracks[i].revisionTrackId = ids[i];
    }
    ++next->revision;
    const std::shared_ptr<const AlbumSessionSnapshot> identityBaseline = next;
    publish(std::move(next));
    // The DB may have finished before fingerprinting. Reconcile that order too.
    refreshPlayingIdentity();
    DatabaseService::instance().resolveTrackIdentities(std::move(ids),
        [sessionId = identitySource->sessionId, requestId,
            composition = identitySource->compositionRevision,
            baseline = identityBaseline](TrackIdentityBatchPayload payload) {
            if (!payload.error.empty()) return;
            const auto& identities = payload.identities;
            auto& service = AlbumSessionService::instance();
            const auto current = service.snapshot();
            if (!current || current->sessionId != sessionId ||
                current->requestId != requestId ||
                current->compositionRevision != composition ||
                identities.size() != current->tracks.size()) return;
            bool changed = false;
            for (std::size_t index = 0; index < identities.size(); ++index) {
                if (identities[index].revisionTrackId !=
                    current->tracks[index].revisionTrackId) return;
                changed = changed || (current->tracks[index].entityId ==
                    baseline->tracks[index].entityId &&
                    identities[index].entityId != current->tracks[index].entityId);
            }
            if (!changed) return;
            auto next = std::make_shared<AlbumSessionSnapshot>(*current);
            for (std::size_t index = 0; index < identities.size(); ++index) {
                // A late missing/old lookup must not undo a newer live binding.
                if (current->tracks[index].entityId == baseline->tracks[index].entityId)
                    next->tracks[index].entityId = identities[index].entityId;
            }
            ++next->revision;
            service.publish(std::move(next));
        });
}

void AlbumSessionService::refreshPlayingIdentity()
{
    if (identityStopped_) return;
    const auto user = DatabaseService::instance().userStateSnapshot();
    if (!user || !user->available || user->stale ||
        user->entityId.empty() || user->revisionTrackId.empty()) return;
    const auto current = snapshot();
    if (!current || current->state != AlbumSessionState::ready ||
        current->outsideSession || !current->actualPlayingIndex ||
        *current->actualPlayingIndex >= current->tracks.size()) return;
    const auto& row = current->tracks[*current->actualPlayingIndex];
    metadb_handle_ptr playing;
    if (!playback_control::get()->get_now_playing(playing) || !playing.is_valid() ||
        row.handle != playing || row.revisionTrackId != user->revisionTrackId) return;
    const auto changed = std::any_of(current->tracks.begin(), current->tracks.end(),
        [&](const auto& track) { return track.revisionTrackId == user->revisionTrackId &&
            track.entityId != user->entityId; });
    if (!changed) return;
    auto next = std::make_shared<AlbumSessionSnapshot>(*current);
    for (auto& track : next->tracks) {
        if (track.revisionTrackId == user->revisionTrackId) track.entityId = user->entityId;
    }
    ++next->revision;
    publish(std::move(next));
}

bool AlbumSessionService::enterFromHostContext()
{
    metadb_handle_ptr anchor;
    if (!playback_control::get()->get_now_playing(anchor) ||
        !anchor.is_valid()) {
        if (!playlist_manager::get()->activeplaylist_get_focus_item_handle(
                anchor) || !anchor.is_valid()) {
            return false;
        }
    }
    return enterForTrack(anchor);
}

bool AlbumSessionService::retry()
{
    const auto current=snapshot();
    if(current && current->state==AlbumSessionState::error && current->anchor.is_valid())
        return enterForTrack(current->anchor);
    return enterFromHostContext();
}

void AlbumSessionService::toggleFromHostContext()
{
    if (playerShellMode() == PlayerShellMode::album) {
        setPlayerShellMode(PlayerShellMode::normal);
        return;
    }
    // Entering the visual mode is deliberately read-only. Failure to resolve
    // a track still opens the empty album surface and never starts playback.
    (void)enterFromHostContext();
    setPlayerShellMode(PlayerShellMode::album);
}

void AlbumSessionService::close()
{
    cancelContinuation();
    identityWorker_->cancelPending();
    requestId_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        pendingRequest_.reset();
        waitingLibraryRequest_.reset();
    }
    preparationLease_.clear();
    publish(std::make_shared<AlbumSessionSnapshot>());
}

std::size_t AlbumSessionService::ownedPlaylist() const noexcept
{
    const auto guid = g_albumSessionPlaylistGuid.get();
    if (InlineIsEqualGUID(guid, GUID_NULL)) return pfc_infinite;
    try {
        return playlist_manager_v5::get()->find_playlist_by_guid(guid);
    } catch (...) {
        return pfc_infinite;
    }
}

bool AlbumSessionService::playTrack(std::size_t index)
{
    cancelContinuation();
    const auto state = snapshot();
    if (!state || state->state != AlbumSessionState::ready ||
        index >= state->tracks.size()) return false;
    std::size_t playlist = pfc_infinite;
    if (!ensurePreparedPlaylist(*state, playlist)) return false;
    auto playlists = playlist_manager::get();
    playlists->set_active_playlist(playlist);
    playlists->playlist_clear_selection(playlist);
    playlists->playlist_set_selection_single(playlist, index, true);
    playlists->playlist_set_focus_item(playlist, index);
    const auto played = playlists->playlist_execute_default_action(playlist, index);
    if (played) {
        const auto latest = snapshot();
        if (latest && latest->sessionId == state->sessionId) {
            auto next = std::make_shared<AlbumSessionSnapshot>(*latest);
            next->ownedPlaylistValid = true;
            next->playlistLeaseValid = true;
            next->transportOrderLease = true;
            ++next->revision;
            publish(std::move(next));
        }
    }
    return played;
}

bool AlbumSessionService::ensurePreparedPlaylist(
    const AlbumSessionSnapshot& state,
    std::size_t& playlist)
{
    auto playlists = playlist_manager::get();
    playlist = ownedPlaylist();
    if (playlist == pfc_infinite) {
        playlist = playlists->create_playlist(
            kPlaylistName, pfc_infinite, pfc_infinite);
        if (playlist == pfc_infinite) return false;
        try {
            g_albumSessionPlaylistGuid =
                playlist_manager_v5::get()->playlist_get_guid(playlist);
        } catch (...) {
            return false;
        }
    }
    if (!preparationLease_.needsPreparation(
            state.sessionId, state.compositionRevision)) {
        return true;
    }
    metadb_handle_list handles;
    for (const auto& track : state.tracks) handles.add_item(track.handle);
    preparingPlaylist_ = true;
    playlists->playlist_undo_backup(playlist);
    playlists->playlist_clear(playlist);
    if (playlists->playlist_insert_items(
            playlist, 0, handles, bit_array_false()) == pfc_infinite) {
        preparingPlaylist_ = false;
        return false;
    }
    preparingPlaylist_ = false;
    preparationLease_.prepared(state.sessionId, state.compositionRevision);
    return true;
}

bool AlbumSessionService::playPreviousLocal()
{
    const auto state = snapshot();
    if (!state) return false;
    const auto target = nekorox::resolveAlbumPolicyIndex(
        state->permutation, state->currentInSession, -1,
        state->policy.repeat);
    return target && playTrack(*target);
}

bool AlbumSessionService::playNextLocal()
{
    const auto state = snapshot();
    if (!state) return false;
    const auto target = nekorox::resolveAlbumPolicyIndex(
        state->permutation, state->currentInSession, 1,
        state->policy.repeat);
    return target && playTrack(*target);
}

void AlbumSessionService::setRepeatMode(nekorox::AlbumRepeatMode mode)
{
    const auto current = snapshot();
    if (!current || current->sessionId == 0 || current->policy.repeat == mode) {
        return;
    }
    auto next = std::make_shared<AlbumSessionSnapshot>(*current);
    next->policy.repeat = mode;
    cancelContinuation();
    ++next->revision;
    publish(std::move(next));
}

void AlbumSessionService::setShuffle(bool enabled)
{
    const auto current = snapshot();
    if (!current || current->sessionId == 0 ||
        current->policy.shuffle == enabled) return;
    auto next = std::make_shared<AlbumSessionSnapshot>(*current);
    next->policy.shuffle = enabled;
    cancelContinuation();
    next->permutation = nekorox::makeAlbumPermutation(
        next->tracks.size(), next->currentInSession, next->policy);
    ++next->revision;
    publish(std::move(next));
}

bool AlbumSessionService::onPlaybackStop(play_control::t_stop_reason reason)
{
    if (reason != play_control::stop_reason_eof) cancelContinuation();
    if(reason==play_control::stop_reason_user){
        const auto current=snapshot();
        if(current && current->actualPlayingIndex){
            auto next=std::make_shared<AlbumSessionSnapshot>(*current);
            next->actualPlayingIndex.reset();++next->revision;publish(std::move(next));
        }
        return false;
    }
    if (reason != play_control::stop_reason_eof ||
        playlist_manager::get()->queue_get_count() != 0) return false;
    const auto state = snapshot();
    if (!state || state->state != AlbumSessionState::ready ||
        !state->transportOrderLease || state->outsideSession ||
        state->currentInSession >= state->tracks.size()) return false;
    const auto target = nekorox::resolveAlbumPolicyIndex(
        state->permutation, state->currentInSession, 1,
        state->policy.repeat);
    if (!target) {
        if(state->actualPlayingIndex){
            auto next=std::make_shared<AlbumSessionSnapshot>(*state);
            next->actualPlayingIndex.reset();++next->revision;publish(std::move(next));
        }
        return false;
    }
    const nekorox::AlbumContinuationToken token{transportIntent_, state->requestId,
        state->sessionId, state->compositionRevision};
    fb2k::inMainThread([token, target = *target] {
        auto& service = AlbumSessionService::instance();
        const auto current = service.snapshot();
        if (current && nekorox::albumContinuationAllowed(token,
            {service.transportIntent_, current->requestId, current->sessionId, current->compositionRevision},
            current->state == AlbumSessionState::ready, current->transportOrderLease,
            current->outsideSession, playlist_manager::get()->queue_get_count() != 0)) {
            (void)service.playTrack(target);
        }
    });
    return true;
}

void AlbumSessionService::onNewTrack(const metadb_handle_ptr& track)
{
    cancelContinuation();
    const auto current = snapshot();
    if (!track.is_valid()) return;
    if (!current || current->sessionId == 0) {
        // The normal artwork surface is always driven by now-playing. Keep
        // ALBUM under the same ownership rule when playback begins while its
        // surface is already open, including an initially empty session.
        if (playerShellMode() == PlayerShellMode::album) {
            (void)enterForTrack(track);
        }
        return;
    }
    std::optional<std::size_t> playingIndex;
    for (std::size_t index = 0; index < current->tracks.size(); ++index) {
        if (current->tracks[index].handle == track) {
            playingIndex = index;
            break;
        }
    }
    if (!playingIndex &&
        playerShellMode() == PlayerShellMode::album) {
        // Crossing an album boundary must behave exactly like the regular
        // artwork panel: the newly playing handle becomes authoritative.
        // enterForTrack is generation-cancelled/latest-wins, so rapid skips
        // cannot publish a late album over the final selection.
        (void)enterForTrack(track);
        return;
    }
    // A different album creates a fresh loading snapshot above. Do not copy
    // the old track list/permutation only to discard it at that boundary.
    auto next = std::make_shared<AlbumSessionSnapshot>(*current);
    next->outsideSession = !playingIndex.has_value();
    next->currentInSession = playingIndex.value_or(static_cast<std::size_t>(-1));
    next->actualPlayingIndex = playingIndex;
    next->queueExternal = playlist_manager::get()->queue_get_count() != 0;
    next->playlistLeaseValid = ownedPlaylist() != pfc_infinite;
    next->ownedPlaylistValid = next->playlistLeaseValid;
    ++next->revision;
    publish(std::move(next));
    refreshPlayingIdentity();
}

void AlbumSessionService::onQueueChanged()
{
    cancelContinuation();
    const auto current = snapshot();
    if (!current || current->sessionId == 0) return;
    const auto queueExternal = playlist_manager::get()->queue_get_count() != 0;
    // Queue contents have their own revision and notifications. This snapshot
    // only records whether a queue exists; retain it when that fact is unchanged.
    if (current->queueExternal == queueExternal) return;
    auto next = std::make_shared<AlbumSessionSnapshot>(*current);
    next->queueExternal = queueExternal;
    ++next->revision;
    publish(std::move(next));
}

void AlbumSessionService::validatePlaylistLease()
{
    if (ownedPlaylist() != pfc_infinite) return;
    cancelContinuation();
    g_albumSessionPlaylistGuid = GUID_NULL;
    preparationLease_.clear();
    const auto current = snapshot();
    if (!current || current->sessionId == 0) return;
    auto next = std::make_shared<AlbumSessionSnapshot>(*current);
    next->playlistLeaseValid = false;
    next->ownedPlaylistValid = false;
    next->transportOrderLease = false;
    ++next->revision;
    publish(std::move(next));
}

void AlbumSessionService::revokePlaylistLease()
{
    cancelContinuation();
    const auto current = snapshot();
    if (!current || current->sessionId == 0 ||
        !current->transportOrderLease) return;
    auto next = std::make_shared<AlbumSessionSnapshot>(*current);
    next->transportOrderLease = false;
    ++next->revision;
    publish(std::move(next));
}

void AlbumSessionService::markOwnedPlaylistModified(std::size_t playlist)
{
    if (preparingPlaylist_ || playlist != ownedPlaylist()) return;
    cancelContinuation();
    preparationLease_.externallyModified();
    const auto current = snapshot();
    if (!current || current->sessionId == 0 ||
        current->compositionExternallyModified) return;
    auto next = std::make_shared<AlbumSessionSnapshot>(*current);
    next->compositionExternallyModified = true;
    next->transportOrderLease = false;
    ++next->revision;
    publish(std::move(next));
}

} // namespace nekorox::component
