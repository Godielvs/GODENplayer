#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace nekorox::component {

enum class ScrubPreviewState : std::uint8_t {
    idle,
    awaiting,
    ready,
    silentSelection,
    failed,
};

struct ScrubPreviewSnapshot {
    ScrubPreviewState state = ScrubPreviewState::idle;
    std::uint64_t generation = 0;
    std::string reason;
    std::uint64_t sessionGeneration = 0;
    std::uint64_t targetRevision = 0;
    std::uint64_t outputOpenCount = 0;
    std::uint64_t decoderOpenCount = 0;
    std::uint64_t flushCount = 0;
    std::uint64_t queuedFrames = 0;
    std::uint64_t submittedCursor = 0;
    std::uint64_t staleDrops = 0;
    std::uint64_t acceptedBlocks = 0;
};

struct ScrubPreviewCrossfadeGains {
    float outgoing = 1.0f;
    float incoming = 0.0f;
};

[[nodiscard]] inline ScrubPreviewCrossfadeGains scrubPreviewCrossfadeGains(
    std::size_t frame, std::size_t frameCount) noexcept
{
    if (frameCount == 0) return {};
    const auto amount = static_cast<float>((std::min)(frame + 1U, frameCount)) /
        static_cast<float>(frameCount + 1U);
    return {1.0f - amount, amount};
}

struct ScrubDeliveryLimits {
    std::size_t blockFrames = 0;
    std::size_t maximumQueuedFrames = 0;
    std::size_t maximumPcmSamples = 0;
};

[[nodiscard]] inline ScrubDeliveryLimits scrubDeliveryLimits(
    std::uint32_t sampleRate, std::uint32_t channels) noexcept
{
    if (sampleRate == 0 || channels == 0 || channels > 32) return {};
    const auto block = std::max<std::size_t>(1,
        static_cast<std::size_t>(std::llround(sampleRate * 0.010)));
    const auto queued = std::max<std::size_t>(block,
        static_cast<std::size_t>(std::llround(sampleRate * 0.060)));
    constexpr auto maximumBytes = std::size_t{8} << 20U;
    return {block, queued, maximumBytes / sizeof(float)};
}

[[nodiscard]] inline bool scrubPreviewNeedsSessionOpen(
    std::uint64_t openSession, std::uint64_t requestedSession,
    bool sameTrack, bool outputOpen, bool decoderOpen) noexcept
{
    return openSession != requestedSession || !sameTrack ||
        !outputOpen || !decoderOpen;
}

inline float scrubPreviewEnvelope(std::size_t frame,
    std::size_t totalFrames, std::uint32_t sampleRate) noexcept
{
    if (totalFrames == 0 || sampleRate == 0 || frame >= totalFrames) return 0.0f;
    const auto edge = std::max<std::size_t>(1,
        static_cast<std::size_t>(std::llround(sampleRate * 0.008)));
    const auto smooth = [](float value) {
        value = std::clamp(value, 0.0f, 1.0f);
        return value * value * (3.0f - 2.0f * value);
    };
    const auto in = smooth(static_cast<float>(frame) /
        static_cast<float>(edge));
    const auto remaining = totalFrames - frame - 1;
    const auto out = smooth(static_cast<float>(remaining) /
        static_cast<float>(edge));
    return std::min(in, out) * 0.50118723f; // declared dry preview at -6 dB
}

class ScrubPreviewService final {
public:
    static ScrubPreviewService& instance();
    bool start(double targetSeconds) noexcept;
    void update(double targetSeconds) noexcept;
    void cancel() noexcept;
    void shutdown() noexcept;
    [[nodiscard]] ScrubPreviewSnapshot snapshot() const;

private:
    ScrubPreviewService();
    ~ScrubPreviewService();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nekorox::component
