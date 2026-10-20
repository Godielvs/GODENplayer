#pragma once

#include <cstdint>

namespace nekorox::component {

struct PresentationFrameToken {
    std::uint64_t revision = 0;
    std::uint64_t epoch = 0;
    std::uint64_t frameId = 0;
};

class PresentationRevision final {
public:
    std::uint64_t invalidate() noexcept { return ++desired_; }
    std::uint64_t drawnRevision() const noexcept { return desired_; }
    [[nodiscard]] PresentationFrameToken capture() noexcept
    {
        return {desired_, epoch_, ++nextFrameId_};
    }
    // A successful Present accepts the captured revision independently from
    // whether compositor observation was possible. `displayObserved` advances
    // the separate displayed watermark only for that same accepted token.
    void presented(
        std::uint64_t drawn, long result, bool displayObserved) noexcept;
    void presented(
        PresentationFrameToken frame, long result, bool displayObserved) noexcept;
    void observedDisplayed(PresentationFrameToken frame) noexcept;
    void deviceEpochChanged() noexcept;

    [[nodiscard]] bool pending() const noexcept { return accepted_ < desired_; }
    [[nodiscard]] std::uint64_t desired() const noexcept { return desired_; }
    [[nodiscard]] std::uint64_t accepted() const noexcept { return accepted_; }
    [[nodiscard]] std::uint64_t displayed() const noexcept { return displayed_; }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }

private:
    std::uint64_t desired_ = 1;
    std::uint64_t accepted_ = 0;
    std::uint64_t displayed_ = 0;
    std::uint64_t epoch_ = 1;
    std::uint64_t nextFrameId_ = 0;
};

} // namespace nekorox::component
