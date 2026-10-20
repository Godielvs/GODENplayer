#pragma once

#include "nekorox/recording_identity.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nekorox {

enum class TimingStatus : std::uint8_t {
    staticText,
    catalogMatched,
    audioVerified,
    rejected,
};

enum class LyricsState : std::uint8_t {
    loadingLocal,
    searching,
    ready,
    instrumentalReported,
    noReliableMatch,
    offline,
    rateLimited,
    failed,
};

enum class LyricsSourceRank : std::uint8_t {
    none = 0,
    staleCache = 1,
    remote = 2,
    cache = 3,
    local = 4,
    user = 5,
};

struct LyricsCue {
    std::int64_t startMs = 0;
    std::optional<std::int64_t> endMs;
    std::vector<std::string> simultaneousLines;

    [[nodiscard]] bool operator==(const LyricsCue&) const = default;
};

struct LyricsDocumentV2 {
    std::string documentId;
    std::string textHash;
    std::string timingHash;
    SourceVersion source;
    RecordingKey binding;
    std::string plainTextUtf8;
    std::vector<LyricsCue> cues;
    TimingStatus timing = TimingStatus::staticText;
    std::vector<std::string> reasonCodes;
    std::string sourceDisplayName;
    std::string attribution;
    std::string sourceUrl;

    [[nodiscard]] bool operator==(const LyricsDocumentV2&) const = default;
};

struct LyricsSnapshot {
    // Monotonic presentation identity. Request generation identifies work;
    // this revision identifies every observable state/document publication
    // within that request.
    std::uint64_t presentationRevision = 0;
    RequestToken request;
    MetadataRevision metadata;
    LyricsState state = LyricsState::loadingLocal;
    std::shared_ptr<const LyricsDocumentV2> document;
    bool staleCache = false;
    LyricsSourceRank sourceRank = LyricsSourceRank::none;
    std::uint64_t sourceRevision = 0;
    bool refreshing = false;
    bool timingUsable = false;
};

} // namespace nekorox
