#include "database_service.h"
#include "album_session_service.h"
#include "titleformat_cache.h"
#include "nekorox/history_retry.h"
#include "build_identity.h"

#include "nekorox/track_identity.h"
#include "runtime.h"
#include "text_util.h"
#include "workspace_notifications.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <system_error>

namespace nekorox::component {
namespace {

constexpr std::size_t kHistorySnapshotLimit = 4096;

std::int64_t utcNowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string newPersonaId()
{
    GUID value{};
    if (FAILED(CoCreateGuid(&value))) return {};
    wchar_t text[40]{};
    if (StringFromGUID2(value, text, static_cast<int>(std::size(text))) <= 0) {
        return {};
    }
    return "persona:" + wideToUtf8(text);
}

std::int64_t secondsToMs(double seconds) noexcept
{
    if (!std::isfinite(seconds) || seconds <= 0.0) return 0;
    constexpr double kMaximum =
        static_cast<double>(std::numeric_limits<std::int64_t>::max()) / 1000.0;
    return static_cast<std::int64_t>(std::llround(std::min(seconds, kMaximum) * 1000.0));
}

std::string makeSessionId()
{
    std::ostringstream output;
    output << utcNowMs() << '-' << GetCurrentProcessId();
    return output.str();
}

std::array<std::string, 3> trackMetadata(const metadb_handle_ptr& track)
{
    std::array<std::string, 3> result{
        "Unknown artist", "Unknown album", "Unknown title"};
    if (track.is_empty()) return result;

    const auto script = cachedTitleFormat(
        "$if2(%artist%,Unknown artist)$char(31)"
        "$if2(%album%,Unknown album)$char(31)"
        "$if2(%title%,%filename%)");
    pfc::string8_fast formatted;
    if (!track->format_title(nullptr, formatted, script, nullptr)) return result;

    const std::string value = formatted.c_str();
    std::size_t begin = 0;
    for (std::size_t field = 0; field < result.size(); ++field) {
        const auto end = value.find('\x1f', begin);
        result[field] = value.substr(begin,
            end == std::string::npos ? std::string::npos : end - begin);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return result;
}

std::uint64_t fileTimeTicks(const std::filesystem::file_time_type& value) noexcept
{
    return static_cast<std::uint64_t>(value.time_since_epoch().count());
}

bool identifyLocalTrack(const std::filesystem::path& path,
    PersistedTrack& track, std::string& error)
{
    std::error_code statusError;
    const auto size = std::filesystem::file_size(path, statusError);
    if (statusError) {
        error = "analysis track size failed: " + statusError.message();
        return false;
    }
    const auto modified = std::filesystem::last_write_time(path, statusError);
    if (statusError) {
        error = "analysis track timestamp failed: " + statusError.message();
        return false;
    }
    try {
        const auto identity = makeTrackId({path, 0, size, fileTimeTicks(modified)});
        track.trackId = identity.value;
        track.path = identity.normalizedPathUtf8;
        track.partialFingerprint = identity.partialFingerprint;
    } catch (const std::exception& exception) {
        error = std::string("analysis track identity failed: ") + exception.what();
        return false;
    }
    track.fileSize = size;
    track.modifiedTicks = fileTimeTicks(modified);
    track.title = wideToUtf8(path.stem().wstring());
    track.lastSeenUtcMs = utcNowMs();
    return true;
}

} // namespace

DatabaseService& DatabaseService::instance()
{
    static DatabaseService service;
    return service;
}

bool DatabaseService::start()
{
    if (started_.exchange(true)) return true;

    const auto sessionId = makeSessionId();
    std::filesystem::path databasePath;
    try {
        const auto profile = core_api::pathInProfile("nekorox/nekorox.db");
        const auto native = filesystem::g_get_native_path(profile.c_str());
        databasePath = utf8ToWide(native.c_str());
    } catch (const std::exception& exception) {
        started_.store(false, std::memory_order_release);
        publishError(std::string("profile path failed: ") + exception.what());
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        stopping_ = false;
        queue_.clear();
        writeSpool_.clear();
        admittedPlayback_.reset();
    }
    try {
        worker_ = std::thread(
            &DatabaseService::workerMain, this,
            std::move(databasePath), sessionId);
    } catch (const std::exception& exception) {
        started_.store(false, std::memory_order_release);
        publishError(std::string("database worker start failed: ") +
            exception.what());
        return false;
    } catch (...) {
        started_.store(false, std::memory_order_release);
        publishError("database worker start failed");
        return false;
    }

    metadb_handle_ptr current;
    if (playback_control::get()->get_now_playing(current)) onNewTrack(current);
    return true;
}

void DatabaseService::stop()
{
    if (!started_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        stopping_ = true;
    }
    queueChanged_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void DatabaseService::retry()
{
    if (!started_.load(std::memory_order_acquire)) {
        (void)start();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (stopping_) return;
        while (!writeSpool_.empty()) {
            auto pending = std::move(writeSpool_.front());
            writeSpool_.pop_front();
            pending.writeRetryAttempt = 0;
            pending.deferred = false;
            pending.readyAt = {};
            queue_.push_back(std::move(pending));
        }
        PendingEvent retryEvent;
        retryEvent.retryDatabase = true;
        retryEvent.utcMs = utcNowMs();
        queue_.push_front(std::move(retryEvent));
    }
    queueChanged_.notify_one();
}

std::unique_ptr<DatabaseService::TrackSeed> DatabaseService::makeTrackSeed(
    const metadb_handle_ptr& track) const
{
    if (track.is_empty()) return {};
    auto seed = std::make_unique<TrackSeed>();
    seed->sourcePath = track->get_path();
    seed->subsong = track->get_subsong_index();
    seed->durationMs = secondsToMs(track->get_length());
    const auto metadata = trackMetadata(track);
    seed->artist = metadata[0];
    seed->album = metadata[1];
    seed->title = metadata[2];
    try {
        const auto native = filesystem::g_get_native_path(track->get_path());
        seed->nativePath = utf8ToWide(native.c_str());
    } catch (...) {
        seed->nativePath = utf8ToWide(track->get_path());
    }
    return seed;
}

DatabaseService::PendingEvent DatabaseService::makeEvent(PlayEventType type) const
{
    PendingEvent event;
    event.type = type;
    event.utcMs = utcNowMs();
    event.eventUid = "event:v1:" + std::to_string(event.utcMs) + ":" +
        std::to_string(nextEventUid_.fetch_add(1, std::memory_order_relaxed));
    auto control = playback_control::get();
    event.positionMs = control->is_playing()
        ? secondsToMs(control->playback_get_position()) : 0;
    event.durationMs = control->is_playing()
        ? secondsToMs(control->playback_get_length()) : 0;

    t_size playlist = std::numeric_limits<t_size>::max();
    t_size item = std::numeric_limits<t_size>::max();
    if (playlist_manager::get()->get_playing_item_location(&playlist, &item)) {
        if (playlist <= static_cast<t_size>(std::numeric_limits<std::int32_t>::max())) {
            event.playlistIndex = static_cast<std::int32_t>(playlist);
        }
        if (item <= static_cast<t_size>(std::numeric_limits<std::int32_t>::max())) {
            event.itemIndex = static_cast<std::int32_t>(item);
        }
    }
    return event;
}

bool DatabaseService::enqueue(PendingEvent&& event)
{
    if (!started_.load(std::memory_order_acquire)) return false;
    // Destroy the superseded callback after unlocking, including its captures.
    std::unique_ptr<AnalysisTask> supersededPacketCost;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (stopping_) return false;
        if (event.analysis &&
            event.analysis->kind == AnalysisTask::Kind::loadPacketCost) {
            // Only one not-yet-started read is useful to the current-track
            // controller. Append the replacement at the end to retain ordering
            // with other reads. Never discard stores or work already in flight.
            const auto obsolete = std::find_if(queue_.begin(), queue_.end(),
                [](const PendingEvent& queued) {
                    return queued.analysis &&
                        queued.analysis->kind == AnalysisTask::Kind::loadPacketCost;
                });
            if (obsolete != queue_.end()) {
                supersededPacketCost = std::move(obsolete->analysis);
                queue_.erase(obsolete);
            }
        }
        const auto now = std::chrono::steady_clock::now();
        const auto playbackEvent = !event.analysis && !event.userState &&
            !event.persona && !event.identities && !event.revisionRebind &&
            !event.lyrics && !event.retryDatabase;
        if (playbackEvent && !event.playbackAdmitted) {
            event.playbackContext = admittedPlayback_;
            event.playbackAdmitted = true;
        } else if (event.userState || event.persona) {
            event.playbackContext = admittedPlayback_;
        }
        if (playbackEvent && event.type == PlayEventType::seek &&
            !event.playbackProcessed) {
            event.deferred = true;
            event.readyAt = now + std::chrono::milliseconds(500);
            for (auto iterator = queue_.rbegin(); iterator != queue_.rend();
                 ++iterator) {
                if (!iterator->analysis && !iterator->userState &&
                    !iterator->persona && !iterator->identities &&
                    !iterator->revisionRebind && !iterator->lyrics &&
                    !iterator->retryDatabase) {
                    // Do not merge across a pause/start/stop, another track,
                    // or an already attempted write with its own identity.
                    if (iterator->type != PlayEventType::seek ||
                        iterator->playbackProcessed ||
                        iterator->playbackContext != event.playbackContext) break;
                    *iterator = std::move(event);
                    queueChanged_.notify_one();
                    return true;
                }
            }
        } else if (playbackEvent && !event.playbackProcessed) {
            // A pause/stop/new-track command commits the final seek before the
            // following semantic event so history ordering stays causal.
            for (auto& queued : queue_) {
                if (queued.deferred && queued.type == PlayEventType::seek &&
                    !queued.playbackProcessed) {
                    queued.deferred = false;
                    queued.readyAt = now;
                }
            }
        }
        if (playbackEvent) {
            constexpr std::size_t kMaximumPlaybackBacklog = 1024;
            std::size_t playbackQueued = 0;
            for (const auto& queued : queue_) {
                if (!queued.analysis && !queued.userState && !queued.persona &&
                    !queued.identities && !queued.revisionRebind &&
                    !queued.lyrics && !queued.retryDatabase) {
                    ++playbackQueued;
                }
            }
            if (playbackQueued >= kMaximumPlaybackBacklog) {
                const auto oldest = std::find_if(queue_.begin(), queue_.end(),
                    [](const PendingEvent& queued) {
                        return !queued.analysis && !queued.userState &&
                            !queued.persona && !queued.identities &&
                            !queued.revisionRebind && !queued.lyrics &&
                            !queued.retryDatabase;
                    });
                if (oldest != queue_.end()) queue_.erase(oldest);
            }
        }
        queue_.push_back(std::move(event));
    }
    queueChanged_.notify_one();
    return true;
}

std::size_t DatabaseService::pendingPlaybackWriteCount()
{
    std::lock_guard<std::mutex> lock(queueMutex_);
    auto count = writeSpool_.size();
    for (const auto& queued : queue_) {
        if (!queued.analysis && !queued.userState && !queued.persona &&
                !queued.identities && !queued.revisionRebind &&
                !queued.lyrics && !queued.retryDatabase)
            ++count;
    }
    return count;
}

void DatabaseService::onNewTrack(const metadb_handle_ptr& track)
{
    auto event = makeEvent(PlayEventType::start);
    event.playbackContext = std::make_shared<PlaybackContext>();
    event.playbackContext->seed = makeTrackSeed(track);
    event.playbackAdmitted = true;
    if (event.playbackContext->seed)
        event.durationMs = event.playbackContext->seed->durationMs;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        admittedPlayback_ = event.playbackContext;
    }
    // Change the publication epoch before clearing the UI. An old writer
    // completion can no longer replace the new track's loading state.
    publishUserStateLoading();
    publishPersonaLoading();
    enqueue(std::move(event));
}

void DatabaseService::onPause(bool paused)
{
    enqueue(makeEvent(paused ? PlayEventType::pause : PlayEventType::resume));
}

void DatabaseService::onSeek(double seconds)
{
    auto event = makeEvent(PlayEventType::seek);
    event.positionMs = secondsToMs(seconds);
    enqueue(std::move(event));
}

void DatabaseService::onStop(play_control::t_stop_reason reason)
{
    auto type = PlayEventType::stop;
    if (reason == play_control::stop_reason_eof) type = PlayEventType::finish;
    else if (reason == play_control::stop_reason_starting_another) {
        type = PlayEventType::skip;
    }
    enqueue(makeEvent(type));
}

void DatabaseService::loadAnalysis(std::filesystem::path trackPath,
    AnalysisCacheCallback callback)
{
    if (!started_.load(std::memory_order_acquire)) {
        AnalysisCachePayload payload;
        payload.trackPath = std::move(trackPath);
        payload.error = "analysis cache service is not running";
        publishAnalysisResult(std::move(callback), std::move(payload));
        return;
    }
    PendingEvent pending;
    pending.analysis = std::make_unique<AnalysisTask>();
    pending.analysis->kind = AnalysisTask::Kind::loadDeep;
    pending.analysis->trackPath = std::move(trackPath);
    pending.analysis->callback = std::move(callback);
    enqueue(std::move(pending));
}

void DatabaseService::storeAnalysis(std::filesystem::path trackPath,
    std::string expectedTrackId,
    AnalysisMediaInfoV1 media,
    std::shared_ptr<const std::vector<AnalysisWaveformBinV1>> waveform,
    std::string resultJson)
{
    if (!started_.load(std::memory_order_acquire)) return;
    PendingEvent pending;
    pending.analysis = std::make_unique<AnalysisTask>();
    pending.analysis->kind = AnalysisTask::Kind::storeDeep;
    pending.analysis->expectedDeepAnalysisTrackId = std::move(expectedTrackId);
    pending.analysis->trackPath = std::move(trackPath);
    pending.analysis->media = media;
    pending.analysis->waveform = std::move(waveform);
    pending.analysis->resultJson = std::move(resultJson);
    enqueue(std::move(pending));
}

void DatabaseService::loadPacketCost(std::filesystem::path trackPath,
    PacketCostCacheCallback callback)
{
    PendingEvent pending;
    pending.analysis = std::make_unique<AnalysisTask>();
    pending.analysis->kind = AnalysisTask::Kind::loadPacketCost;
    pending.analysis->trackPath = std::move(trackPath);
    pending.analysis->packetCostCallback = std::move(callback);
    if (!enqueue(std::move(pending))) {
        PacketCostCachePayload payload;
        payload.trackPath = std::move(pending.analysis->trackPath);
        payload.error = "packet-cost cache service is not running";
        publishPacketCostResult(std::move(pending.analysis->packetCostCallback),
            std::move(payload));
    }
}

void DatabaseService::storePacketCost(std::filesystem::path trackPath,
    std::string expectedTrackId,
    std::shared_ptr<const PacketCostResult> cost)
{
    if (!started_.load(std::memory_order_acquire) || cost == nullptr ||
        expectedTrackId.empty()) return;
    PendingEvent pending;
    pending.analysis = std::make_unique<AnalysisTask>();
    pending.analysis->kind = AnalysisTask::Kind::storePacketCost;
    pending.analysis->trackPath = std::move(trackPath);
    pending.analysis->packetCost = std::move(cost);
    pending.analysis->expectedPacketCostTrackId = std::move(expectedTrackId);
    enqueue(std::move(pending));
}

void DatabaseService::resolveTrackIdentities(
    std::vector<std::string> revisionTrackIds,
    TrackIdentityBatchCallback callback)
{
    if (!callback) return;
    if (!started_.load(std::memory_order_acquire)) {
        fb2k::inMainThread([callback = std::move(callback)] {
            callback({{}, "database service is not running"});
        });
        return;
    }
    PendingEvent pending;
    pending.identities = std::make_unique<IdentityBatchTask>();
    pending.identities->revisionTrackIds = std::move(revisionTrackIds);
    pending.identities->callback = std::move(callback);
    if (!enqueue(std::move(pending))) {
        auto rejected = std::move(pending.identities->callback);
        fb2k::inMainThread([rejected = std::move(rejected)] {
            rejected({{}, "database service stopped before identity lookup"});
        });
    }
}

void DatabaseService::rebindTagRevisions(
    std::vector<TrackRevisionRebind> revisions,
    TrackRevisionRebindCallback callback)
{
    if (!started_.load(std::memory_order_acquire) || revisions.empty()) {
        if (callback) fb2k::inMainThread([callback = std::move(callback)] {
            callback(false, "database service is not running");
        });
        return;
    }
    PendingEvent pending;
    pending.revisionRebind = std::make_unique<RevisionRebindTask>();
    pending.revisionRebind->revisions = std::move(revisions);
    pending.revisionRebind->callback = std::move(callback);
    if (!enqueue(std::move(pending))) {
        // enqueue does not consume rejected events. Complete exactly once so
        // callers keeping admission closed cannot hang during shutdown.
        auto rejected = std::move(pending.revisionRebind->callback);
        if (rejected) fb2k::inMainThread([rejected = std::move(rejected)] {
            rejected(false, "database service stopped before identity binding");
        });
    }
}

void DatabaseService::loadLyrics(
    std::filesystem::path trackPath,
    PersistedLyricsBindingV2 binding,
    LyricsCacheCallback callback)
{
    if (!started_.load(std::memory_order_acquire)) {
        LyricsCachePayload payload;
        payload.error = "lyrics cache service is not running";
        if (callback) fb2k::inMainThread(
            [callback = std::move(callback), payload = std::move(payload)]() mutable {
                callback(std::move(payload));
            });
        return;
    }
    PendingEvent pending;
    pending.lyrics = std::make_unique<LyricsTask>();
    pending.lyrics->kind = LyricsTask::Kind::load;
    pending.lyrics->trackPath = std::move(trackPath);
    pending.lyrics->binding = std::move(binding);
    pending.lyrics->callback = std::move(callback);
    enqueue(std::move(pending));
}

void DatabaseService::storeLyrics(
    std::filesystem::path trackPath,
    PersistedLyricsDocumentV2 document,
    PersistedLyricsBindingV2 binding)
{
    if (!started_.load(std::memory_order_acquire)) return;
    PendingEvent pending;
    pending.lyrics = std::make_unique<LyricsTask>();
    pending.lyrics->kind = LyricsTask::Kind::store;
    pending.lyrics->trackPath = std::move(trackPath);
    pending.lyrics->document = std::move(document);
    pending.lyrics->binding = std::move(binding);
    enqueue(std::move(pending));
}

void DatabaseService::storeLyricsRejection(
    std::filesystem::path trackPath,
    PersistedLyricsRejectionV2 rejection)
{
    if (!started_.load(std::memory_order_acquire)) return;
    PendingEvent pending;
    pending.lyrics = std::make_unique<LyricsTask>();
    pending.lyrics->kind = LyricsTask::Kind::reject;
    pending.lyrics->trackPath = std::move(trackPath);
    pending.lyrics->binding.entityId = rejection.entityId;
    pending.lyrics->binding.subsong = rejection.subsong;
    pending.lyrics->binding.mediaRevision = rejection.mediaRevision;
    pending.lyrics->rejection = std::move(rejection);
    enqueue(std::move(pending));
}

std::shared_ptr<const HistorySnapshot> DatabaseService::historySnapshot() const
{
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    return snapshot_;
}

std::shared_ptr<const TrackUserStateSnapshot>
DatabaseService::userStateSnapshot() const
{
    std::lock_guard<std::mutex> lock(userStateMutex_);
    return userStateSnapshot_;
}

std::shared_ptr<const PersonaSnapshot> DatabaseService::personaSnapshot() const
{
    std::lock_guard<std::mutex> lock(personaMutex_);
    return personaSnapshot_;
}

bool DatabaseService::toggleCurrentFavorite()
{
    const auto current = userStateSnapshot();
    if (!current->available || current->trackId.empty()) return false;
    PendingEvent pending;
    pending.userState = std::make_unique<UserStateTask>();
    pending.userState->kind = UserStateTask::Kind::setFavorite;
    pending.userState->expectedTrackId = current->trackId;
    pending.userState->favorite = !current->favorite;
    pending.userState->commandId = nextCommandId_.fetch_add(
        1, std::memory_order_relaxed);
    publishUserCommandAccepted(pending.userState->commandId);
    enqueue(std::move(pending));
    return true;
}

bool DatabaseService::setCurrentNote(
    std::string note, const std::string& expectedTrackId)
{
    const auto current = userStateSnapshot();
    if (!current->available || current->trackId.empty() ||
        note.size() > 64U * 1024U ||
        (!expectedTrackId.empty() && current->trackId != expectedTrackId)) {
        return false;
    }
    PendingEvent pending;
    pending.userState = std::make_unique<UserStateTask>();
    pending.userState->kind = UserStateTask::Kind::setNote;
    pending.userState->expectedTrackId = current->trackId;
    pending.userState->note = std::move(note);
    pending.userState->commandId = nextCommandId_.fetch_add(
        1, std::memory_order_relaxed);
    publishUserCommandAccepted(pending.userState->commandId);
    enqueue(std::move(pending));
    return true;
}

bool DatabaseService::saveCurrentPersona(
    PersistedPersona persona, const std::string& expectedAliasKey)
{
    const auto current = personaSnapshot();
    if (!current->available || current->aliasKey.empty() ||
        (!expectedAliasKey.empty() && current->aliasKey != expectedAliasKey) ||
        persona.displayName.empty() || persona.aliases.empty()) {
        return false;
    }
    if (persona.personaId.empty()) persona.personaId = newPersonaId();
    if (persona.personaId.empty()) return false;
    for (auto& relationship : persona.relationships) {
        if (relationship.targetAliasKey.empty()) {
            relationship.targetAliasKey = personaAliasKey(
                utf8ToWide(relationship.targetDisplayName));
        }
        if (relationship.targetPersonaId.empty()) {
            relationship.targetPersonaId = newPersonaId();
        }
        if (relationship.targetPersonaId.empty() ||
            relationship.targetAliasKey.empty()) return false;
    }
    persona.updatedUtcMs = utcNowMs();
    PendingEvent pending;
    pending.persona = std::make_unique<PersonaTask>();
    pending.persona->expectedAliasKey = current->aliasKey;
    pending.persona->persona = std::move(persona);
    enqueue(std::move(pending));
    return true;
}

void DatabaseService::publishReady(const std::string& sessionId)
{
    auto previous = historySnapshot();
    auto next = std::make_shared<HistorySnapshot>(*previous);
    next->generation = previous->generation + 1;
    next->available = true;
    next->readAvailable = true;
    next->writerAvailable = true;
    next->health = DatabaseHealthState::ready;
    next->sessionId = sessionId;
    next->retryAttempt = 0;
    next->error.clear();
    next->queuedEvents = pendingPlaybackWriteCount();
    {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        snapshot_ = std::move(next);
    }
    notifyWorkspaceHistoryChanged();
}

void DatabaseService::publishError(std::string error,
    DatabaseHealthState health, unsigned retryAttempt)
{
    auto previous = historySnapshot();
    auto next = std::make_shared<HistorySnapshot>(*previous);
    ++next->generation;
    next->writerAvailable = false;
    next->available = next->readAvailable;
    next->health = health;
    next->retryAttempt = retryAttempt;
    next->error = std::move(error);
    next->queuedEvents = pendingPlaybackWriteCount();
    {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        snapshot_ = std::move(next);
    }
    notifyWorkspaceHistoryChanged();
}

void DatabaseService::publishEvent(
    const PersistedPlayEvent& event,
    const PersistedTrack& track)
{
    auto previous = historySnapshot();
    auto next = std::make_shared<HistorySnapshot>(*previous);
    ++next->generation;
    // A committed event is authoritative evidence that the writer recovered.
    next->available = true;
    next->readAvailable = true;
    next->writerAvailable = true;
    next->health = DatabaseHealthState::ready;
    next->retryAttempt = 0;
    next->error.clear();
    next->queuedEvents = pendingPlaybackWriteCount();
    HistoryEventRecord row;
    row.eventUid = event.eventUid;
    row.sessionId = event.sessionId;
    row.trackId = event.trackId;
    row.type = event.type;
    row.utcMs = event.utcMs;
    row.positionMs = event.positionMs;
    row.durationMs = event.durationMs;
    row.artist = track.artist;
    row.album = track.album;
    row.title = track.title;
    const auto position = std::upper_bound(next->events.begin(), next->events.end(),
        row.utcMs, [](std::int64_t utcMs, const HistoryEventRecord& existing) {
            return utcMs < existing.utcMs;
        });
    next->events.insert(position, std::move(row));
    if (next->events.size() > kHistorySnapshotLimit) {
        next->events.erase(next->events.begin(),
            next->events.begin() +
                static_cast<std::ptrdiff_t>(next->events.size() - kHistorySnapshotLimit));
    }
    {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        snapshot_ = std::move(next);
    }
    notifyWorkspaceHistoryChanged();
}

void DatabaseService::publishAnalysisResult(
    AnalysisCacheCallback callback, AnalysisCachePayload payload)
{
    if (!callback) {
        if (!payload.error.empty()) {
            const auto error = std::move(payload.error);
            fb2k::inMainThread([error] {
                FB2K_console_formatter() << "GODENplayer Core analysis cache: " << error.c_str();
            });
        }
        return;
    }
    auto shared = std::make_shared<AnalysisCachePayload>(std::move(payload));
    fb2k::inMainThread([callback = std::move(callback), shared] {
        callback(std::move(*shared));
    });
}

void DatabaseService::publishPacketCostResult(
    PacketCostCacheCallback callback, PacketCostCachePayload payload)
{
    if (!callback) {
        if (!payload.error.empty()) {
            const auto error = std::move(payload.error);
            fb2k::inMainThread([error] {
                FB2K_console_formatter() << "GODENplayer Core packet-cost cache: "
                                        << error.c_str();
            });
        }
        return;
    }
    auto shared = std::make_shared<PacketCostCachePayload>(std::move(payload));
    fb2k::inMainThread([callback = std::move(callback), shared] {
        callback(std::move(*shared));
    });
}

void DatabaseService::publishUserState(
    PersistedTrackUserState state, std::string error,
    std::uint64_t commandId,
    const std::shared_ptr<PlaybackContext>& expectedPlayback)
{
    auto previous = userStateSnapshot();
    auto next = error.empty()
        ? std::make_shared<TrackUserStateSnapshot>()
        : std::make_shared<TrackUserStateSnapshot>(*previous);
    next->generation = previous->generation + 1;
    next->available = error.empty() && state.found;
    next->stale = !error.empty();
    if (error.empty()) {
        next->entityId = std::move(state.entityId);
        next->canonicalTrackId = std::move(state.canonicalTrackId);
        next->revisionTrackId = std::move(state.currentRevisionTrackId);
        next->trackId = std::move(state.trackId);
        next->favorite = state.favorite;
        next->note = std::move(state.note);
    }
    next->error = std::move(error);
    if (commandId != 0) {
        next->commandId = commandId;
        next->commandState = next->error.empty()
            ? DatabaseCommandState::persisted
            : DatabaseCommandState::failed;
    }
    const std::shared_ptr<const TrackUserStateSnapshot> published = next;
    {
        std::unique_lock<std::mutex> admissionLock(queueMutex_, std::defer_lock);
        if (expectedPlayback) {
            admissionLock.lock();
            if (expectedPlayback != admittedPlayback_) return;
        }
        std::lock_guard<std::mutex> lock(userStateMutex_);
        userStateSnapshot_ = std::move(next);
    }
    if (published->available && !published->stale) {
        fb2k::inMainThread([this, published, expectedPlayback] {
            {
                std::lock_guard lock(queueMutex_);
                if (!started_.load(std::memory_order_acquire) || stopping_ ||
                    (expectedPlayback && expectedPlayback != admittedPlayback_)) return;
            }
            if (userStateSnapshot() != published) return;
            AlbumSessionService::instance().refreshPlayingIdentity();
        });
    }
    Runtime::instance().refreshPresentationState();
    notifyWorkspaceModelChanged();
    postListenRefresh();
}

void DatabaseService::publishUserCommandAccepted(std::uint64_t commandId)
{
    auto previous = userStateSnapshot();
    auto next = std::make_shared<TrackUserStateSnapshot>(*previous);
    ++next->generation;
    next->commandId = commandId;
    next->commandState = DatabaseCommandState::accepted;
    next->error.clear();
    {
        std::lock_guard<std::mutex> lock(userStateMutex_);
        userStateSnapshot_ = std::move(next);
    }
    Runtime::instance().refreshPresentationState();
    notifyWorkspaceModelChanged();
    postListenRefresh();
}

void DatabaseService::publishUserStateLoading()
{
    auto previous = userStateSnapshot();
    auto next = std::make_shared<TrackUserStateSnapshot>();
    next->generation = previous->generation + 1;
    {
        std::lock_guard<std::mutex> lock(userStateMutex_);
        userStateSnapshot_ = std::move(next);
    }
    Runtime::instance().refreshPresentationState();
    notifyWorkspaceModelChanged();
    postListenRefresh();
}

void DatabaseService::publishPersona(
    std::string artistAlias, std::string aliasKey,
    PersistedPersona persona, std::string error,
    const std::shared_ptr<PlaybackContext>& expectedPlayback)
{
    auto previous = personaSnapshot();
    auto next = std::make_shared<PersonaSnapshot>();
    next->generation = previous->generation + 1;
    next->available = error.empty();
    next->artistAlias = std::move(artistAlias);
    next->aliasKey = std::move(aliasKey);
    next->persona = std::move(persona);
    next->error = std::move(error);
    {
        std::unique_lock<std::mutex> admissionLock(queueMutex_, std::defer_lock);
        if (expectedPlayback) {
            admissionLock.lock();
            if (expectedPlayback != admittedPlayback_) return;
        }
        std::lock_guard<std::mutex> lock(personaMutex_);
        personaSnapshot_ = std::move(next);
    }
    notifyWorkspaceModelChanged();
}

void DatabaseService::publishPersonaLoading()
{
    auto previous = personaSnapshot();
    auto next = std::make_shared<PersonaSnapshot>();
    next->generation = previous->generation + 1;
    {
        std::lock_guard<std::mutex> lock(personaMutex_);
        personaSnapshot_ = std::move(next);
    }
    notifyWorkspaceModelChanged();
}

void DatabaseService::workerMain(
    std::filesystem::path databasePath,
    std::string sessionId)
{
    NekoDatabase database;
    std::string error;
    constexpr std::array retryDelays{
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(250),
        std::chrono::milliseconds(1000),
        std::chrono::milliseconds(4000),
    };
    bool opened = false;
    for (unsigned attempt = 0; attempt < retryDelays.size(); ++attempt) {
        if (retryDelays[attempt].count() != 0) {
            std::unique_lock<std::mutex> lock(queueMutex_);
            if (queueChanged_.wait_for(lock, retryDelays[attempt],
                    [this] { return stopping_; })) {
                break;
            }
        }
        error.clear();
        if (database.open(databasePath, error) &&
            database.beginSession(sessionId, utcNowMs(),
                build_identity::kVersion, error)) {
            opened = true;
            break;
        }
        database.close();
        const auto incompatible = error.find("schema is newer") != std::string::npos;
        publishError(error,
            incompatible ? DatabaseHealthState::incompatibleSchema
                         : DatabaseHealthState::transientError,
            attempt + 1U);
        if (incompatible) break;
    }
    if (opened) {
        publishReady(sessionId);
    }

    auto retainFailedPlaybackWrite = [this](PendingEvent pending,
            const std::string& failure) {
        ++pending.writeRetryAttempt;
        const auto decision = historyRetryDecision(
            failure, pending.writeRetryAttempt);
        const auto health = decision.failure ==
                HistoryFailureClass::incompatibleSchema
            ? DatabaseHealthState::incompatibleSchema
            : DatabaseHealthState::transientError;
        const auto attempt = pending.writeRetryAttempt;
        if (decision.retry) {
            pending.deferred = true;
            pending.readyAt = std::chrono::steady_clock::now() +
                decision.delay;
            enqueue(std::move(pending));
            publishError(failure, health, attempt);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            constexpr std::size_t kMaximumWriteSpool = 1024;
            if (writeSpool_.size() >= kMaximumWriteSpool)
                writeSpool_.pop_front();
            pending.deferred = false;
            pending.readyAt = {};
            writeSpool_.push_back(std::move(pending));
        }
        publishError(failure, health, attempt);
    };

    PersistedTrack currentTrack;
    bool hasCurrentTrack = false;
    while (true) {
        PendingEvent pending;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            while (true) {
                if (queue_.empty()) {
                    if (stopping_) break;
                    queueChanged_.wait(lock);
                    continue;
                }
                const auto now = std::chrono::steady_clock::now();
                auto ready = queue_.end();
                auto readyPriority = std::numeric_limits<unsigned>::max();
                auto earliest = std::chrono::steady_clock::time_point::max();
                for (auto iterator = queue_.begin(); iterator != queue_.end();
                     ++iterator) {
                    if (!iterator->deferred || iterator->readyAt <= now) {
                        // Interactive state/history owns the writer ahead of
                        // cache maintenance.  Reads which unblock a visible
                        // cost/waveform follow, then reproducible stores.
                        unsigned priority = 0;
                        if (iterator->retryDatabase) {
                            priority = 0;
                        } else if (!database.isOpen() &&
                            !iterator->analysis && !iterator->userState &&
                            !iterator->persona && !iterator->identities &&
                            !iterator->revisionRebind && !iterator->lyrics) {
                            priority = 1;
                        } else if (iterator->analysis) {
                            const auto kind = iterator->analysis->kind;
                            priority = kind == AnalysisTask::Kind::loadDeep ||
                                    kind == AnalysisTask::Kind::loadPacketCost
                                ? 1U : 2U;
                        } else if (iterator->lyrics) {
                            priority = iterator->lyrics->kind == LyricsTask::Kind::load
                                ? 1U : 2U;
                        }
                        if (ready == queue_.end() || priority < readyPriority) {
                            ready = iterator;
                            readyPriority = priority;
                        }
                        continue;
                    }
                    earliest = std::min(earliest, iterator->readyAt);
                }
                if (ready != queue_.end()) {
                    pending = std::move(*ready);
                    queue_.erase(ready);
                    break;
                }
                if (stopping_) {
                    pending = std::move(queue_.front());
                    queue_.pop_front();
                    break;
                }
                queueChanged_.wait_until(lock, earliest);
            }
            if (queue_.empty() && stopping_ && pending.utcMs == 0 &&
                !pending.analysis && !pending.userState && !pending.persona &&
                !pending.identities && !pending.revisionRebind &&
                !pending.lyrics && !pending.retryDatabase) {
                break;
            }
        }

        if (pending.retryDatabase) {
            if (database.isOpen()) {
                publishReady(sessionId);
                continue;
            }
            for (unsigned attempt = 0; attempt < retryDelays.size(); ++attempt) {
                if (retryDelays[attempt].count() != 0) {
                    std::unique_lock<std::mutex> lock(queueMutex_);
                    if (queueChanged_.wait_for(lock, retryDelays[attempt],
                            [this] { return stopping_; })) break;
                }
                error.clear();
                if (database.open(databasePath, error) &&
                    database.beginSession(sessionId, utcNowMs(),
                        build_identity::kVersion, error)) {
                    opened = true;
                    publishReady(sessionId);
                    break;
                }
                database.close();
                const auto incompatible =
                    error.find("schema is newer") != std::string::npos;
                publishError(error,
                    incompatible ? DatabaseHealthState::incompatibleSchema
                                 : DatabaseHealthState::transientError,
                    attempt + 1U);
                if (incompatible) break;
            }
            continue;
        }

        if (pending.lyrics) {
            auto task = std::move(pending.lyrics);
            LyricsCachePayload payload;
            std::string cacheError;
            if (!database.isOpen()) {
                cacheError = "database is not available";
            } else {
                PersistedTrack revision;
                TrackIdentityResolution identity;
                if (!task->trackPath.empty() &&
                    identifyLocalTrack(task->trackPath, revision, cacheError)) {
                    if (task->binding.subsong != 0) {
                        try {
                            const auto subsongIdentity = makeTrackId({task->trackPath,
                                task->binding.subsong, revision.fileSize,
                                revision.modifiedTicks});
                            revision.trackId = subsongIdentity.value;
                        } catch (...) {
                            cacheError = "lyrics subsong identity failed";
                        }
                    }
                }
                if (cacheError.empty() && !revision.trackId.empty() &&
                    database.resolveTrackIdentity(
                        revision.trackId, identity, cacheError) && identity.found) {
                    task->binding.entityId = identity.entityId;
                } else if (!cacheError.empty()) {
                    cacheError.clear();
                }
            }
            if (cacheError.empty() && database.isOpen() &&
                task->kind == LyricsTask::Kind::load) {
                database.readLyricsForBinding(task->binding.entityId,
                    task->binding.subsong, task->binding.mediaRevision,
                    task->binding.metadataRevision, payload.document,
                    payload.binding, cacheError);
                payload.found = payload.document.found;
                if (payload.found) {
                    database.readLyricsRejection(
                        task->binding.entityId,
                        task->binding.subsong,
                        task->binding.mediaRevision,
                        payload.document.documentId,
                        payload.rejection,
                        cacheError);
                }
            } else if (cacheError.empty() && database.isOpen() &&
                task->kind == LyricsTask::Kind::store) {
                database.storeLyricsDocumentAndBinding(task->document,
                    task->binding, cacheError);
            } else if (cacheError.empty() && database.isOpen()) {
                // Identity resolution above canonicalizes the entity for all
                // lyrics operations before persistence.
                if (!task->binding.entityId.empty()) {
                    task->rejection.entityId = task->binding.entityId;
                }
                database.storeLyricsRejection(task->rejection, cacheError);
            }
            payload.error = std::move(cacheError);
            if (task->callback) {
                auto shared = std::make_shared<LyricsCachePayload>(
                    std::move(payload));
                fb2k::inMainThread(
                    [callback = std::move(task->callback), shared]() mutable {
                        callback(std::move(*shared));
                    });
            } else if (!payload.error.empty()) {
                const auto message = std::move(payload.error);
                fb2k::inMainThread([message] {
                    FB2K_console_formatter() <<
                        "GODENplayer Core lyrics cache: " << message.c_str();
                });
            }
            continue;
        }

        if (pending.revisionRebind) {
            auto task = std::move(pending.revisionRebind);
            bool success = database.isOpen();
            std::string error = success ? std::string{} :
                "database is not available";
            std::vector<KnownRevisionBinding> bindings;
            bindings.reserve(task->revisions.size());
            for (const auto& request : task->revisions) {
                if (!success) break;
                TrackIdentityResolution previous;
                if (!database.resolveTrackIdentity(
                        request.previousRevisionTrackId, previous, error)) {
                    success = false;
                    break;
                }
                // A revision with no user-state entity needs no rebound. The
                // normal scanner will establish its canonical identity.
                if (!previous.found) continue;
                PersistedTrack current;
                if (request.frozenRevision) {
                    current = *request.frozenRevision;
                    if (current.trackId.empty() || current.path.empty() ||
                            current.subsong != request.subsong) {
                        error = "frozen media revision is incomplete";
                        success = false;
                        break;
                    }
                } else if (!identifyLocalTrack(request.currentPath, current, error)) {
                    success = false;
                    break;
                }
                if (!request.frozenRevision && request.subsong != 0) {
                    try {
                        const auto identity = makeTrackId({request.currentPath,
                            request.subsong, current.fileSize,
                            current.modifiedTicks});
                        current.trackId = identity.value;
                        current.path = identity.normalizedPathUtf8;
                        current.partialFingerprint = identity.partialFingerprint;
                        current.subsong = request.subsong;
                    } catch (const std::exception& exception) {
                        error = std::string("tag revision identity failed: ") +
                            exception.what();
                        success = false;
                        break;
                    }
                }
                bindings.push_back({std::move(current), previous.entityId});
            }
            // Prepare every identity before writing. A later invalid request or
            // SQLite failure must not leave earlier subsongs partially bound.
            if (success) success = database.bindKnownRevisions(bindings, error);
            // The merge can update state for an existing playing alias as well
            // as the newly bound revision. Refresh by entity after commit, and
            // only while this playback context still owns admission.
            if (success && hasCurrentTrack && !bindings.empty()) {
                std::shared_ptr<PlaybackContext> playing;
                {
                    std::lock_guard lock(queueMutex_);
                    playing = admittedPlayback_;
                }
                if (playing && playing->track &&
                        playing->track->trackId == currentTrack.trackId) {
                    PersistedTrackUserState state;
                    std::string stateError;
                    database.readTrackUserState(currentTrack.trackId, state, stateError);
                    if (!stateError.empty() || std::any_of(bindings.begin(), bindings.end(),
                            [&](const auto& binding) { return binding.entityId == state.entityId; })) {
                        publishUserState(std::move(state), std::move(stateError), 0, playing);
                    }
                }
            }
            if (task->callback) {
                fb2k::inMainThread([callback = std::move(task->callback),
                        success, error = std::move(error)]() mutable {
                    callback(success, std::move(error));
                });
            }
            continue;
        }

        if (pending.identities) {
            auto task = std::move(pending.identities);
            TrackIdentityBatchPayload payload;
            if (!database.isOpen()) {
                payload.error = "database is not available";
            } else {
                payload.identities.reserve(task->revisionTrackIds.size());
                for (const auto& revision : task->revisionTrackIds) {
                    TrackIdentityResolution identity;
                    // Album may lack a revision for an individual source.
                    // Preserve that slot without invalidating valid siblings.
                    if (!revision.empty() && !database.resolveTrackIdentity(
                            revision, identity, payload.error)) {
                        if (payload.error.empty()) payload.error = "identity query failed";
                        payload.identities.clear();
                        break;
                    }
                    identity.revisionTrackId = revision;
                    payload.identities.push_back(std::move(identity));
                }
            }
            auto shared = std::make_shared<TrackIdentityBatchPayload>(std::move(payload));
            fb2k::inMainThread([callback = std::move(task->callback), shared] {
                callback(std::move(*shared));
            });
            continue;
        }

        if (pending.analysis) {
            auto task = std::move(pending.analysis);
            PersistedTrack analysisTrack;
            std::string identityError;
            if (!database.isOpen()) identityError = "database is not available";
            else identifyLocalTrack(task->trackPath, analysisTrack, identityError);

            const auto isPacketCost =
                task->kind == AnalysisTask::Kind::loadPacketCost ||
                task->kind == AnalysisTask::Kind::storePacketCost;
            if (isPacketCost) {
                PacketCostCachePayload payload;
                payload.trackPath = task->trackPath;
                payload.sourceTrackId = analysisTrack.trackId;
                payload.error = identityError;
                if (payload.error.empty() &&
                    task->kind == AnalysisTask::Kind::loadPacketCost) {
                    PersistedAnalysisBundle cached;
                    if (!database.readAnalysisBundle(analysisTrack.trackId,
                            std::string(kPacketCostAnalyzerVersion), cached,
                            payload.error)) {
                        // Corrupt reproducible layers are removed by NekoDatabase.
                    } else if (cached.found) {
                        const PersistedAnalysisLayer* packetLayer = nullptr;
                        for (const auto& layer : cached.layers) {
                            if (layer.name == "packet-cost") packetLayer = &layer;
                        }
                        PacketCostResult restored;
                        if (packetLayer == nullptr ||
                            !deserializePacketCost(packetLayer->data, restored,
                                payload.error)) {
                            std::string removeError;
                            database.removeAnalysisBundle(analysisTrack.trackId,
                                std::string(kPacketCostAnalyzerVersion), removeError);
                            if (payload.error.empty()) {
                                payload.error = "cached packet-cost layer is missing";
                            }
                        } else {
                            payload.cost = std::make_shared<PacketCostResult>(
                                std::move(restored));
                            payload.found = true;
                        }
                    }
                } else if (payload.error.empty()) {
                    if (task->expectedPacketCostTrackId.empty() ||
                        task->expectedPacketCostTrackId != analysisTrack.trackId) {
                        // The file can change after the reader completes but
                        // before this queued write runs. Never label its old
                        // result with the newly observed file identity.
                        payload.error = "packet-cost source changed before cache store";
                    } else if (task->packetCost == nullptr) {
                        payload.error = "packet-cost store payload is missing";
                    } else {
                        std::vector<std::byte> encoded;
                        if (serializePacketCost(*task->packetCost, encoded,
                                payload.error)) {
                            PersistedAnalysisBundle bundle;
                            bundle.trackId = analysisTrack.trackId;
                            bundle.analyzerVersion =
                                std::string(kPacketCostAnalyzerVersion);
                            bundle.updatedUtcMs = utcNowMs();
                            bundle.lastAccessUtcMs = bundle.updatedUtcMs;
                            const auto resolution = task->packetCost->levels.empty()
                                ? 0 : task->packetCost->levels.front()
                                    .resolutionMilliseconds;
                            bundle.layers.push_back({"packet-cost",
                                static_cast<std::int64_t>(resolution),
                                std::move(encoded)});
                            std::ostringstream manifest;
                            manifest << "{\"schema\":\"nekorox.packet-cost-manifest.v1\","
                                     << "\"supported\":"
                                     << (task->packetCost->supported ? "true" : "false")
                                     << ",\"codec\":"
                                     << static_cast<unsigned>(task->packetCost->codec)
                                     << ",\"durationSeconds\":"
                                     << task->packetCost->durationSeconds
                                     << ",\"levels\":"
                                     << task->packetCost->levels.size() << '}';
                            bundle.manifestJson = manifest.str();
                            analysisTrack.durationMs = secondsToMs(
                                task->packetCost->durationSeconds);
                            database.storeAnalysisBundle(analysisTrack, bundle,
                                kAnalysisCacheLimitBytes, payload.error);
                        }
                    }
                }
                publishPacketCostResult(std::move(task->packetCostCallback),
                    std::move(payload));
            } else {
                AnalysisCachePayload payload;
                payload.trackPath = task->trackPath;
                payload.error = identityError;
                if (payload.error.empty() && task->kind == AnalysisTask::Kind::storeDeep &&
                    (task->expectedDeepAnalysisTrackId.empty() ||
                        task->expectedDeepAnalysisTrackId != analysisTrack.trackId)) {
                    payload.error = "analysis source changed before cache commit";
                }
                if (payload.error.empty() &&
                    task->kind == AnalysisTask::Kind::loadDeep) {
                    PersistedAnalysis cached;
                    if (!database.readAnalysis(analysisTrack.trackId,
                            std::string(kDeepAnalysisVersion), cached,
                            payload.error)) {
                        // Corrupt reproducible layers are removed by NekoDatabase.
                    } else if (cached.found) {
                        if (cached.waveform100ms.size() <
                            sizeof(AnalysisMediaInfoV1)) {
                            payload.error = "cached waveform header is truncated";
                        } else {
                            std::memcpy(&payload.media,
                                cached.waveform100ms.data(), sizeof(payload.media));
                            const auto expected = sizeof(payload.media) +
                                static_cast<std::size_t>(
                                    payload.media.waveformBinCount) *
                                    sizeof(AnalysisWaveformBinV1);
                            if (expected != cached.waveform100ms.size()) {
                                payload.error =
                                    "cached waveform payload is inconsistent";
                            } else {
                                auto waveform = std::make_shared<
                                    std::vector<AnalysisWaveformBinV1>>(
                                        payload.media.waveformBinCount);
                                if (!waveform->empty()) {
                                    std::memcpy(waveform->data(),
                                        cached.waveform100ms.data() +
                                            sizeof(payload.media),
                                        waveform->size() *
                                            sizeof(waveform->front()));
                                }
                                payload.waveform = std::move(waveform);
                                payload.resultJson.assign(
                                    reinterpret_cast<const char*>(
                                        cached.resultJson.data()),
                                    cached.resultJson.size());
                                payload.found = true;
                            }
                        }
                        if (!payload.error.empty()) {
                            std::string removeError;
                            database.removeAnalysisBundle(analysisTrack.trackId,
                                std::string(kDeepAnalysisVersion), removeError);
                        }
                    }
                } else if (payload.error.empty()) {
                    static const std::vector<AnalysisWaveformBinV1> emptyWaveform;
                    const auto& waveform = task->waveform != nullptr
                        ? *task->waveform : emptyWaveform;
                    task->media.waveformBinCount =
                        static_cast<std::uint32_t>(waveform.size());
                    PersistedAnalysis analysis;
                    analysis.trackId = analysisTrack.trackId;
                    analysis.analyzerVersion = std::string(kDeepAnalysisVersion);
                    analysis.updatedUtcMs = utcNowMs();
                    analysis.lastAccessUtcMs = analysis.updatedUtcMs;
                    analysis.resultJson.assign(
                        reinterpret_cast<const std::byte*>(task->resultJson.data()),
                        reinterpret_cast<const std::byte*>(task->resultJson.data() +
                            task->resultJson.size()));
                    analysis.waveform100ms.resize(sizeof(task->media) +
                        waveform.size() * sizeof(AnalysisWaveformBinV1));
                    std::memcpy(analysis.waveform100ms.data(), &task->media,
                        sizeof(task->media));
                    if (!waveform.empty()) {
                        std::memcpy(
                            analysis.waveform100ms.data() + sizeof(task->media),
                            waveform.data(),
                            waveform.size() * sizeof(waveform.front()));
                    }
                    std::ostringstream manifest;
                    manifest << "{\"schema\":\"nekorox.analysis-manifest.v1\","
                             << "\"sampleRate\":" << task->media.sampleRate << ','
                             << "\"sampleCount\":" << task->media.sampleCount << ','
                             << "\"durationSeconds\":"
                             << task->media.durationSeconds << ','
                             << "\"waveformBins\":"
                             << task->media.waveformBinCount << '}';
                    analysis.manifestJson = manifest.str();
                    analysisTrack.durationMs = secondsToMs(
                        task->media.durationSeconds);
                    database.storeAnalysis(analysisTrack, analysis,
                        kAnalysisCacheLimitBytes, payload.error);
                }
                publishAnalysisResult(std::move(task->callback),
                    std::move(payload));
            }
            continue;
        }

        if (pending.userState) {
            auto task = std::move(pending.userState);
            PersistedTrackUserState state;
            std::string stateError;
            if (!database.isOpen()) {
                stateError = "database is not available";
            } else if (!hasCurrentTrack ||
                currentTrack.trackId != task->expectedTrackId) {
                stateError = "current track changed before the user-state command";
            } else {
                const auto updated = utcNowMs();
                const bool stored = task->kind == UserStateTask::Kind::setFavorite
                    ? database.setFavorite(
                        currentTrack, task->favorite, updated, stateError)
                    : database.setNote(currentTrack, task->note, updated, stateError);
                if (stored) {
                    database.readTrackUserState(
                        currentTrack.trackId, state, stateError);
                }
            }
            publishUserState(std::move(state), std::move(stateError),
                task->commandId, pending.playbackContext);
            continue;
        }

        if (pending.persona) {
            auto task = std::move(pending.persona);
            PersistedPersona persona;
            std::string personaError;
            const auto artistAlias = hasCurrentTrack ? currentTrack.artist : std::string{};
            const auto aliasKey = personaAliasKey(utf8ToWide(artistAlias));
            if (!database.isOpen()) {
                personaError = "database is not available";
            } else if (!hasCurrentTrack || aliasKey.empty() ||
                aliasKey != task->expectedAliasKey) {
                personaError = "current artist changed before the persona command";
            } else if (database.upsertPersona(task->persona, personaError)) {
                database.readPersonaByAliasKey(aliasKey, persona, personaError);
            }
            publishPersona(artistAlias, aliasKey,
                std::move(persona), std::move(personaError), pending.playbackContext);
            continue;
        }

        const auto context = pending.playbackContext;
        if (!context || !context->seed) continue;
        if (!context->resolutionAttempted) {
            context->resolutionAttempted = true;
            const auto& seed = *context->seed;
            PersistedTrack resolved;
            std::error_code statusError;
            const auto size = std::filesystem::file_size(seed.nativePath, statusError);
            const auto fileSize = statusError ? 0 : size;
            statusError.clear();
            const auto modified = std::filesystem::last_write_time(
                seed.nativePath, statusError);
            const auto modifiedTicks = statusError ? 0 : fileTimeTicks(modified);
            try {
                const auto identity = makeTrackId({
                    seed.nativePath, seed.subsong, fileSize, modifiedTicks});
                resolved.trackId = identity.value;
                resolved.path = identity.normalizedPathUtf8;
                resolved.partialFingerprint = identity.partialFingerprint;
            } catch (const std::exception& exception) {
                const auto message = std::string(
                    "track identity failed (database remains ready): ") +
                    exception.what();
                fb2k::inMainThread([message] {
                    FB2K_console_formatter() << "GODENplayer History: "
                        << message.c_str();
                });
                continue;
            }
            resolved.artist = seed.artist;
            resolved.album = seed.album;
            resolved.title = seed.title;
            resolved.subsong = seed.subsong;
            resolved.fileSize = fileSize;
            resolved.modifiedTicks = modifiedTicks;
            resolved.durationMs = seed.durationMs;
            context->track = std::move(resolved);
        }
        if (!context->track) continue;
        if (!pending.playbackProcessed) {
            pending.playbackProcessed = true;
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (context == admittedPlayback_) {
                currentTrack = *context->track;
                hasCurrentTrack = true;
            }
        }
        // Resolve before checking the database: even writes retained while it
        // is unavailable must keep their original track and file revision.
        if (!database.isOpen()) {
            retainFailedPlaybackWrite(std::move(pending),
                error.empty() ? "database is not available" : error);
            continue;
        }
        auto eventTrack = *context->track;
        PersistedPlayEvent event;
        event.eventUid = pending.eventUid;
        event.sessionId = sessionId;
        event.trackId = eventTrack.trackId;
        event.type = pending.type;
        event.utcMs = pending.utcMs;
        event.positionMs = pending.positionMs;
        event.durationMs = pending.durationMs > 0
            ? pending.durationMs : eventTrack.durationMs;
        event.playlistIndex = pending.playlistIndex;
        event.itemIndex = pending.itemIndex;
        eventTrack.lastSeenUtcMs = pending.utcMs;
        bool inserted = false;
        if (!database.recordPlayEvent(eventTrack, event, error, &inserted)) {
            retainFailedPlaybackWrite(std::move(pending), error);
            continue;
        }
        if (inserted) publishEvent(event, eventTrack);
        if (!context->initialStatePublished) {
            PersistedTrackUserState state;
            std::string stateError;
            database.readTrackUserState(
                eventTrack.trackId, state, stateError);
            const bool userStateRead = stateError.empty();
            publishUserState(std::move(state), std::move(stateError), 0, context);
            PersistedPersona persona;
            std::string personaError;
            const auto aliasKey = personaAliasKey(
                utf8ToWide(eventTrack.artist));
            if (!aliasKey.empty()) {
                database.readPersonaByAliasKey(
                    aliasKey, persona, personaError);
            }
            context->initialStatePublished = userStateRead && personaError.empty();
            publishPersona(eventTrack.artist, aliasKey,
                std::move(persona), std::move(personaError), context);
        }
    }

    if (database.isOpen()) {
        if (!database.endSession(sessionId, utcNowMs(), error)) publishError(error);
        database.close();
    }
}

} // namespace nekorox::component
