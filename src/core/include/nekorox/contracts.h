#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <string_view>

namespace nekorox {

inline constexpr std::uint32_t kPlaybackSnapshotVersionV1 = 1;
inline constexpr std::uint32_t kPlaybackSnapshotVersionV2 = 2;
inline constexpr std::uint32_t kPlaybackPositionSnapshotVersionV1 = 1;
inline constexpr std::uint32_t kRealtimeRingVersion = 3;
inline constexpr std::uint32_t kAnalysisProtocolVersion = 1;
inline constexpr std::uint32_t kListenCommandVersion = 2;
inline constexpr std::uint32_t kThemeSnapshotVersionV1 = 1;
inline constexpr std::uint32_t kThemeSnapshotVersionV2 = 2;
inline constexpr std::uint32_t kAnalysisFrameMagic = 0x4F524B4E;
inline constexpr std::uint32_t kAnalysisMaxPayloadBytes = 128U * 1024U * 1024U;
inline constexpr std::string_view kDeepAnalysisVersion =
    "libsonare-1.5.2+nekorox.2";

inline constexpr std::size_t kRealtimeMaxChannels = 8;
inline constexpr std::size_t kRealtimeMaxFrames = 4096;
inline constexpr std::size_t kRealtimeMaxSamples =
    kRealtimeMaxChannels * kRealtimeMaxFrames;
inline constexpr std::size_t kPlaybackMetadataBytes = 256;
inline constexpr std::size_t kPlaybackPathBytes = 1024;

enum class PlaybackState : std::uint8_t {
    stopped = 0,
    playing = 1,
    paused = 2,
};

struct PlaybackSnapshotV1 {
    std::uint32_t version = kPlaybackSnapshotVersionV1;
    std::uint32_t sizeBytes = 0;
    std::uint64_t generation = 0;
    PlaybackState state = PlaybackState::stopped;
    std::uint8_t reservedState[7]{};
    double positionSeconds = 0.0;
    double durationSeconds = 0.0;
    std::uint32_t playlistIndex = 0;
    std::uint32_t playlistItemIndex = 0;
    std::uint32_t subsong = 0;
    std::uint32_t flags = 0;
    std::uint64_t trackIdentityHash = 0;
    std::array<char, kPlaybackMetadataBytes> title{};
    std::array<char, kPlaybackMetadataBytes> artist{};
    std::array<char, kPlaybackMetadataBytes> album{};
    std::array<char, kPlaybackPathBytes> path{};
};

inline constexpr std::uint32_t kPlaybackFlagMuted = 1U << 0;
inline constexpr std::uint32_t kPlaybackFlagFavorite = 1U << 1;
inline constexpr std::uint32_t kPlaybackFlagFavoriteAvailable = 1U << 2;

struct PlaybackSnapshotV2 {
    std::uint32_t version = kPlaybackSnapshotVersionV2;
    std::uint32_t sizeBytes = 0;
    std::uint64_t generation = 0;
    PlaybackState state = PlaybackState::stopped;
    std::uint8_t reservedState[3]{};
    std::uint32_t playbackOrder = 0;
    double positionSeconds = 0.0;
    double durationSeconds = 0.0;
    std::uint32_t playlistIndex = 0;
    std::uint32_t playlistItemIndex = 0;
    std::uint32_t subsong = 0;
    std::uint32_t flags = 0;
    std::uint64_t trackIdentityHash = 0;
    std::uint64_t queueRevision = 0;
    std::uint64_t artworkIdentityHash = 0;
    float volumeDb = 0.0f;
    std::uint32_t reserved = 0;
    std::array<char, kPlaybackMetadataBytes> title{};
    std::array<char, kPlaybackMetadataBytes> artist{};
    std::array<char, kPlaybackMetadataBytes> album{};
    std::array<char, kPlaybackPathBytes> path{};
};

// High-frequency playback clock published independently from semantic state.
// The individual fields are mirrored by Runtime atomics; this trivially
// copyable DTO is intentionally not persisted and can be sampled every frame.
struct PlaybackPositionSnapshotV1 {
    std::uint32_t version = kPlaybackPositionSnapshotVersionV1;
    std::uint32_t sizeBytes = sizeof(PlaybackPositionSnapshotV1);
    double positionSeconds = 0.0;
    std::uint64_t qpcTicks = 0;
    std::uint64_t positionRevision = 0;
    std::uint64_t seekGeneration = 0;
};

enum class ListenCommandKind : std::uint16_t {
    seek = 1,
    analyze = 2,
    cancelAnalysis = 3,
    toggleFavorite = 4,
    navigate = 5,
};

enum class ListenSpace : std::uint32_t {
    overview = 0,
    live = 1,
};

#pragma pack(push, 1)
struct ListenCommandV1 {
    std::uint32_t version = kListenCommandVersion;
    std::uint16_t sizeBytes = sizeof(ListenCommandV1);
    ListenCommandKind kind = ListenCommandKind::navigate;
    std::uint64_t requestId = 0;
    double value = 0.0;
    std::uint32_t index = 0;
    std::uint32_t flags = 0;
};

struct RgbaColorV1 {
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float alpha = 1.0f;
};

struct ThemeSnapshotV1 {
    std::uint32_t version = kThemeSnapshotVersionV1;
    std::uint32_t sizeBytes = sizeof(ThemeSnapshotV1);
    std::uint64_t generation = 0;
    std::uint64_t artworkIdentityHash = 0;
    RgbaColorV1 base{};
    RgbaColorV1 panel{};
    RgbaColorV1 text{};
    RgbaColorV1 accent{};
    RgbaColorV1 secondary{};
    RgbaColorV1 highlight{};
    float minimumContrast = 4.5f;
    std::uint32_t flags = 0;
};

struct ThemeSnapshotV2 {
    std::uint32_t version = kThemeSnapshotVersionV2;
    std::uint32_t sizeBytes = sizeof(ThemeSnapshotV2);
    std::uint64_t generation = 0;
    std::uint64_t artworkIdentityHash = 0;
    std::uint64_t artworkGeneration = 0;
    RgbaColorV1 base{};
    RgbaColorV1 panel{};
    RgbaColorV1 text{};
    RgbaColorV1 accent{};
    RgbaColorV1 secondary{};
    RgbaColorV1 highlight{};
    float minimumContrast = 4.5f;
    float transitionProgress = 1.0f;
    std::uint32_t flags = 0;
    std::uint32_t reserved = 0;
};

// Public V3 name retained by the architecture contract. ThemeSnapshotV2 is
// kept as the short source-compatible spelling used by the component.
using UiThemeSnapshotV2 = ThemeSnapshotV2;
#pragma pack(pop)

inline constexpr std::uint32_t kThemeFlagArtworkDerived = 1U << 0;

[[nodiscard]] constexpr bool validPlaybackSnapshot(
    const PlaybackSnapshotV1& snapshot) noexcept
{
    return snapshot.version == kPlaybackSnapshotVersionV1 &&
        snapshot.sizeBytes == sizeof(PlaybackSnapshotV1) &&
        static_cast<std::uint8_t>(snapshot.state) <=
            static_cast<std::uint8_t>(PlaybackState::paused);
}

[[nodiscard]] constexpr bool validPlaybackSnapshot(
    const PlaybackSnapshotV2& snapshot) noexcept
{
    return snapshot.version == kPlaybackSnapshotVersionV2 &&
        snapshot.sizeBytes == sizeof(PlaybackSnapshotV2) &&
        static_cast<std::uint8_t>(snapshot.state) <=
            static_cast<std::uint8_t>(PlaybackState::paused) &&
        (snapshot.flags & ~(kPlaybackFlagMuted |
            kPlaybackFlagFavorite |
            kPlaybackFlagFavoriteAvailable)) == 0;
}

[[nodiscard]] constexpr bool validPlaybackPositionSnapshot(
    const PlaybackPositionSnapshotV1& snapshot) noexcept
{
    return snapshot.version == kPlaybackPositionSnapshotVersionV1 &&
        snapshot.sizeBytes == sizeof(PlaybackPositionSnapshotV1) &&
        snapshot.positionSeconds >= 0.0;
}

[[nodiscard]] constexpr bool validListenCommand(
    const ListenCommandV1& command) noexcept
{
    return command.version == kListenCommandVersion &&
        command.sizeBytes == sizeof(ListenCommandV1) &&
        static_cast<std::uint16_t>(command.kind) >=
            static_cast<std::uint16_t>(ListenCommandKind::seek) &&
        static_cast<std::uint16_t>(command.kind) <=
            static_cast<std::uint16_t>(ListenCommandKind::navigate) &&
        (command.kind != ListenCommandKind::navigate ||
            command.index <= static_cast<std::uint32_t>(ListenSpace::live));
}

[[nodiscard]] constexpr bool validThemeSnapshot(
    const ThemeSnapshotV1& snapshot) noexcept
{
    return snapshot.version == kThemeSnapshotVersionV1 &&
        snapshot.sizeBytes == sizeof(ThemeSnapshotV1) &&
        snapshot.minimumContrast >= 1.0f;
}

[[nodiscard]] constexpr bool validThemeSnapshot(
    const ThemeSnapshotV2& snapshot) noexcept
{
    return snapshot.version == kThemeSnapshotVersionV2 &&
        snapshot.sizeBytes == sizeof(ThemeSnapshotV2) &&
        snapshot.minimumContrast >= 1.0f &&
        snapshot.transitionProgress >= 0.0f &&
        snapshot.transitionProgress <= 1.0f;
}

struct RealtimeFrameV1 {
    std::uint32_t version = kRealtimeRingVersion;
    std::uint64_t sequence = 0;
    std::uint64_t streamGeneration = 0;
    // Authoritative playback admission stamped before PCM publication. Unlike
    // streamGeneration, this does not change for seek/pause/resume/format
    // boundaries, so history consumers can distinguish a new recording from
    // a discontinuity without sampling mutable UI state after the fact.
    std::uint64_t playbackSerial = 0;
    std::uint64_t recordingIdentityHash = 0;
    std::uint64_t firstFrameIndex = 0;
    std::uint64_t qpcTicks = 0;
    double playbackSeconds = -1.0;
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
    std::uint16_t flags = 0;
    std::uint32_t discontinuityReasonMask = 0;
    std::uint32_t frameCount = 0;
    std::uint32_t sampleCount = 0;
    std::array<float, kRealtimeMaxSamples> samples{};
};

inline constexpr std::uint16_t kRealtimeFrameFlagPcm = 1U << 0U;
inline constexpr std::uint16_t kRealtimeFrameFlagDiscontinuity = 1U << 1U;

enum class AnalysisMessageKind : std::uint16_t {
    start = 1,
    cancel = 2,
    pcm = 3,
    end = 4,
    progress = 5,
    partialResult = 6,
    complete = 7,
    error = 8,
};

enum class AnalysisStage : std::uint16_t {
    queued = 0,
    decode = 1,
    waveform = 2,
    features = 3,
    bpm = 4,
    key = 5,
    beats = 6,
    harmony = 7,
    segmentation = 8,
    timbre = 9,
    dynamics = 10,
    rhythm = 11,
    melody = 12,
    stabilization = 13,
    complete = 14,
};

#pragma pack(push, 1)
struct AnalysisFrameHeaderV1 {
    std::uint32_t magic = kAnalysisFrameMagic; // NKRO in little endian.
    std::uint16_t version = kAnalysisProtocolVersion;
    AnalysisMessageKind kind = AnalysisMessageKind::error;
    std::uint32_t payloadBytes = 0;
    std::uint64_t requestId = 0;
};

struct AnalysisProgressV1 {
    AnalysisStage stage = AnalysisStage::queued;
    std::uint16_t reserved = 0;
    float stageProgress = 0.0f;
    float overallProgress = 0.0f;
    std::uint64_t completedUnits = 0;
    std::uint64_t totalUnits = 0;
};

struct AnalysisMediaInfoV1 {
    std::uint32_t sampleRate = 0;
    std::uint32_t waveformResolutionMilliseconds = 100;
    std::uint64_t sampleCount = 0;
    double durationSeconds = 0.0;
    std::uint32_t waveformBinCount = 0;
    std::uint32_t reserved = 0;
};

struct AnalysisWaveformBinV1 {
    float peak = 0.0f;
    float rms = 0.0f;
};
#pragma pack(pop)

static_assert(sizeof(AnalysisFrameHeaderV1) == 20);
static_assert(sizeof(AnalysisProgressV1) == 28);
static_assert(sizeof(AnalysisMediaInfoV1) == 32);
static_assert(sizeof(AnalysisWaveformBinV1) == 8);
static_assert(sizeof(ListenCommandV1) == 32);
static_assert(sizeof(ThemeSnapshotV1) == 128);
static_assert(sizeof(ThemeSnapshotV2) == 144);
static_assert(std::is_trivially_copyable_v<PlaybackSnapshotV1>);
static_assert(std::is_trivially_copyable_v<PlaybackSnapshotV2>);
static_assert(std::is_trivially_copyable_v<ListenCommandV1>);
static_assert(std::is_trivially_copyable_v<ThemeSnapshotV1>);
static_assert(std::is_trivially_copyable_v<ThemeSnapshotV2>);

[[nodiscard]] constexpr bool validAnalysisFrameHeader(
    const AnalysisFrameHeaderV1& header) noexcept
{
    return header.magic == kAnalysisFrameMagic &&
        header.version == kAnalysisProtocolVersion &&
        header.payloadBytes <= kAnalysisMaxPayloadBytes;
}

} // namespace nekorox
