#pragma once

#include "nekorox/lyrics_match_policy.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nekorox::component {

enum class LyricsRequestReason : std::uint8_t {
    newTrack = 0,
    refresh = 1,
};

struct LyricsRequestContext {
    LyricsMetadata metadata;
    RequestToken request;
    std::filesystem::path nativePath;
    std::filesystem::path profilePath;
    std::string embeddedPlain;
    std::string embeddedSynced;
    bool onlineEnabled = true;
    bool offlineMode = false;
    LyricsRequestReason reason = LyricsRequestReason::newTrack;
};

enum class LyricsProviderOutcome : std::uint8_t {
    candidates,
    notFound,
    offline,
    rateLimited,
    networkError,
    invalidResponse,
    cancelled,
};

struct LyricsProviderResult {
    LyricsProviderOutcome outcome = LyricsProviderOutcome::notFound;
    std::vector<LyricsCandidateV2> candidates;
    std::string reason;
    std::chrono::seconds retryAfter{0};
    unsigned logicalRequests = 0;
};

} // namespace nekorox::component
