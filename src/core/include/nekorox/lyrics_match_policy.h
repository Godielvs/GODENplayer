#pragma once

#include "nekorox/lyrics_document.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nekorox {

inline constexpr std::uint32_t kLyricsMatchPolicyVersion = 1;
inline constexpr std::size_t kMaximumLyricsBytes = 512U * 1024U;
inline constexpr std::size_t kMaximumLyricsLines = 4096U;
inline constexpr std::size_t kMaximumLyricsTimestampTokens = 16384U;
inline constexpr std::size_t kMaximumLyricsCueGroups = 2048U;

enum class LyricsVersionKind : std::uint8_t {
    live,
    remix,
    edit,
    demo,
    acoustic,
    instrumental,
    spedUp,
    slowed,
    remaster,
};

struct LyricsVersionQualifier {
    LyricsVersionKind kind = LyricsVersionKind::live;
    std::string value;

    [[nodiscard]] bool operator==(const LyricsVersionQualifier&) const = default;
};

struct NormalizedLyricsIdentity {
    std::string title;
    std::string artist;
    std::string album;
    std::vector<LyricsVersionQualifier> versions;
};

struct LyricsMetadata {
    RecordingKey recording;
    MetadataRevision metadata;
    std::string title;
    std::string artist;
    std::string album;
    std::optional<double> durationSeconds;
    std::string recordingMbid;
    std::string releaseMbid;
};

struct LyricsCandidateV2 {
    SourceVersion source;
    std::string title;
    std::string artist;
    std::string album;
    std::optional<double> durationSeconds;
    std::string plainLyrics;
    std::string syncedLyrics;
    bool instrumental = false;
    std::string sourceDisplayName;
    std::string attribution;
    std::string sourceUrl;
};

struct LyricsCandidateEvaluation {
    int score = 0;
    bool textEligible = false;
    bool timingEligible = false;
    bool remasterUncertain = false;
    std::vector<std::string> reasonCodes;
};

struct LyricsSelection {
    std::optional<std::size_t> candidateIndex;
    LyricsCandidateEvaluation evaluation;
    bool ambiguous = false;
    std::vector<std::string> reasonCodes;
};

struct LyricsParseResult {
    std::shared_ptr<const LyricsDocumentV2> document;
    bool validPayload = false;
    bool completeTiming = false;
    std::vector<std::string> reasonCodes;
};

[[nodiscard]] NormalizedLyricsIdentity normalizeLyricsIdentity(
    std::string_view title,
    std::string_view artist,
    std::string_view album);

[[nodiscard]] LyricsCandidateEvaluation evaluateLyricsCandidate(
    const LyricsMetadata& wanted,
    const LyricsCandidateV2& candidate);

[[nodiscard]] LyricsSelection selectLyricsCandidate(
    const LyricsMetadata& wanted,
    const std::vector<LyricsCandidateV2>& candidates);

[[nodiscard]] LyricsParseResult parseLyricsDocumentV2(
    const LyricsMetadata& wanted,
    const LyricsCandidateV2& candidate,
    const LyricsCandidateEvaluation& evaluation);

[[nodiscard]] std::optional<std::size_t> activeLyricsCueIndex(
    const LyricsDocumentV2& document,
    std::int64_t playbackMs) noexcept;

} // namespace nekorox
