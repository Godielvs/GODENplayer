#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace nekorox::component {

[[nodiscard]] inline std::atomic<std::uint64_t>& operationSequenceState() noexcept
{
    static std::atomic<std::uint64_t> value{0};
    return value;
}

// Process-wide, wall-clock-backed ordering for user-visible reversible
// operations.  The timestamp component makes a persisted journal comparable
// after restart while the CAS guarantees strict ordering inside one process.
[[nodiscard]] inline std::uint64_t nextOperationSequence() noexcept
{
    auto& last = operationSequenceState();
    const auto wall = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    auto observed = last.load(std::memory_order_relaxed);
    for (;;) {
        const auto wanted = (std::max)(wall, observed + 1U);
        if (last.compare_exchange_weak(observed, wanted,
                std::memory_order_relaxed, std::memory_order_relaxed)) {
            return wanted;
        }
    }
}

inline void observeOperationSequence(std::uint64_t value) noexcept
{
    auto& last = operationSequenceState();
    auto observed = last.load(std::memory_order_relaxed);
    while (observed < value && !last.compare_exchange_weak(observed, value,
        std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

} // namespace nekorox::component
