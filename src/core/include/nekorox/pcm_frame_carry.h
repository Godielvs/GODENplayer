#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nekorox {

// Lossless decoded-frame carry shared by the scrub worker and its tests.
// A decoder chunk is converted exactly once; consumers drain whole frames and
// no subsequent decoder run is needed until this object is empty.
class PcmFrameCarry final {
public:
    void clear() noexcept
    {
        samples_.clear();
        frameOffset_ = 0;
        rate_ = channels_ = mask_ = 0;
    }

    template <typename Sample>
    bool replace(const Sample* source, std::size_t frames,
        std::uint32_t rate, std::uint32_t channels, std::uint32_t mask,
        std::size_t maximumBytes)
    {
        clear();
        if (!source || frames == 0 || rate == 0 || channels == 0 ||
            frames > maximumBytes / sizeof(float) / channels) return false;
        rate_ = rate;
        channels_ = channels;
        mask_ = mask;
        samples_.resize(frames * channels);
        std::transform(source, source + samples_.size(), samples_.begin(),
            [](Sample sample) { return static_cast<float>(sample); });
        return true;
    }

    std::size_t appendTo(std::vector<float>& destination,
        std::size_t maximumFrames)
    {
        if (empty() || maximumFrames == 0) return 0;
        const auto amount = std::min(maximumFrames, frames() - frameOffset_);
        const auto first = samples_.begin() + frameOffset_ * channels_;
        destination.insert(destination.end(), first, first + amount * channels_);
        frameOffset_ += amount;
        return amount;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return channels_ == 0 || frameOffset_ >= frames();
    }
    [[nodiscard]] std::size_t frames() const noexcept
    {
        return channels_ == 0 ? 0 : samples_.size() / channels_;
    }
    [[nodiscard]] std::uint32_t rate() const noexcept { return rate_; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }
    [[nodiscard]] std::uint32_t mask() const noexcept { return mask_; }

private:
    std::vector<float> samples_;
    std::size_t frameOffset_ = 0;
    std::uint32_t rate_ = 0;
    std::uint32_t channels_ = 0;
    std::uint32_t mask_ = 0;
};

} // namespace nekorox
