#pragma once

#include "nekorox/contracts.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace nekorox {

inline constexpr std::uint32_t kDynamicsBinMeasured = 1U << 0U;
inline constexpr std::uint32_t kDynamicsBinPartial = 1U << 1U;
inline constexpr std::uint32_t kDynamicsBinUnknown = 1U << 2U;
inline constexpr std::uint32_t kDynamicsBinGapBefore = 1U << 3U;
inline constexpr std::uint32_t kDynamicsBinGapAfter = 1U << 4U;

struct DynamicsBin {
    std::uint64_t firstFrame = 0;
    std::uint64_t endFrame = 0;
    std::uint64_t segmentId = 0;
    std::uint64_t startQpc = 0;
    std::uint64_t endQpc = 0;
    std::uint32_t sampleRate = 0;
    double sumSquares = 0.0;
    std::uint64_t scalarCount = 0;
    double samplePeak = 0.0;
    std::uint32_t validFrames = 0;
    std::uint32_t unknownFrames = 0;
    std::uint32_t nominalFrames = 0;
    std::uint32_t sanitizedSamples = 0;
    std::uint32_t flags = 0;

    [[nodiscard]] double coverage() const noexcept
    {
        return nominalFrames != 0
            ? static_cast<double>(validFrames) /
                static_cast<double>(nominalFrames)
            : 0.0;
    }

    [[nodiscard]] bool measured() const noexcept
    {
        return (flags & kDynamicsBinMeasured) != 0 && scalarCount != 0;
    }
};

struct DynamicsReading {
    double peak = 0.0;
    double power = 0.0;
    double durationSeconds = 0.0;
    bool complete = false;

    [[nodiscard]] std::optional<double> peakDb() const noexcept;
    [[nodiscard]] std::optional<double> rmsDb() const noexcept;
    [[nodiscard]] std::optional<double> crestDb() const noexcept;
};

struct DynamicsSnapshot {
    static constexpr std::size_t capacity = 320;
    std::array<DynamicsBin, capacity> bins{};
    std::size_t count = 0;
    std::uint64_t epoch = 0;
    std::uint64_t revision = 0;
    std::uint64_t sourceSequence = 0;
    std::uint64_t sourceQpc = 0;
    std::uint64_t streamGeneration = 0;
    std::uint64_t playbackSerial = 0;
    std::uint64_t recordingIdentityHash = 0;
    std::uint32_t sampleRate = 0;
    std::uint64_t qpcFrequency = 0;
    bool valid = false;
    bool dataGap = false;
    std::uint64_t sanitizedSamples = 0;
    std::uint64_t unknownFrames = 0;
    std::uint64_t partialBinsCommitted = 0;
    std::uint64_t unknownMicroseconds = 0;
};

[[nodiscard]] DynamicsReading readDynamicsTail(
    const DynamicsSnapshot& snapshot,
    double minimumKnownSeconds = 0.4) noexcept;

class DynamicsHistory final {
public:
    static constexpr std::size_t capacity = DynamicsSnapshot::capacity;
    DynamicsHistory() noexcept;

    bool ingest(const RealtimeFrameV1& frame,
        bool forceDiscontinuity = false) noexcept;
    void invalidate(bool dataGap = true) noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] std::uint64_t totalClosed() const noexcept { return totalClosed_; }
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const DynamicsBin& chronological(
        std::size_t index) const noexcept;
    [[nodiscard]] DynamicsReading reading(
        std::size_t requestedBins) const noexcept;
    [[nodiscard]] DynamicsSnapshot snapshot(
        std::uint64_t sourceSequence,
        std::uint64_t sourceQpc) const noexcept;

private:
    void beginEpoch(std::uint32_t channels, std::uint32_t sampleRate,
        std::uint64_t stream, std::uint64_t playbackSerial,
        std::uint64_t recordingIdentityHash,
        std::uint64_t firstFrame, std::uint64_t firstQpc) noexcept;
    void beginSegment(std::uint32_t channels, std::uint32_t sampleRate,
        std::uint64_t stream, std::uint64_t firstFrame,
        std::uint64_t firstQpc) noexcept;
    bool closePending(bool partial, bool gapAfter) noexcept;
    bool appendUnknown(std::uint64_t firstFrame, std::uint64_t endFrame,
        std::uint64_t startQpc, std::uint64_t endQpc,
        std::uint32_t sampleRate) noexcept;
    void appendClosed(const DynamicsBin& bin) noexcept;
    void trimBefore(std::uint64_t minimumQpc) noexcept;

    std::array<DynamicsBin, capacity> bins_{};
    DynamicsBin pending_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    std::uint32_t channels_ = 0;
    std::uint32_t sampleRate_ = 0;
    std::uint32_t phase_ = 0;
    std::uint64_t streamGeneration_ = 0;
    std::uint64_t playbackSerial_ = 0;
    std::uint64_t recordingIdentityHash_ = 0;
    std::uint64_t segmentId_ = 0;
    std::uint64_t nextFrame_ = 0;
    std::uint64_t qpcFrequency_ = 1;
    std::uint64_t epoch_ = 0;
    std::uint64_t revision_ = 0;
    std::uint64_t totalClosed_ = 0;
    std::uint64_t sanitizedSamples_ = 0;
    std::uint64_t unknownFrames_ = 0;
    std::uint64_t partialBinsCommitted_ = 0;
    std::uint64_t unknownMicroseconds_ = 0;
    bool valid_ = false;
    bool dataGap_ = false;
};

static_assert(sizeof(DynamicsHistory) <= 32U * 1024U);
static_assert(sizeof(DynamicsSnapshot) <= 32U * 1024U);

} // namespace nekorox
