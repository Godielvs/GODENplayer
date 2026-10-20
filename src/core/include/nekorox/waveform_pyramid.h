#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace nekorox {

inline constexpr std::uint32_t kWaveformPyramidLegacyVersion = 1;
inline constexpr std::uint32_t kWaveformPyramidVersion = 2;
inline constexpr std::uint32_t kWaveformPyramidLegacyWireVersion = 1;
inline constexpr std::uint32_t kWaveformPyramidWireVersion = 2;
inline constexpr std::uint32_t kWaveformPyramidMagic = 0x3150574eU; // NWP1
inline constexpr std::size_t kWaveformPyramidLevelCount = 4;
inline constexpr std::array<std::uint32_t, kWaveformPyramidLevelCount>
    kWaveformPyramidResolutionsMs{10, 40, 160, 640};
inline constexpr std::size_t kWaveformPyramidMaximumBaseBins =
    24U * 60U * 60U * 100U; // 24 hours at 10 ms.

inline constexpr std::uint32_t kWaveformPyramidFlagComplete = 1U << 0U;

// Compact perceptual payload added in V2. The original peak/RMS pair remains
// at the front of every bin so V1 renderers and the V1 wire reader have an
// unambiguous fallback. Values are normalized unsigned fixed point.
struct WaveformDetailV2 {
    std::uint16_t peakLeft = 0;
    std::uint16_t peakRight = 0;
    std::uint16_t rmsLeft = 0;
    std::uint16_t rmsRight = 0;
    std::uint8_t medianAmplitude = 0;
    std::uint8_t denseAmplitude = 0;
    std::uint8_t lowEnergy = 0;
    std::uint8_t midEnergy = 0;
    std::uint8_t highEnergy = 0;
    std::uint8_t transient = 0;
    std::uint8_t reserved0 = 0;
    std::uint8_t reserved1 = 0;
};

static_assert(sizeof(WaveformDetailV2) == 16);

struct WaveformBinV1 {
    float peak = 0.0f;
    float rms = 0.0f;
    WaveformDetailV2 detail{};
};

struct WaveformLevelV1 {
    std::uint32_t resolutionMs = 0;
    std::vector<WaveformBinV1> bins;
};

struct WaveformPyramidV1 {
    std::uint32_t version = kWaveformPyramidVersion;
    std::uint32_t flags = 0;
    std::uint64_t generation = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint64_t totalFrames = 0;
    std::uint64_t scannedFrames = 0;
    double durationSeconds = 0.0;
    std::array<WaveformLevelV1, kWaveformPyramidLevelCount> levels{};

    [[nodiscard]] bool complete() const noexcept
    {
        return (flags & kWaveformPyramidFlagComplete) != 0;
    }
};

[[nodiscard]] bool validateWaveformPyramid(
    const WaveformPyramidV1& pyramid,
    std::string* error = nullptr) noexcept;

[[nodiscard]] bool encodeWaveformPyramid(
    const WaveformPyramidV1& pyramid,
    std::vector<std::byte>& output,
    std::string& error);

[[nodiscard]] bool decodeWaveformPyramid(
    std::span<const std::byte> encoded,
    WaveformPyramidV1& output,
    std::string& error);

// Streaming accumulator for the 10 ms base level. It retains waveform bins,
// never source PCM. Higher levels are derived when a snapshot is requested.
class WaveformPyramidBuilder final {
public:
    explicit WaveformPyramidBuilder(
        std::uint64_t generation = 0,
        std::uint64_t totalFrames = 0) noexcept;

    WaveformPyramidBuilder(const WaveformPyramidBuilder&) = delete;
    WaveformPyramidBuilder& operator=(const WaveformPyramidBuilder&) = delete;

    [[nodiscard]] bool appendInterleaved(
        std::span<const float> samples,
        std::size_t frameCount,
        std::uint32_t channels,
        std::uint32_t sampleRate,
        std::string& error);

    [[nodiscard]] WaveformPyramidV1 snapshot(bool complete = false) const;
    [[nodiscard]] WaveformPyramidV1 finalize();

    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] std::uint64_t scannedFrames() const noexcept
    {
        return scannedFrames_;
    }
    [[nodiscard]] std::uint64_t totalFrames() const noexcept
    {
        return totalFrames_;
    }
    [[nodiscard]] std::size_t completedBaseBins() const noexcept
    {
        return baseBins_.size();
    }

private:
    [[nodiscard]] WaveformBinV1 partialBin() const noexcept;
    void finishPartialBin();

    std::uint64_t generation_ = 0;
    std::uint64_t totalFrames_ = 0;
    std::uint64_t scannedFrames_ = 0;
    std::uint32_t sampleRate_ = 0;
    std::uint32_t channels_ = 0;
    std::vector<WaveformBinV1> baseBins_;
    double partialSquareSum_ = 0.0;
    double partialLeftSquareSum_ = 0.0;
    double partialRightSquareSum_ = 0.0;
    double partialLowSquareSum_ = 0.0;
    double partialMidSquareSum_ = 0.0;
    double partialHighSquareSum_ = 0.0;
    float partialPeak_ = 0.0f;
    float partialLeftPeak_ = 0.0f;
    float partialRightPeak_ = 0.0f;
    float partialTransient_ = 0.0f;
    std::array<std::uint32_t, 32> partialAmplitudeHistogram_{};
    std::uint64_t partialFrameCount_ = 0;
    float lowPass250_ = 0.0f;
    float lowPass4000_ = 0.0f;
    float fastEnvelope_ = 0.0f;
    float slowEnvelope_ = 0.0f;
    float lowCoefficient_ = 0.0f;
    float highCoefficient_ = 0.0f;
    float fastEnvelopeCoefficient_ = 0.0f;
    float slowEnvelopeCoefficient_ = 0.0f;
    bool finalized_ = false;
};

} // namespace nekorox
