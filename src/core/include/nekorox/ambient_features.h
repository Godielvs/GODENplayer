#pragma once

#include "contracts.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace nekorox {

inline constexpr std::uint32_t kAmbientFeaturesVersionV1 = 1;
inline constexpr std::size_t kAmbientBandCount = 3;

enum class AmbientBand : std::uint8_t {
    low = 0,
    mid = 1,
    high = 2,
};

enum class AmbientSourceKind : std::uint8_t {
    playback = 0,
    unsupported = 1,
};

inline constexpr std::uint32_t kAmbientFeatureValid = 1U << 0U;
inline constexpr std::uint32_t kAmbientFeatureDiscontinuity = 1U << 1U;

// Immutable worker-to-UI payload. Keep this compact so the fixed snapshot pool
// can publish without allocations or ownership of PCM buffers.
struct AmbientFeaturesV1 {
    std::uint32_t version = kAmbientFeaturesVersionV1;
    std::uint32_t sizeBytes = sizeof(AmbientFeaturesV1);
    std::uint64_t publicationRevision = 0;
    std::uint64_t sourceSequence = 0;
    std::uint64_t streamGeneration = 0;
    std::uint64_t firstFrame = 0;
    std::uint64_t endFrame = 0;
    std::uint64_t sourceEndQpc = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
    AmbientSourceKind sourceKind = AmbientSourceKind::playback;
    std::uint8_t reserved = 0;
    std::array<float, kAmbientBandCount> level{};
    std::array<float, kAmbientBandCount> bandDb{-120.0f, -120.0f, -120.0f};
    // Raw mean-square band energy. Flow A3 performs its own adaptive temporal
    // normalization and therefore must not consume the older display envelope.
    std::array<double, kAmbientBandCount> bandPower{};
    // C2's shared musical contract. Relative values intentionally have a
    // neutral baseline near 1; consumers must apply `gate` before treating
    // them as force. Growth is signed, attack is an event envelope.
    std::array<float, kAmbientBandCount> absoluteEnergy{};
    std::array<float, kAmbientBandCount> relativeFast{1.0f, 1.0f, 1.0f};
    std::array<float, kAmbientBandCount> relativeSlow{1.0f, 1.0f, 1.0f};
    std::array<float, kAmbientBandCount> growth{};
    std::array<float, kAmbientBandCount> attack{};
    std::uint64_t playbackSerial = 0;
    std::uint64_t recordingIdentityHash = 0;
    float gate = 0.0f;
    float broadbandDb = -120.0f;
    std::uint32_t flags = 0;
};

static_assert(sizeof(AmbientFeaturesV1) <= 256);

[[nodiscard]] bool validAmbientFeatures(
    const AmbientFeaturesV1& features) noexcept;

class AmbientFeaturesModel final {
public:
    bool configure(std::uint32_t sampleRate, std::uint16_t channels,
        std::uint64_t streamGeneration,
        std::uint64_t firstFrame = 0,
        std::uint64_t playbackSerial = 0,
        std::uint64_t recordingIdentityHash = 0) noexcept;
    void reset() noexcept;

    // Input is interleaved PCM. A gap or source mismatch is rejected so a
    // caller must explicitly start a new source epoch rather than replaying a
    // stale visual tail into the next recording.
    bool ingest(std::span<const float> pcm, std::uint64_t firstFrame,
        std::uint64_t streamGeneration, std::uint64_t sourceSequence,
        std::uint64_t sourceEndQpc) noexcept;

    [[nodiscard]] const AmbientFeaturesV1& snapshot() const noexcept
    {
        return snapshot_;
    }
    [[nodiscard]] std::uint64_t completedWindows() const noexcept
    {
        return completedWindows_;
    }
    [[nodiscard]] bool configured() const noexcept { return sampleRate_ != 0; }

private:
    struct Biquad {
        double b0 = 0.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
        double z1 = 0.0;
        double z2 = 0.0;
        void configure(double sampleRate, double frequency,
            bool highPass) noexcept;
        [[nodiscard]] double step(double sample) noexcept;
    };
    struct BandFilter {
        Biquad highPass;
        Biquad lowPass;
        [[nodiscard]] double step(double sample) noexcept
        {
            return lowPass.step(highPass.step(sample));
        }
    };

    void finishWindow() noexcept;

    std::uint32_t sampleRate_ = 0;
    std::uint16_t channels_ = 0;
    std::uint32_t framesPerWindow_ = 0;
    std::uint32_t framesInWindow_ = 0;
    std::uint64_t nextFrame_ = 0;
    std::uint64_t windowFirstFrame_ = 0;
    std::uint64_t latestSourceSequence_ = 0;
    std::uint64_t latestSourceEndQpc_ = 0;
    std::uint64_t completedWindows_ = 0;
    std::array<std::array<BandFilter, kAmbientBandCount>,
        kRealtimeMaxChannels> filters_{};
    std::array<double, kAmbientBandCount> accumulatedBandPower_{};
    std::array<double, kAmbientBandCount> slowBandDb_{};
    std::array<double, kAmbientBandCount> fastBandDb_{};
    std::array<double, kAmbientBandCount> previousFastBandDb_{};
    std::array<double, kAmbientBandCount> relativeEnvelope_{1.0, 1.0, 1.0};
    std::array<double, kAmbientBandCount> attackEnvelope_{};
    std::array<double, kAmbientBandCount> envelopes_{};
    double accumulatedBroadbandPower_ = 0.0;
    double gate_ = 0.0;
    double quietSeconds_ = 0.0;
    bool audible_ = false;
    bool baselineReady_ = false;
    AmbientFeaturesV1 snapshot_{};
};

enum class AlbumAmbientMode : std::uint8_t {
    off = 0,
    subtle = 1,
    reactive = 2,
};

struct AlbumAmbientEligibility {
    bool visible = false;
    bool playing = false;
    bool muted = false;
    bool hardware = false;
    bool hasArtwork = false;
    bool albumMode = false;
    bool bookletOpen = false;
    bool scrubbing = false;
    bool dawPreview = false;
    bool outsideSession = false;
    bool reducedMotion = false;
    AlbumAmbientMode mode = AlbumAmbientMode::reactive;

    [[nodiscard]] bool wantsAudio() const noexcept;
};

[[nodiscard]] std::array<float, kAmbientBandCount> ambientTail(
    std::array<float, kAmbientBandCount> start,
    double elapsedSeconds) noexcept;
[[nodiscard]] bool ambientSampleFresh(double ageSeconds) noexcept;
[[nodiscard]] double ambientBackgroundHz(double displayHz) noexcept;

struct AmbientTextureSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

[[nodiscard]] AmbientTextureSize fitAmbientTexture(
    std::uint32_t width, std::uint32_t height,
    bool degraded = false) noexcept;

} // namespace nekorox
