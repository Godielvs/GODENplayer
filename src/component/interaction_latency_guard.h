#pragma once

#include <atomic>
#include <cstdint>

namespace nekorox::component {

struct InteractionLatencySnapshot {
    bool explicitWindow = false;
    bool pendingRealInput = false;
    bool sentMessageAdvisory = false;
    std::uint64_t lastRealInputQpc = 0;

    [[nodiscard]] bool shouldYieldAmbient() const noexcept
    {
        return explicitWindow || pendingRealInput;
    }
};

// A timer-free, process-wide hint used by decorative renderers to yield GPU
// and UI-thread budget during short input bursts. It never changes animation
// timing or model state; renderers simply choose a cheaper presentation path.
class InteractionLatencyGuard final {
public:
    static InteractionLatencyGuard& instance() noexcept;

    void noteScroll() noexcept;
    [[nodiscard]] bool preferLatency() const noexcept;
    [[nodiscard]] bool shouldYieldRender() const noexcept;
    [[nodiscard]] InteractionLatencySnapshot snapshot() const noexcept;
    [[nodiscard]] std::uint64_t lastRealInputQpc() const noexcept
    {
        return lastRealInputQpc_.load(std::memory_order_acquire);
    }

private:
    static std::uint64_t nowMilliseconds() noexcept;

    std::atomic<std::uint64_t> deadlineMilliseconds_{0};
    mutable std::atomic<std::uint64_t> lastRealInputQpc_{0};
};

} // namespace nekorox::component
