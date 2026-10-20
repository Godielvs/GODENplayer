#include "media_file_operation_service.h"
#include "operation_sequence.h"
#include "database_service.h"
#include "media_binding_recovery.h"
#include "tag_edit_service.h"
#include "nekorox/track_identity.h"

#include "text_util.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <set>

namespace nekorox::component {
namespace {

bool sameNativePath(const std::filesystem::path& left,
    const std::filesystem::path& right) noexcept
{
    if (left.empty() || right.empty()) return false;
    return _wcsicmp(left.lexically_normal().c_str(),
        right.lexically_normal().c_str()) == 0;
}

std::filesystem::path nativeTrackPath(const metadb_handle_ptr& track)
{
    if (!track.is_valid()) return {};
    try {
        return std::filesystem::path(utf8ToWide(
            filesystem::g_get_native_path(track->get_path()).c_str()));
    } catch (...) {
        return {};
    }
}

std::filesystem::path journalPath()
{
    const auto native = filesystem::g_get_native_path(core_api::get_profile_path());
    const auto ticks = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::wostringstream id;
    id << std::hex << ticks << L'-' << GetCurrentProcessId() << L'-'
       << GetTickCount64();
    return std::filesystem::path(utf8ToWide(native.c_str())) /
        L"goden" / L"import-journals" / (id.str() + L".json");
}

std::string jsonEscape(std::wstring_view value)
{
    if (value.empty()) return {};
    const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), utf8.data(), bytes, nullptr, nullptr) != bytes)
        return {};
    std::string output;
    output.reserve(utf8.size() + 8);
    static constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : utf8) {
        const auto c = static_cast<unsigned char>(byte);
        if (c == '\\') output += "\\\\";
        else if (c == '\"') output += "\\\"";
        else if (c < 0x20) {
            output += "\\u00";
            output.push_back(hex[c >> 4U]);
            output.push_back(hex[c & 0x0fU]);
        } else output.push_back(byte);
    }
    return output;
}

const char* outcomeText(MediaCopyOutcome outcome)
{
    switch (outcome) {
    case MediaCopyOutcome::staging: return "Staging";
    case MediaCopyOutcome::verified: return "Verified";
    case MediaCopyOutcome::backedUp: return "BackedUp";
    case MediaCopyOutcome::copied: return "Copied";
    case MediaCopyOutcome::moved: return "Moved";
    case MediaCopyOutcome::duplicateRecoverable: return "DuplicateRecoverable";
    case MediaCopyOutcome::skipped: return "Skipped";
    case MediaCopyOutcome::rejected: return "Rejected";
    case MediaCopyOutcome::conflict: return "Conflict";
    case MediaCopyOutcome::failed: return "Failed";
    case MediaCopyOutcome::cancelled: return "Cancelled";
    }
    return "Unknown";
}

void notifyOperations(const std::vector<MediaCopyResult>& results)
{
    std::vector<pfc::string8> copyFromStorage, copyToStorage;
    std::vector<pfc::string8> moveFromStorage, moveToStorage;
    for (const auto& item : results) {
        const bool copied = item.outcome == MediaCopyOutcome::copied ||
            item.outcome == MediaCopyOutcome::duplicateRecoverable;
        const bool moved = item.outcome == MediaCopyOutcome::moved;
        if ((!copied && !moved) || sameNativePath(item.source, item.destination))
            continue;
        pfc::string8 from, to;
        filesystem::g_get_canonical_path(
            wideToUtf8(item.source.wstring()).c_str(), from);
        filesystem::g_get_canonical_path(
            wideToUtf8(item.destination.wstring()).c_str(), to);
        if (moved) {
            moveFromStorage.push_back(std::move(from));
            moveToStorage.push_back(std::move(to));
        } else {
            copyFromStorage.push_back(std::move(from));
            copyToStorage.push_back(std::move(to));
        }
    }
    pfc::list_t<const char*> copyFrom, copyTo, moveFrom, moveTo;
    for (std::size_t index = 0; index < copyFromStorage.size(); ++index) {
        copyFrom.add_item(copyFromStorage[index].c_str());
        copyTo.add_item(copyToStorage[index].c_str());
    }
    for (std::size_t index = 0; index < moveFromStorage.size(); ++index) {
        moveFrom.add_item(moveFromStorage[index].c_str());
        moveTo.add_item(moveToStorage[index].c_str());
    }
    if (copyFrom.get_count() != 0)
        file_operation_callback::g_on_files_copied(copyFrom, copyTo);
    if (moveFrom.get_count() != 0)
        file_operation_callback::g_on_files_moved(moveFrom, moveTo);
}

bool needsMetadataRefresh(const MediaCopyResult& result)
{
    return (result.outcome == MediaCopyOutcome::copied ||
            result.outcome == MediaCopyOutcome::moved) &&
        sameNativePath(result.source, result.destination);
}

std::vector<MediaRevisionBinding> captureMediaRevisions(
    const std::filesystem::path& path, std::stop_token stop)
{
    abort_callback_impl abort;
    std::stop_callback cancelled(stop, [&abort] { abort.abort(); });
    const auto size = std::filesystem::file_size(path);
    const auto modified = std::filesystem::last_write_time(path);
    pfc::string8 canonical;
    filesystem::g_get_canonical_path(wideToUtf8(path.wstring()).c_str(), canonical);
    input_info_reader::ptr reader;
    input_entry::g_open_for_info_read(reader, nullptr, canonical.c_str(), abort);
    const auto count = reader->get_subsong_count();
    if (count == 0 || count > 65536) throw std::runtime_error("Invalid physical subsong count");
    std::set<std::uint32_t> subsongs;
    std::vector<MediaRevisionBinding> revisions;
    for (t_uint32 i = 0; i < count; ++i) {
        abort.check();
        const auto subsong = reader->get_subsong(i);
        if (!subsongs.insert(subsong).second) continue;
        const auto identity = makeTrackId({path, subsong, size,
            static_cast<std::uint64_t>(modified.time_since_epoch().count())});
        MediaRevisionBinding binding;
        binding.previousRevisionTrackId = identity.value;
        binding.current.subsong = subsong;
        revisions.push_back(std::move(binding));
    }
    if (size != std::filesystem::file_size(path) ||
            modified != std::filesystem::last_write_time(path))
        throw std::runtime_error("Source changed during identity capture");
    return revisions;
}

// The serial file workers accumulate one SDK hint list for the operation.
// add_hint_reader reads every subsong; on_done is deliberately deferred until
// the complete batch reaches the main thread, to notify other components once.
void collectMetadataHints(std::vector<MediaCopyResult>& results,
    metadb_hint_list::ptr& hints, std::stop_token stop)
{
    abort_callback_impl abort;
    std::stop_callback cancelled(stop, [&abort] { abort.abort(); });
    for (auto& result : results) {
        if (!needsMetadataRefresh(result)) continue;
        result.metadataRefreshFailed = false;
        try {
            abort.check();
            pfc::string8 canonical;
            filesystem::g_get_canonical_path(
                wideToUtf8(result.destination.wstring()).c_str(), canonical);
            input_info_reader::ptr reader;
            input_entry::g_open_for_info_read(reader, nullptr, canonical.c_str(), abort);
            if (!hints.is_valid()) hints = metadb_hint_list::create();
            hints->add_hint_reader(canonical.c_str(), reader, abort);
        } catch (...) {
            result.metadataRefreshFailed = true;
            result.detail += L"; file verified, but metadata could not be refreshed";
        }
    }
}

void publishMetadataHints(std::vector<MediaCopyResult>& results,
    const metadb_hint_list::ptr& hints)
{
    if (!hints.is_valid()) return;
    try {
        hints->on_done();
    } catch (...) {
        for (auto& result : results) {
            if (!needsMetadataRefresh(result)) continue;
            result.metadataRefreshFailed = true;
            result.detail += L"; file verified, but metadata publication failed";
        }
    }
}

} // namespace

MediaFileOperationService& MediaFileOperationService::instance()
{
    static MediaFileOperationService value;
    return value;
}

MediaFileOperationService::~MediaFileOperationService()
{
    shutdownAndDrain();
}

bool MediaFileOperationService::busy() const noexcept
{
    std::lock_guard lock(mutex_);
    return active_ || recoveryActive_;
}

bool MediaFileOperationService::mutationAllowed(const std::filesystem::path& path) const noexcept
{
    std::lock_guard lock(mutex_);
    return !recoveryActive_ && !recoveryLimited_ && !shuttingDown_ &&
        std::none_of(blockedBindingPaths_.begin(), blockedBindingPaths_.end(),
            [&](const auto& blocked) { return sameNativePath(path, blocked); });
}

void MediaFileOperationService::beginBindingRecovery()
{
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        if (active_ || recoveryActive_ || shuttingDown_) return;
        recoveryActive_ = true;
        generation = ++recoveryGeneration_;
    }
    const auto directory = journalPath().parent_path();
    if (worker_.joinable()) worker_.join();
    worker_ = std::jthread([this, directory, generation](std::stop_token stop) {
        auto scan = scanMediaBindingRecovery(directory, stop);
        fb2k::inMainThread([this, scan = std::move(scan), generation]() mutable {
            {
                std::lock_guard lock(mutex_);
                if (shuttingDown_ || !recoveryActive_ || recoveryGeneration_ != generation) return;
                recoveryLimited_ = scan.limited;
                blockedBindingPaths_ = std::move(scan.blockedPaths);
            }
            std::vector<TrackRevisionRebind> requests;
            for (const auto& item : scan.pending) for (const auto& binding : item.revisions)
                requests.push_back({binding.previousRevisionTrackId,
                    std::filesystem::path(utf8ToWide(binding.current.path)), binding.current.subsong, binding.current});
            if (requests.empty()) {
                bool attention = false;
                {
                    std::lock_guard lock(mutex_);
                    recoveryActive_ = false;
                    attention = recoveryLimited_ || !blockedBindingPaths_.empty();
                }
                if (attention)
                    console::print("GODEN MEDIA: pending identity recovery needs attention; affected edits are protected.");
                return;
            }
            DatabaseService::instance().rebindTagRevisions(std::move(requests),
                [this, items = std::move(scan.pending), generation](bool success, std::string) mutable {
                    {
                        std::lock_guard lock(mutex_);
                        if (shuttingDown_ || !recoveryActive_ || recoveryGeneration_ != generation) return;
                    }
                    if (worker_.joinable()) worker_.join();
                    worker_ = std::jthread([this, items = std::move(items), success, generation](std::stop_token stop) {
                        std::vector<std::filesystem::path> failed;
                        for (const auto& item : items) {
                            if (success && acknowledgeMediaBindingRecovery(item, stop)) continue;
                            for (const auto& binding : item.revisions)
                                failed.push_back(std::filesystem::path(utf8ToWide(binding.current.path)));
                        }
                        fb2k::inMainThread([this, failed = std::move(failed), generation]() mutable {
                            bool attention = false;
                            {
                                std::lock_guard lock(mutex_);
                                if (shuttingDown_ || !recoveryActive_ || recoveryGeneration_ != generation) return;
                                blockedBindingPaths_.insert(blockedBindingPaths_.end(), failed.begin(), failed.end());
                                recoveryActive_ = false;
                                attention = recoveryLimited_ || !blockedBindingPaths_.empty();
                            }
                            if (attention)
                                console::print("GODEN MEDIA: identity recovery remains pending; affected edits are protected.");
                        });
                    });
                });
        });
    });
}

bool MediaFileOperationService::hasInPlaceUndo() const noexcept
{
    std::lock_guard lock(mutex_);
    return !active_ && !recoveryActive_ && !inPlaceUndo_.empty();
}

std::uint64_t MediaFileOperationService::undoOperationSequence() const noexcept
{
    std::lock_guard lock(mutex_);
    return !active_ && !inPlaceUndo_.empty() ? inPlaceUndoSequence_ : 0;
}

bool MediaFileOperationService::undoLastInPlace(Completion completion)
{
    if (TagEditService::instance().busy()) return false;
    std::vector<MediaCopyResult> undo;
    std::uint64_t operation = 0;
    {
        std::lock_guard lock(mutex_);
        if (active_ || recoveryActive_ || recoveryLimited_ || shuttingDown_ || inPlaceUndo_.empty()) return false;
        for (const auto& item : inPlaceUndo_) for (const auto& blocked : blockedBindingPaths_)
            if (sameNativePath(item.destination, blocked)) return false;
        active_ = true;
        workerActive_ = true;
        identityBindingPending_ = false;
        operation = activeOperationSequence_ = nextOperationSequence();
        undo = inPlaceUndo_;
    }
    if (worker_.joinable()) worker_.join();
    worker_ = std::jthread([this, operation, undo = std::move(undo),
            completion = std::move(completion)](std::stop_token stop) mutable {
        MediaRestoreExecutionOptions options;
        options.journalPath = journalPath();
        options.allowRestore = [this](const std::filesystem::path& path) {
            std::lock_guard lock(mutex_);
            return !shuttingDown_ && !sameNativePath(playbackPath_, path);
        };
        auto results = restoreInPlaceBackups(undo, stop, options);
        metadb_hint_list::ptr hints;
        collectMetadataHints(results, hints, stop);
        fb2k::inMainThread([this, operation, hints, results = std::move(results),
                completion = std::move(completion)]() mutable {
            finishOperation(std::move(results), std::move(completion), operation, true, hints);
        });
    });
    return true;
}

bool MediaFileOperationService::startCopy(MediaPlan plan, Completion completion)
{
    if (plan.mode != MediaOrganizationMode::copy) return false;
    return startPlan(std::move(plan), std::move(completion));
}

bool MediaFileOperationService::startPlan(MediaPlan plan, Completion completion,
    MediaCopyExecutionOptions options)
{
    if (TagEditService::instance().busy()) return false;
    std::set<std::wstring> destinations;
    for (const auto& item : plan.items)
        if (!mutationAllowed(item.source) || !mutationAllowed(item.destination) ||
                (plan.mode == MediaOrganizationMode::inPlace &&
                    !destinations.insert(normalizeWindowsPath(item.destination)).second)) return false;
    if ((plan.mode != MediaOrganizationMode::copy &&
            plan.mode != MediaOrganizationMode::move &&
            plan.mode != MediaOrganizationMode::inPlace) || plan.items.empty()) return false;
    {
        std::lock_guard lock(mutex_);
        if (active_ || shuttingDown_) return false;
    }
    if (worker_.joinable()) worker_.join();
    const auto journal = journalPath();
    if (options.journalPath.empty()) options.journalPath = journal;
    if (options.backupRoot.empty()) {
        const auto native = filesystem::g_get_native_path(core_api::get_profile_path());
        options.backupRoot = std::filesystem::path(utf8ToWide(native.c_str())) /
            L"goden" / L"tag-backups" / journal.stem();
    }
    metadb_handle_ptr playing;
    const auto playingPath = playback_control::get()->get_now_playing(playing)
        ? nativeTrackPath(playing) : std::filesystem::path{};
    {
        std::lock_guard lock(mutex_);
        active_ = true;
        activeOperationSequence_ = nextOperationSequence();
        identityBindingPending_ = false;
        metadataHints_.release();
        workerActive_ = false;
        activePlan_ = plan;
        pending_.clear();
        pending_.reserve(plan.items.size());
        for (std::size_t index = 0; index < plan.items.size(); ++index)
            pending_.push_back({std::move(plan.items[index]), index, 0});
        current_.reset();
        results_.clear();
        options_ = std::move(options);
        completion_ = std::move(completion);
        playbackPath_ = playingPath;
        queueJournalPath_ = options_.journalPath;
        if (!persistQueueLocked()) {
            active_ = false;
            pending_.clear();
            completion_ = {};
            queueJournalPath_.clear();
            return false;
        }
    }
    pump();
    return true;
}

std::filesystem::path MediaFileOperationService::itemJournalPath(
    const PendingWork& work) const
{
    auto base = queueJournalPath_;
    std::wostringstream name;
    name << base.stem().wstring() << L"-item-" << std::setw(4)
         << std::setfill(L'0') << work.sequence << L"-attempt-"
         << work.attempt << base.extension().wstring();
    return base.parent_path() / name.str();
}

bool MediaFileOperationService::persistQueueLocked() const
{
    if (queueJournalPath_.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(queueJournalPath_.parent_path(), error);
    if (error) return false;
    const auto staging = queueJournalPath_.wstring() + L".staging";
    std::ofstream stream(std::filesystem::path(staging),
        std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream << "{\n  \"schema\":\"goden.media-operation-queue.v1\",\n"
           << "  \"mode\":\"" << (activePlan_.mode == MediaOrganizationMode::move
                ? "Move" : activePlan_.mode == MediaOrganizationMode::inPlace
                ? "InPlace" : "Copy") << "\",\n"
           << "  \"approvedRoot\":\"" << jsonEscape(activePlan_.approvedRoot.wstring())
           << "\",\n  \"shuttingDown\":" << (shuttingDown_ ? "true" : "false")
           << ",\n  \"current\":";
    const auto writeWork = [&stream](const PendingWork& work) {
        stream << "{\"sequence\":" << work.sequence << ",\"attempt\":"
               << work.attempt << ",\"source\":\""
               << jsonEscape(work.item.source.wstring())
               << "\",\"destination\":\""
               << jsonEscape(work.item.destination.wstring()) << "\"}";
    };
    if (current_) writeWork(*current_);
    else stream << "null";
    stream << ",\n  \"pending\":[";
    for (std::size_t index = 0; index < pending_.size(); ++index) {
        if (index) stream << ',';
        writeWork(pending_[index]);
    }
    stream << "],\n  \"completed\":[";
    for (std::size_t index = 0; index < results_.size(); ++index) {
        if (index) stream << ',';
        const auto& result = results_[index];
        stream << "{\"source\":\"" << jsonEscape(result.source.wstring())
               << "\",\"destination\":\""
               << jsonEscape(result.destination.wstring())
               << "\",\"sourceBackup\":\""
               << jsonEscape(result.sourceBackup.wstring())
               << "\",\"outcome\":\"" << outcomeText(result.outcome)
               << "\",\"sourceSha256\":\"" << result.sourceSha256
               << "\",\"sha256\":\"" << result.sha256
               << "\",\"sidecarsExpected\":" << result.sidecarsExpected
               << ",\"sidecarsCommitted\":" << result.sidecarsCommitted << '}';
    }
    stream << "]\n}\n";
    stream.flush();
    if (!stream) return false;
    stream.close();
    return MoveFileExW(staging.c_str(), queueJournalPath_.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool MediaFileOperationService::playbackProtects(
    const std::filesystem::path& path) const noexcept
{
    std::lock_guard lock(mutex_);
    return sameNativePath(playbackPath_, path);
}

void MediaFileOperationService::onPlaybackChanged(const metadb_handle_ptr& track)
{
    {
        std::lock_guard lock(mutex_);
        playbackPath_ = nativeTrackPath(track);
        if (shuttingDown_) return;
    }
    pump();
}

void MediaFileOperationService::pump()
{
    MediaPlan single;
    MediaCopyExecutionOptions options;
    PendingWork selectedWork;
    MediaPlanItem selected;
    bool haveItem = false;
    bool aborted = false;
    Completion failedCompletion;
    std::vector<MediaCopyResult> failedResults;
    metadb_hint_list::ptr hints;
    std::uint64_t operation = 0;
    {
        std::lock_guard lock(mutex_);
        if (!active_ || workerActive_ || shuttingDown_) return;
        operation = activeOperationSequence_;
        hints = metadataHints_;
        for (auto iterator = pending_.begin(); iterator != pending_.end(); ++iterator) {
            const bool protectedMove = (activePlan_.mode == MediaOrganizationMode::move ||
                    activePlan_.mode == MediaOrganizationMode::inPlace) &&
                sameNativePath(playbackPath_, iterator->item.source);
            const bool protectedReplace = iterator->item.replaceExisting &&
                sameNativePath(playbackPath_, iterator->item.destination);
            if (protectedMove || protectedReplace) continue;
            selectedWork = *iterator;
            selected = iterator->item;
            pending_.erase(iterator);
            current_ = selectedWork;
            haveItem = true;
            break;
        }
        if (!haveItem) return;
        single.approvedRoot = activePlan_.approvedRoot;
        single.mode = activePlan_.mode;
        single.collision = activePlan_.collision;
        single.items.push_back(selected);
        options = options_;
        options.journalPath = itemJournalPath(selectedWork);
        workerActive_ = true;
        if (!persistQueueLocked()) {
            current_.reset();
            MediaCopyResult failure;
            failure.source = selected.source;
            failure.destination = selected.destination;
            failure.outcome = MediaCopyOutcome::failed;
            failure.detail = L"Durable operation queue could not be updated";
            results_.push_back(std::move(failure));
            for (const auto& pending : pending_) {
                MediaCopyResult skipped;
                skipped.source = pending.item.source;
                skipped.destination = pending.item.destination;
                skipped.outcome = MediaCopyOutcome::cancelled;
                skipped.detail = L"Batch stopped before mutation because its queue was not durable";
                results_.push_back(std::move(skipped));
            }
            pending_.clear();
            workerActive_ = true; // Keep admission closed through metadata publication.
            aborted = true;
            failedCompletion = std::move(completion_);
            failedResults = std::move(results_);
        }
    }
    if (aborted) {
        // pump can also run inside a playback callback. Publish the terminal
        // metadata batch at the same deferred boundary as normal completion.
        fb2k::inMainThread([this, results = std::move(failedResults),
                completion = std::move(failedCompletion), operation, hints]() mutable {
            finishOperation(std::move(results), std::move(completion), operation, false, hints);
        });
    }
    if (aborted || !haveItem) return;
    if (worker_.joinable()) worker_.join();
    const auto upstreamAllow = options.allowStep;
    options.allowStep = [this, selected, mode = single.mode,
            upstreamAllow](std::wstring_view step) {
        if (upstreamAllow && !upstreamAllow(step)) return false;
        const bool destructive = step == L"before-move-rename" ||
            step == L"before-final-rename" || step == L"before-source-delete";
        if (!destructive) return true;
        if ((mode == MediaOrganizationMode::move ||
                mode == MediaOrganizationMode::inPlace) &&
            playbackProtects(selected.source)) return false;
        if (selected.replaceExisting && playbackProtects(selected.destination))
            return false;
        return true;
    };
    library_file_move_scope::ptr moveScope;
    if (single.mode == MediaOrganizationMode::move)
        moveScope = library_file_move_manager::get()->acquire_scope();
    worker_ = std::jthread([this, single = std::move(single), options,
            selected, selectedWork, moveScope, operation, hints](std::stop_token stop) mutable {
        if (single.mode == MediaOrganizationMode::inPlace) {
            options.captureSourceRevisions = [stop](const auto& source) {
                return captureMediaRevisions(source, stop);
            };
        }
        auto itemResults = executeMediaPlan(single, stop, options);
        if (single.mode == MediaOrganizationMode::inPlace)
            collectMetadataHints(itemResults, hints, stop);
        fb2k::inMainThread([this, itemResults = std::move(itemResults),
                selected, selectedWork, moveScope, operation, hints]() mutable {
            {
                std::lock_guard lock(mutex_);
                if (shuttingDown_ || !active_ || activeOperationSequence_ != operation) return;
            }
            notifyOperations(itemResults);
            Completion completion;
            std::vector<MediaCopyResult> completed;
            bool repump = false;
            bool finished = false;
            {
                std::lock_guard lock(mutex_);
                if (shuttingDown_ || !active_ || activeOperationSequence_ != operation) return;
                workerActive_ = false;
                metadataHints_ = hints;
                current_.reset();
                const bool protectedNow = sameNativePath(playbackPath_, selected.source) ||
                    (selected.replaceExisting &&
                        sameNativePath(playbackPath_, selected.destination));
                const bool inPlace = activePlan_.mode == MediaOrganizationMode::inPlace;
                const bool safeToRetry = itemResults.size() == 1 && protectedNow &&
                    !itemResults[0].physicalCommitNeedsBinding &&
                    std::filesystem::exists(selected.source) &&
                    (inPlace || !std::filesystem::exists(selected.destination) ||
                        selected.replaceExisting) &&
                    (itemResults[0].outcome == MediaCopyOutcome::failed ||
                     itemResults[0].outcome == MediaCopyOutcome::conflict);
                if (safeToRetry && !shuttingDown_) {
                    auto retry = selectedWork;
                    ++retry.attempt;
                    pending_.push_back(std::move(retry));
                }
                else results_.insert(results_.end(),
                    std::make_move_iterator(itemResults.begin()),
                    std::make_move_iterator(itemResults.end()));
                const auto queuePersisted = persistQueueLocked();
                if (!queuePersisted) {
                    for (auto& result : results_) {
                        if (result.outcome == MediaCopyOutcome::copied ||
                            result.outcome == MediaCopyOutcome::moved)
                            result.detail += L"; aggregate queue journal update failed (item journal retained)";
                    }
                }
                if (pending_.empty() || !queuePersisted) {
                    workerActive_ = true;
                    finished = true;
                    completion = std::move(completion_);
                    completed = std::move(results_);
                } else if (!shuttingDown_) repump = true;
            }
            if (finished) finishOperation(std::move(completed),
                std::move(completion), operation, false, hints);
            else if (repump) pump();
        });
    });
}

void MediaFileOperationService::finishOperation(std::vector<MediaCopyResult> results,
    Completion completion, std::uint64_t operation, bool undo,
    metadb_hint_list::ptr hints, bool identitiesSettled)
{
    {
        std::lock_guard lock(mutex_);
        if (shuttingDown_ || !active_ || activeOperationSequence_ != operation) return;
        if (!identitiesSettled && identityBindingPending_) return;
    }
    if (!identitiesSettled) {
        std::vector<TrackRevisionRebind> requests;
        for (const auto& result : results) {
            if (!needsMetadataRefresh(result) || result.identityRefreshFailed) continue;
            for (const auto& binding : result.revisionBindings) {
                requests.push_back({binding.previousRevisionTrackId, result.destination,
                    binding.current.subsong, binding.current});
            }
        }
        if (!requests.empty()) {
            {
                std::lock_guard lock(mutex_);
                identityBindingPending_ = true;
            }
            DatabaseService::instance().rebindTagRevisions(std::move(requests),
                [this, results = std::move(results), completion = std::move(completion),
                        operation, undo, hints](bool success, std::string) mutable {
                    {
                        std::lock_guard lock(mutex_);
                        if (shuttingDown_ || !active_ || activeOperationSequence_ != operation) return;
                    }
                    if (!success) for (auto& result : results) {
                        if (needsMetadataRefresh(result) && !result.revisionBindings.empty()) {
                            result.identityRefreshFailed = true;
                            result.detail += L"; file verified, but favorite/note identity binding needs attention";
                        }
                    }
                    if (worker_.joinable()) worker_.join();
                    worker_ = std::jthread([this, results = std::move(results),
                            completion = std::move(completion), operation, undo, hints, success]
                            (std::stop_token stop) mutable {
                        if (success) for (auto& result : results) {
                            if (!needsMetadataRefresh(result) || result.identityRefreshFailed ||
                                    result.bindingJournalPath.empty()) continue;
                            MediaBindingRecoveryItem item{result.bindingJournalPath,
                                result.bindingJournalSha256, result.revisionBindings};
                            if (!acknowledgeMediaBindingRecovery(item, stop)) {
                                result.identityRefreshFailed = true;
                                result.detail += L"; identity saved, but recovery confirmation needs attention";
                            } else result.physicalCommitNeedsBinding = false;
                        }
                        fb2k::inMainThread([this, results = std::move(results),
                                completion = std::move(completion), operation, undo, hints]() mutable {
                            finishOperation(std::move(results), std::move(completion), operation, undo, hints, true);
                        });
                    });
                });
            return;
        }
    }
    // SDK notification can reenter components. Keep admission closed, but
    // never hold our mutex while it dispatches those callbacks.
    publishMetadataHints(results, hints);
    {
        std::lock_guard lock(mutex_);
        if (shuttingDown_ || !active_ || activeOperationSequence_ != operation) return;
        for (const auto& result : results)
            if (result.physicalCommitNeedsBinding ||
                    (needsMetadataRefresh(result) && result.identityRefreshFailed))
                blockedBindingPaths_.push_back(result.destination);
        if (undo) {
            // A mixed batch can restore some files while playback protects
            // another. Retry only the remaining files with their original
            // expected hashes; already restored files must not block recovery.
            // restoreInPlaceBackups returns one result per input in order.
            // Verify the resource too, so an unexpected result cannot consume
            // another backup. One linear pass keeps large batches cheap.
            std::size_t resultIndex = 0;
            std::erase_if(inPlaceUndo_, [&results, &resultIndex](const auto& pending) {
                if (resultIndex >= results.size()) return false;
                const auto& result = results[resultIndex++];
                return result.outcome == MediaCopyOutcome::moved &&
                    sameNativePath(result.destination, pending.destination) &&
                    sameNativePath(result.backup, pending.backup);
            });
            if (inPlaceUndo_.empty()) inPlaceUndoSequence_ = 0;
        } else if (activePlan_.mode == MediaOrganizationMode::inPlace) {
            inPlaceUndo_.clear();
            for (const auto& result : results)
                if (result.outcome == MediaCopyOutcome::copied && !result.backup.empty())
                    inPlaceUndo_.push_back(result);
            inPlaceUndoSequence_ = inPlaceUndo_.empty() ? 0 : operation;
        }
        active_ = false;
        workerActive_ = false;
        identityBindingPending_ = false;
        metadataHints_.release();
    }
    if (completion) completion(std::move(results));
}

void MediaFileOperationService::beginShutdown() noexcept
{
    std::lock_guard lock(mutex_);
    shuttingDown_ = true;
    if (active_) (void)persistQueueLocked();
}

void MediaFileOperationService::shutdownAndDrain() noexcept
{
    beginShutdown();
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    std::lock_guard lock(mutex_);
    active_ = false;
    workerActive_ = false;
    identityBindingPending_ = false;
    recoveryActive_ = false;
    current_.reset();
    pending_.clear();
    completion_ = {};
    metadataHints_.release();
}

} // namespace nekorox::component
