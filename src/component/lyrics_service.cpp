#include "lyrics_service.h"
#include "titleformat_cache.h"

#include "database_service.h"
#include "runtime.h"
#include "text_util.h"
#include "ui_invalidation_bus.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <thread>

namespace nekorox::component {
namespace {

const GUID kLyricsOnlineGuid = {0x8309a80a, 0x591f, 0x4aeb,
    {0x96, 0x7d, 0xc0, 0x73, 0xed, 0xb5, 0x83, 0x7c}};
const GUID kLyricsOfflineGuid = {0x0a4a959b, 0xf299, 0x487f,
    {0xae, 0xca, 0x7b, 0x8c, 0xea, 0xa7, 0x17, 0xdd}};
cfg_bool gLyricsOnline(kLyricsOnlineGuid, true);
cfg_bool gLyricsOffline(kLyricsOfflineGuid, false);

std::uint64_t hashAppend(std::uint64_t hash, std::string_view value) noexcept
{
    constexpr std::uint64_t prime = 1099511628211ULL;
    for (const auto byte : value) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= prime;
    }
    return hash;
}

std::uint64_t hashField(std::uint64_t hash, std::string_view value) noexcept
{
    hash = hashAppend(hash, std::to_string(value.size()));
    hash = hashAppend(hash, ":");
    return hashAppend(hash, value);
}

std::string hex64(std::uint64_t value)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

std::string metadataValue(const metadb_info_container::ptr& container,
    std::initializer_list<const char*> names)
{
    if (!container.is_valid()) return {};
    try {
        const auto& info = container->info();
        for (const auto* name : names) {
            const auto count = info.meta_get_count_by_name(name);
            for (t_size index = 0; index < count; ++index) {
                const auto* value = info.meta_get(name, index);
                if (value != nullptr && *value != '\0') return value;
            }
        }
    } catch (...) {
    }
    return {};
}

std::string formatted(const metadb_handle_ptr& track,
    const char* expression)
{
    if (!track.is_valid()) return {};
    try {
        const auto script = cachedTitleFormat(expression);
        pfc::string8_fast value;
        if (script.is_valid() && track->format_title(
                nullptr, value, script, nullptr)) return value.c_str();
    } catch (...) {
    }
    return {};
}

std::filesystem::path nativePath(const metadb_handle_ptr& track)
{
    if (!track.is_valid()) return {};
    try {
        const auto native = filesystem::g_get_native_path(track->get_path());
        return utf8ToWide(native.c_str());
    } catch (...) {
        return {};
    }
}

std::filesystem::path profilePath()
{
    try {
        const auto native = filesystem::g_get_native_path(core_api::get_profile_path());
        return utf8ToWide(native.c_str());
    } catch (...) {
        return {};
    }
}

std::int64_t utcNowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string joinReasons(const std::vector<std::string>& reasons)
{
    std::string value;
    for (const auto& reason : reasons) {
        if (!value.empty()) value.push_back('\n');
        value += reason;
    }
    return value;
}

std::string negativeKey(const LyricsRequestContext& request)
{
    return request.metadata.recording.entityId + '\x1f' +
        std::to_string(request.metadata.recording.subsong) + '\x1f' +
        request.metadata.recording.mediaRevision + '\x1f' +
        std::to_string(request.metadata.metadata.value);
}

std::string rejectionKey(
    const LyricsRequestContext& request, std::string_view documentId)
{
    return request.metadata.recording.entityId + '\x1f' +
        std::to_string(request.metadata.recording.subsong) + '\x1f' +
        request.metadata.recording.mediaRevision + '\x1f' +
        std::string(documentId);
}

LyricsCandidateV2 candidateFromPersisted(
    const PersistedLyricsDocumentV2& cached)
{
    LyricsCandidateV2 candidate;
    candidate.source = {cached.provider, cached.providerRecordId,
        cached.providerRevision};
    candidate.title = cached.title;
    candidate.artist = cached.artist;
    candidate.album = cached.album;
    if (cached.durationMs > 0)
        candidate.durationSeconds = cached.durationMs / 1000.0;
    candidate.plainLyrics = cached.plainTextUtf8;
    candidate.syncedLyrics = cached.syncedTextUtf8;
    candidate.instrumental = cached.instrumental;
    candidate.sourceDisplayName = cached.sourceDisplayName;
    candidate.attribution = cached.attribution;
    candidate.sourceUrl = cached.sourceUrl;
    return candidate;
}

bool sameLyricsPresentation(
    const LyricsSnapshot& left,
    const LyricsSnapshot& right) noexcept
{
    const auto sameDocument = [&] {
        if (left.document == right.document) return true;
        if (!left.document || !right.document) return false;
        return *left.document == *right.document;
    }();
    return left.request == right.request &&
        left.metadata == right.metadata &&
        left.state == right.state &&
        sameDocument &&
        left.staleCache == right.staleCache &&
        left.sourceRank == right.sourceRank &&
        left.sourceRevision == right.sourceRevision &&
        left.refreshing == right.refreshing &&
        left.timingUsable == right.timingUsable;
}

} // namespace

LyricsService::LyricsService()
{
    snapshot_.store(std::make_shared<LyricsSnapshot>(),
        std::memory_order_release);
}

LyricsService::~LyricsService() { stop(); }

LyricsService& LyricsService::instance()
{
    static LyricsService service;
    return service;
}

bool LyricsService::start()
{
    if (started_.exchange(true)) return true;
    refreshCurrent();
    return true;
}

void LyricsService::stop() noexcept
{
    if (!started_.exchange(false)) return;
    generation_.fetch_add(1, std::memory_order_acq_rel);
    worker_.shutdownAndDrain();
}

LyricsRequestContext LyricsService::capture(
    const metadb_handle_ptr& track,
    std::uint64_t generation) const
{
    LyricsRequestContext result;
    result.request.generation = generation;
    if (!track.is_valid()) return result;
    result.nativePath = nativePath(track);
    result.profilePath = profilePath();
    result.metadata.title = formatted(track, "$if2(%title%,$filename(%path%))");
    result.metadata.artist = formatted(track, "$if2(%artist%,Unknown artist)");
    result.metadata.album = formatted(track, "%album%");
    const auto length = track->get_length();
    if (std::isfinite(length) && length > 0.0)
        result.metadata.durationSeconds = length;
    result.metadata.recording.subsong = static_cast<std::uint32_t>(
        track->get_subsong_index());
    const std::string sourcePath = track->get_path();
    auto identityHash = hashField(1469598103934665603ULL, sourcePath);
    identityHash = hashField(identityHash,
        std::to_string(result.metadata.recording.subsong));
    result.metadata.recording.entityId = "recording:v1:" + hex64(identityHash);
    // File stat is intentionally deferred to LatestWorker. Admission must be
    // immediate and must not touch storage on the foobar main thread.
    result.metadata.recording.mediaRevision = "pending-v1";
    auto metadataHash = hashField(1469598103934665603ULL,
        result.metadata.title);
    metadataHash = hashField(metadataHash, result.metadata.artist);
    metadataHash = hashField(metadataHash, result.metadata.album);
    metadataHash = hashField(metadataHash,
        result.metadata.durationSeconds
            ? std::to_string(*result.metadata.durationSeconds) : std::string{});
    result.metadata.metadata.value = metadataHash;
    result.request.recording = result.metadata.recording;
    return result;
}

bool LyricsService::isCurrent(const RequestToken& request) const noexcept
{
    const auto current = snapshot();
    return started_.load(std::memory_order_acquire) && current &&
        current->request == request &&
        generation_.load(std::memory_order_acquire) == request.generation;
}

void LyricsService::onNewTrack(const metadb_handle_ptr& track)
{
    beginRequest(track, LyricsRequestReason::newTrack);
}

void LyricsService::beginRequest(const metadb_handle_ptr& track,
    LyricsRequestReason reason)
{
    if (!started_.load(std::memory_order_acquire)) return;
    worker_.cancelPending();
    const auto generation = generation_.fetch_add(
        1, std::memory_order_acq_rel) + 1U;
    cacheSatisfiedGeneration_.store(0, std::memory_order_release);
    auto request = capture(track, generation);
    request.reason = reason;
    request.onlineEnabled = onlineEnabled();
    request.offlineMode = offlineMode();
    if (request.metadata.title.empty() || request.metadata.artist.empty()) {
        LyricsSnapshot empty;
        empty.request = request.request;
        empty.metadata = request.metadata.metadata;
        empty.state = LyricsState::noReliableMatch;
        publish(std::move(empty));
        worker_.cancelPending();
        return;
    }
    // Retain the immutable primary metadata snapshot; copy potentially large
    // embedded lyrics only on the worker, without re-reading a newer track.
    metadb_info_container::ptr info;
    try { info = track->get_info_ref(); } catch (...) {}
    publishAdmission(request, LyricsState::loadingLocal);
    const auto submitted = worker_.submit([this, request, info = std::move(info)](
            std::stop_token stop) mutable {
        resolveMediaRevision(std::move(request), std::move(info), stop);
    });
    if (!submitted) publishState(request, LyricsState::failed);
}

void LyricsService::onMetadataChanged(const metadb_handle_ptr& track)
{
    metadb_handle_ptr current = track;
    if (!current.is_valid())
        (void)playback_control::get()->get_now_playing(current);
    beginRequest(current, LyricsRequestReason::refresh);
}

void LyricsService::onStop() noexcept
{
    const auto generation = generation_.fetch_add(
        1, std::memory_order_acq_rel) + 1U;
    worker_.cancelPending();
    LyricsSnapshot empty;
    empty.request.generation = generation;
    empty.state = LyricsState::noReliableMatch;
    publish(std::move(empty));
}

std::shared_ptr<const LyricsSnapshot> LyricsService::snapshot() const noexcept
{
    return snapshot_.load(std::memory_order_acquire);
}

void LyricsService::subscribe(HWND window, UINT message)
{
    if (window == nullptr || message == 0) return;
    std::lock_guard lock(subscriberMutex_);
    const auto found = std::find_if(subscribers_.begin(), subscribers_.end(),
        [window](const auto& value) { return value.window == window; });
    if (found == subscribers_.end()) subscribers_.push_back({window, message});
    else found->message = message;
}

void LyricsService::unsubscribe(HWND window) noexcept
{
    std::lock_guard lock(subscriberMutex_);
    subscribers_.erase(std::remove_if(subscribers_.begin(), subscribers_.end(),
        [window](const auto& value) { return value.window == window; }),
        subscribers_.end());
}

bool LyricsService::onlineEnabled() const noexcept { return gLyricsOnline; }
bool LyricsService::offlineMode() const noexcept { return gLyricsOffline; }

void LyricsService::setOnlineEnabled(bool enabled)
{
    if (static_cast<bool>(gLyricsOnline) == enabled) return;
    gLyricsOnline = enabled;
    refreshCurrent();
}

void LyricsService::setOfflineMode(bool offline)
{
    if (static_cast<bool>(gLyricsOffline) == offline) return;
    gLyricsOffline = offline;
    refreshCurrent();
}

std::wstring LyricsService::statusText() const
{
    const auto current = snapshot();
    if (!current) return L"Lyrics unavailable";
    const auto source = current->document &&
        !current->document->sourceDisplayName.empty()
        ? L" · " + utf8ToWide(current->document->sourceDisplayName)
        : std::wstring{};
    switch (current->state) {
    case LyricsState::loadingLocal: return L"Checking local lyrics…";
    case LyricsState::searching: return L"Searching lyrics…";
    case LyricsState::ready:
        return current->document && current->document->timing ==
                TimingStatus::catalogMatched
            ? L"Synced lyrics" + source + L" · catalog match"
            : L"Lyrics without reliable sync" + source;
    case LyricsState::instrumentalReported:
        return L"Instrumental according to source" + source;
    case LyricsState::noReliableMatch: return L"No reliable match";
    case LyricsState::offline: return L"Offline · local/cache only";
    case LyricsState::rateLimited: return L"Lyrics provider rate limited";
    case LyricsState::failed: return L"Lyrics temporarily unavailable";
    }
    return L"Lyrics unavailable";
}

void LyricsService::publish(LyricsSnapshot next)
{
    if (next.request.generation != generation_.load(std::memory_order_acquire))
        return;
    const auto current = snapshot();
    if (current && sameLyricsPresentation(*current, next)) return;
    next.presentationRevision = presentationRevision_.fetch_add(
        1, std::memory_order_acq_rel) + 1U;
    auto shared = std::make_shared<const LyricsSnapshot>(std::move(next));
    snapshot_.store(shared, std::memory_order_release);
    std::vector<Subscriber> subscribers;
    {
        std::lock_guard lock(subscriberMutex_);
        subscribers = subscribers_;
    }
    for (const auto& subscriber : subscribers) {
        if (IsWindow(subscriber.window))
            PostMessageW(subscriber.window, subscriber.message, 0, 0);
    }
    UiInvalidationBus::instance().publish(
        UiInvalidationSurface::workspace, UiDirty::semantic);
}

void LyricsService::publishAdmission(const LyricsRequestContext& request,
    LyricsState state)
{
    if (!started_.load(std::memory_order_acquire) ||
        request.request.generation !=
            generation_.load(std::memory_order_acquire)) return;
    LyricsSnapshot next;
    next.request = request.request;
    next.metadata = request.metadata.metadata;
    next.state = state;
    next.refreshing = request.reason == LyricsRequestReason::refresh;
    if (next.refreshing) {
        const auto current = snapshot();
        const auto sameRecording = current && current->document &&
            current->request.recording.entityId ==
                request.request.recording.entityId &&
            current->request.recording.subsong ==
                request.request.recording.subsong;
        if (sameRecording) {
            auto preserved = std::make_shared<LyricsDocumentV2>(
                *current->document);
            preserved->binding = request.request.recording;
            const auto revisionKnown =
                !request.request.recording.mediaRevision.empty();
            const auto sameRevision = revisionKnown &&
                current->document->binding.mediaRevision ==
                    request.request.recording.mediaRevision;
            if (!sameRevision) {
                preserved->timing = TimingStatus::staticText;
                preserved->reasonCodes.push_back(
                    "refresh_pending_media_revision");
            }
            next.document = std::move(preserved);
            next.staleCache = true;
            next.sourceRank = current->sourceRank;
            next.sourceRevision = current->sourceRevision;
            next.timingUsable = sameRevision &&
                next.document->timing != TimingStatus::rejected;
        }
    }
    publish(std::move(next));
}

bool LyricsService::promoteResolvedRequest(
    const LyricsRequestContext& request)
{
    if (!core_api::is_main_thread()) return false;
    const auto current = snapshot();
    if (!started_.load(std::memory_order_acquire) || !current ||
        current->request.generation != request.request.generation ||
        current->request.recording.entityId !=
            request.request.recording.entityId ||
        current->request.recording.subsong !=
            request.request.recording.subsong ||
        current->metadata != request.metadata.metadata ||
        generation_.load(std::memory_order_acquire) !=
            request.request.generation) return false;
    publishAdmission(request, LyricsState::loadingLocal);
    return isCurrent(request.request);
}

void LyricsService::resolveMediaRevision(
    LyricsRequestContext request, metadb_info_container::ptr info,
    std::stop_token stop)
{
    std::error_code error;
    std::uintmax_t size = 0;
    auto modified = std::filesystem::file_time_type{};
    bool statAvailable = !request.nativePath.empty();
    if (statAvailable) {
        size = std::filesystem::file_size(request.nativePath, error);
        statAvailable = !error;
        error.clear();
        modified = std::filesystem::last_write_time(request.nativePath, error);
        statAvailable = statAvailable && !error;
    }
    request.metadata.recording.mediaRevision = !statAvailable
        ? "stream-v1"
        : std::to_string(size) + ":" +
            std::to_string(modified.time_since_epoch().count());
    request.request.recording = request.metadata.recording;
    if (stop.stop_requested()) return;
    fb2k::inMainThread([this, request = std::move(request),
            info = std::move(info)]() mutable {
        if (!promoteResolvedRequest(request)) return;
        requestCached(request);
        const auto submitted = worker_.submit(
            [this, request, info = std::move(info)](
                    std::stop_token innerStop) mutable {
                runRequest(std::move(request), std::move(info), innerStop);
            });
        if (!submitted) publishState(request, LyricsState::failed);
    });
}

void LyricsService::publishState(const LyricsRequestContext& request,
    LyricsState state, std::shared_ptr<const LyricsDocumentV2> document,
    bool staleCache, LyricsSourceRank sourceRank,
    std::uint64_t sourceRevision)
{
    if (!core_api::is_main_thread()) {
        fb2k::inMainThread([this, request, state,
            document = std::move(document), staleCache, sourceRank,
            sourceRevision]() mutable {
            publishState(request, state, std::move(document), staleCache,
                sourceRank, sourceRevision);
        });
        return;
    }
    if (!isCurrent(request.request)) return;
    const auto current = snapshot();
    if (!current || current->metadata != request.metadata.metadata) return;
    LyricsSnapshot next = *current;
    next.state = state;
    if (document) {
        if (sourceRank < current->sourceRank ||
            (sourceRank == current->sourceRank &&
                sourceRevision < current->sourceRevision)) return;
        next.document = std::move(document);
        next.staleCache = staleCache;
        next.sourceRank = sourceRank;
        next.sourceRevision = sourceRevision;
    }
    next.refreshing = state == LyricsState::loadingLocal ||
        state == LyricsState::searching;
    next.timingUsable = next.document &&
        next.document->binding == request.request.recording &&
        next.document->timing != TimingStatus::rejected &&
        next.document->timing != TimingStatus::staticText;
    publish(std::move(next));
}

std::shared_ptr<const LyricsDocumentV2> LyricsService::resolve(
    const LyricsRequestContext& request,
    const std::vector<LyricsCandidateV2>& candidates) const
{
    const auto selection = selectLyricsCandidate(request.metadata, candidates);
    if (!selection.candidateIndex) return {};
    const auto parsed = parseLyricsDocumentV2(request.metadata,
        candidates[*selection.candidateIndex], selection.evaluation);
    if (!parsed.validPayload || !parsed.document) return {};
    const auto rejection = rememberedRejection(
        request, parsed.document->documentId);
    if (rejection == "wrong_document") return {};
    if (rejection == "bad_timing") {
        auto staticDocument = std::make_shared<LyricsDocumentV2>(
            *parsed.document);
        staticDocument->timing = TimingStatus::staticText;
        staticDocument->reasonCodes.push_back("user_rejected_timing");
        return staticDocument;
    }
    return parsed.document;
}

LyricsProviderResult LyricsService::lookupLocal(
    const LyricsRequestContext& request, metadb_info_container::ptr info,
    std::stop_token stop)
{
    if (stop.stop_requested() || !isCurrent(request.request)) {
        LyricsProviderResult cancelled;
        cancelled.outcome = LyricsProviderOutcome::cancelled;
        return cancelled;
    }
    // Keep the bodies confined to local lookup. Cache, publication and remote
    // callbacks need identity and title metadata, not copies of embedded text.
    auto localRequest = request;
    localRequest.embeddedSynced = metadataValue(info,
        {"SYNCEDLYRICS", "SYNCED LYRICS", "SYNCED LYRICS UTF8"});
    if (localRequest.embeddedSynced.empty()) {
        localRequest.embeddedPlain = metadataValue(info,
            {"UNSYNCEDLYRICS", "UNSYNCED LYRICS", "LYRICS"});
    }
    info.release();
    return local_.lookup(localRequest, stop);
}

void LyricsService::runRequest(
    LyricsRequestContext request, metadb_info_container::ptr info,
    std::stop_token stop)
{
    const auto local = lookupLocal(request, std::move(info), stop);
    if (stop.stop_requested() || !isCurrent(request.request)) return;
    if (!local.candidates.empty()) {
        const auto selection = selectLyricsCandidate(
            request.metadata, local.candidates);
        if (selection.candidateIndex) {
            const auto& candidate = local.candidates[*selection.candidateIndex];
            const auto parsed = parseLyricsDocumentV2(
                request.metadata, candidate, selection.evaluation);
            if (parsed.document && rememberedRejection(
                    request, parsed.document->documentId) !=
                    "wrong_document") {
                auto document = parsed.document;
                if (rememberedRejection(request, document->documentId) ==
                        "bad_timing") {
                    auto replacement = std::make_shared<LyricsDocumentV2>(
                        *document);
                    replacement->timing = TimingStatus::staticText;
                    replacement->reasonCodes.push_back(
                        "user_rejected_timing");
                    document = std::move(replacement);
                }
                publishState(request, candidate.instrumental
                    ? LyricsState::instrumentalReported : LyricsState::ready,
                    document, false, LyricsSourceRank::local,
                    static_cast<std::uint64_t>(utcNowMs()));
                persist(request, candidate, *parsed.document);
                return;
            }
        }
    }
    const auto stableUntil = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(800);
    while (!stop.stop_requested() &&
        std::chrono::steady_clock::now() < stableUntil) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (stop.stop_requested() || !isCurrent(request.request)) return;
    if (cacheSatisfiedGeneration_.load(std::memory_order_acquire) ==
        request.request.generation) return;
    if (request.offlineMode || !request.onlineEnabled) {
        publishState(request, LyricsState::offline);
        return;
    }
    const auto key = negativeKey(request);
    {
        std::lock_guard lock(negativeMutex_);
        if (const auto found = negative_.find(key); found != negative_.end() &&
            found->second.expires > std::chrono::steady_clock::now()) {
            publishState(request, found->second.state);
            return;
        }
    }
    publishState(request, LyricsState::searching);
    const auto remote = remote_.lookup(request, stop);
    if (stop.stop_requested() || !isCurrent(request.request)) return;
    if (remote.outcome == LyricsProviderOutcome::candidates) {
        const auto selection = selectLyricsCandidate(
            request.metadata, remote.candidates);
        if (selection.candidateIndex) {
            const auto& candidate = remote.candidates[*selection.candidateIndex];
            const auto parsed = parseLyricsDocumentV2(
                request.metadata, candidate, selection.evaluation);
            if (parsed.document && rememberedRejection(
                    request, parsed.document->documentId) !=
                    "wrong_document") {
                auto document = parsed.document;
                if (rememberedRejection(request, document->documentId) ==
                        "bad_timing") {
                    auto replacement = std::make_shared<LyricsDocumentV2>(
                        *document);
                    replacement->timing = TimingStatus::staticText;
                    replacement->reasonCodes.push_back(
                        "user_rejected_timing");
                    document = std::move(replacement);
                }
                publishState(request, candidate.instrumental
                    ? LyricsState::instrumentalReported : LyricsState::ready,
                    document, false, LyricsSourceRank::remote,
                    static_cast<std::uint64_t>(utcNowMs()));
                persist(request, candidate, *parsed.document);
                return;
            }
        }
        publishState(request, LyricsState::noReliableMatch);
        return;
    }
    LyricsState state = LyricsState::failed;
    auto ttl = std::chrono::seconds(60);
    unsigned validMisses = 0;
    if (remote.outcome == LyricsProviderOutcome::notFound) {
        state = LyricsState::noReliableMatch;
        {
            std::lock_guard lock(negativeMutex_);
            if (const auto found = negative_.find(key);
                found != negative_.end()) {
                validMisses = found->second.validMisses;
            }
        }
        ++validMisses;
        ttl = std::chrono::duration_cast<std::chrono::seconds>(
            validMisses >= 2 ? std::chrono::hours(24 * 7)
                             : std::chrono::hours(24));
    } else if (remote.outcome == LyricsProviderOutcome::rateLimited) {
        state = LyricsState::rateLimited;
        ttl = std::max(std::chrono::seconds(1), remote.retryAfter);
    } else if (remote.outcome == LyricsProviderOutcome::offline) {
        state = LyricsState::offline;
    }
    {
        std::lock_guard lock(negativeMutex_);
        negative_[key] = {
            state, std::chrono::steady_clock::now() + ttl, validMisses};
    }
    publishState(request, state);
}

void LyricsService::requestCached(const LyricsRequestContext& request)
{
    PersistedLyricsBindingV2 key;
    key.entityId = request.metadata.recording.entityId;
    key.subsong = request.metadata.recording.subsong;
    key.mediaRevision = request.metadata.recording.mediaRevision;
    key.metadataRevision = request.metadata.metadata.value;
    DatabaseService::instance().loadLyrics(request.nativePath, std::move(key),
        [this, request](LyricsCachePayload payload) {
            if (!payload.found || !payload.error.empty() ||
                !isCurrent(request.request)) return;
            // Local files/tags are re-read by runRequest on every admission.
            // A persistent copy cannot prove that its source still exists or
            // contains the same text; legacy embedded IDs also collided.
            if (payload.document.provider == "local" &&
                payload.document.documentId.rfind("local:v2:", 0) != 0) return;
            if (payload.rejection.found) {
                rememberRejection(request, payload.document.documentId,
                    payload.rejection.reason);
                if (payload.rejection.reason == "wrong_document") {
                    publishState(request, LyricsState::noReliableMatch);
                    return;
                }
            }
            const auto candidate = candidateFromPersisted(payload.document);
            const std::vector candidates{candidate};
            const auto document = resolve(request, candidates);
            if (!document) return;
            const auto expired = payload.document.provider == "local" ||
                payload.document.expiryUtcMs < utcNowMs();
            if (!expired) cacheSatisfiedGeneration_.store(
                request.request.generation, std::memory_order_release);
            publishState(request, candidate.instrumental
                ? LyricsState::instrumentalReported : LyricsState::ready,
                document, expired, expired ? LyricsSourceRank::staleCache
                                           : LyricsSourceRank::cache,
                static_cast<std::uint64_t>(std::max<std::int64_t>(
                    0, payload.document.fetchedUtcMs)));
        });
}

void LyricsService::persist(const LyricsRequestContext& request,
    const LyricsCandidateV2& candidate,
    const LyricsDocumentV2& document)
{
    PersistedLyricsDocumentV2 stored;
    stored.documentId = document.documentId;
    stored.provider = candidate.source.provider;
    stored.providerRecordId = candidate.source.recordId;
    stored.providerRevision = candidate.source.revision;
    stored.title = candidate.title;
    stored.artist = candidate.artist;
    stored.album = candidate.album;
    stored.durationMs = candidate.durationSeconds
        ? static_cast<std::int64_t>(std::llround(*candidate.durationSeconds * 1000.0))
        : 0;
    stored.plainTextUtf8 = candidate.plainLyrics;
    stored.syncedTextUtf8 = candidate.syncedLyrics;
    stored.instrumental = candidate.instrumental;
    stored.textHash = document.textHash;
    stored.timingHash = document.timingHash;
    stored.timingStatus = static_cast<std::int32_t>(document.timing);
    stored.reasonCodes = joinReasons(document.reasonCodes);
    stored.sourceDisplayName = candidate.sourceDisplayName;
    stored.attribution = candidate.attribution;
    stored.sourceUrl = candidate.sourceUrl;
    stored.fetchedUtcMs = utcNowMs();
    stored.expiryUtcMs = candidate.source.provider == "local"
        ? (std::numeric_limits<std::int64_t>::max)()
        : stored.fetchedUtcMs + std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::hours(24 * 30)).count();
    stored.policyVersion = kLyricsMatchPolicyVersion;
    PersistedLyricsBindingV2 binding;
    binding.entityId = request.metadata.recording.entityId;
    binding.subsong = request.metadata.recording.subsong;
    binding.mediaRevision = request.metadata.recording.mediaRevision;
    binding.metadataRevision = request.metadata.metadata.value;
    binding.documentId = stored.documentId;
    binding.state = static_cast<std::int32_t>(candidate.instrumental
        ? LyricsState::instrumentalReported : LyricsState::ready);
    binding.timingStatus = static_cast<std::int32_t>(document.timing);
    binding.reasonCodes = stored.reasonCodes;
    binding.updatedUtcMs = stored.fetchedUtcMs;
    DatabaseService::instance().storeLyrics(request.nativePath,
        std::move(stored), std::move(binding));
}

void LyricsService::refreshCurrent()
{
    if (!started_.load(std::memory_order_acquire)) return;
    metadb_handle_ptr current;
    if (playback_control::get()->get_now_playing(current)) onNewTrack(current);
    else onStop();
}

std::string LyricsService::rememberedRejection(
    const LyricsRequestContext& request,
    std::string_view documentId) const
{
    std::lock_guard lock(rejectionMutex_);
    const auto found = rejections_.find(rejectionKey(request, documentId));
    return found == rejections_.end() ? std::string{} : found->second;
}

void LyricsService::rememberRejection(
    const LyricsRequestContext& request,
    std::string documentId,
    std::string reason)
{
    std::lock_guard lock(rejectionMutex_);
    rejections_[rejectionKey(request, documentId)] = std::move(reason);
}

void LyricsService::persistRejection(
    const LyricsSnapshot& current,
    std::string reason)
{
    if (!current.document) return;
    PersistedLyricsRejectionV2 rejection;
    rejection.entityId = current.request.recording.entityId;
    rejection.subsong = current.request.recording.subsong;
    rejection.mediaRevision = current.request.recording.mediaRevision;
    rejection.documentId = current.document->documentId;
    rejection.textHash = current.document->textHash;
    rejection.timingHash = current.document->timingHash;
    rejection.reason = std::move(reason);
    rejection.rejectedUtcMs = utcNowMs();
    metadb_handle_ptr track;
    std::filesystem::path path;
    if (playback_control::get()->get_now_playing(track)) path = nativePath(track);
    DatabaseService::instance().storeLyricsRejection(
        std::move(path), std::move(rejection));
}

bool LyricsService::markCurrentWrongLyrics()
{
    const auto current = snapshot();
    if (!current || !current->document) return false;
    LyricsRequestContext request;
    request.request = current->request;
    request.metadata.recording = current->request.recording;
    request.metadata.metadata = current->metadata;
    rememberRejection(
        request, current->document->documentId, "wrong_document");
    persistRejection(*current, "wrong_document");
    LyricsSnapshot rejected = *current;
    rejected.state = LyricsState::noReliableMatch;
    rejected.document.reset();
    rejected.sourceRank = LyricsSourceRank::none;
    rejected.sourceRevision = 0;
    rejected.staleCache = false;
    publish(std::move(rejected));
    return true;
}

bool LyricsService::showCurrentWithoutSync()
{
    const auto current = snapshot();
    if (!current || !current->document) return false;
    LyricsRequestContext request;
    request.request = current->request;
    request.metadata.recording = current->request.recording;
    request.metadata.metadata = current->metadata;
    rememberRejection(request, current->document->documentId, "bad_timing");
    persistRejection(*current, "bad_timing");
    auto document = std::make_shared<LyricsDocumentV2>(*current->document);
    document->timing = TimingStatus::staticText;
    document->reasonCodes.push_back("user_rejected_timing");
    LyricsSnapshot staticLyrics = *current;
    staticLyrics.document = std::move(document);
    staticLyrics.sourceRank = LyricsSourceRank::user;
    staticLyrics.sourceRevision = static_cast<std::uint64_t>(utcNowMs());
    staticLyrics.state = LyricsState::ready;
    publish(std::move(staticLyrics));
    return true;
}

void LyricsService::retryCurrent()
{
    const auto current = snapshot();
    if (current) {
        LyricsRequestContext request;
        request.request = current->request;
        request.metadata.recording = current->request.recording;
        request.metadata.metadata = current->metadata;
        std::lock_guard lock(negativeMutex_);
        negative_.erase(negativeKey(request));
    }
    refreshCurrent();
}

} // namespace nekorox::component
