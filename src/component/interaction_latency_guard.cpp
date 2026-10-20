#include "interaction_latency_guard.h"

#include <Windows.h>

namespace nekorox::component {
namespace {

constexpr std::uint64_t kScrollLatencyWindowMilliseconds = 120;

} // namespace

InteractionLatencyGuard& InteractionLatencyGuard::instance() noexcept
{
    static InteractionLatencyGuard guard;
    return guard;
}

void InteractionLatencyGuard::noteScroll() noexcept
{
    LARGE_INTEGER qpc{};
    if (QueryPerformanceCounter(&qpc) && qpc.QuadPart > 0) {
        lastRealInputQpc_.store(
            static_cast<std::uint64_t>(qpc.QuadPart),
            std::memory_order_release);
    }
    const auto deadline = nowMilliseconds() +
        kScrollLatencyWindowMilliseconds;
    auto observed = deadlineMilliseconds_.load(std::memory_order_relaxed);
    while (observed < deadline &&
        !deadlineMilliseconds_.compare_exchange_weak(
            observed,
            deadline,
            std::memory_order_release,
            std::memory_order_relaxed)) {
    }
}

bool InteractionLatencyGuard::preferLatency() const noexcept
{
    return nowMilliseconds() <
        deadlineMilliseconds_.load(std::memory_order_acquire);
}

bool InteractionLatencyGuard::shouldYieldRender() const noexcept
{
    // Queue observations can mark unread input as old for the host's waits.
    // Use the hint recorded when the host actually delivers human input.
    // Renderers never inspect or change message-queue state.
    return preferLatency();
}

InteractionLatencySnapshot InteractionLatencyGuard::snapshot() const noexcept
{
    InteractionLatencySnapshot result;
    result.explicitWindow = preferLatency();
    result.lastRealInputQpc = lastRealInputQpc();
    return result;
}

std::uint64_t InteractionLatencyGuard::nowMilliseconds() noexcept
{
    return static_cast<std::uint64_t>(GetTickCount64());
}

} // namespace nekorox::component
