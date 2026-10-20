#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nekorox::goden_audio {

// A callback pushes its entire block before any of its latency-aligned reads.
// Keep that block in addition to the maximum delay, or its first writes would
// overwrite samples still needed at the beginning of the delayed output.
template<std::size_t MaximumDelay, std::size_t MaximumBlock,
    std::size_t MaximumChannels>
class FixedDspHistory final {
public:
    static constexpr std::size_t capacity = MaximumDelay + MaximumBlock + 1U;

    FixedDspHistory()
        : samples_(capacity * MaximumChannels), tags_(capacity) {}

    void reset() noexcept
    {
        cursor_ = 0;
        if (++generation_ == 0) {
            std::fill(tags_.begin(), tags_.end(), 0U);
            generation_ = 1;
        }
    }

    void push(const float* input, std::size_t frames,
        std::uint32_t channels) noexcept
    {
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto base = cursor_ * MaximumChannels;
            for (std::uint32_t channel = 0; channel < channels; ++channel)
                samples_[base + channel] = input[frame * channels + channel];
            tags_[cursor_] = generation_;
            cursor_ = (cursor_ + 1U) % capacity;
        }
    }

    void read(std::uint32_t delayFrames, float* output,
        std::size_t frames, std::uint32_t channels) const noexcept
    {
        const auto bounded = std::min<std::size_t>(delayFrames, MaximumDelay);
        auto cursor = (cursor_ + capacity - (frames % capacity) +
            capacity - bounded) % capacity;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto valid = tags_[cursor] == generation_;
            const auto base = cursor * MaximumChannels;
            for (std::uint32_t channel = 0; channel < channels; ++channel)
                output[frame * channels + channel] =
                    valid ? samples_[base + channel] : 0.0f;
            cursor = (cursor + 1U) % capacity;
        }
    }

private:
    std::vector<float> samples_;
    std::vector<std::uint32_t> tags_;
    std::size_t cursor_ = 0;
    std::uint32_t generation_ = 1;
};

} // namespace nekorox::goden_audio
