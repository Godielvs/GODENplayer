#pragma once

#include "nekorox/c11/c11_reason_tickets.h"

#include <atomic>
#include <cstdint>

namespace nekorox {

enum class DiscontinuityReason : std::uint32_t {
    none = 0,
    sourceStart = 1U << 0U,
    trackChange = 1U << 1U,
    seek = 1U << 2U,
    pauseResume = 1U << 3U,
    streamChange = 1U << 4U,
    formatChange = 1U << 5U,
    clockBackward = 1U << 6U,
    backlogLoss = 1U << 7U,
    publicationGap = 1U << 8U,
    explicitReset = 1U << 9U,
};

[[nodiscard]] constexpr std::uint32_t reasonMask(
    DiscontinuityReason reason) noexcept
{
    return static_cast<std::uint32_t>(reason);
}

struct PendingDiscontinuity {
    std::uint32_t reasonMask = 0;
    goden::c11::ReasonTickets::Snapshot ticketSnapshot{};
    std::uint64_t firstSequence = 0;
    std::uint64_t firstQpc = 0;
    std::uint64_t rejectedAttempts = 0;
    bool metadataCoherent = false;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return reasonMask != 0;
    }
};

// A lock-free semantic latch shared by realtime publishers. Per-reason tickets
// ensure that committing an older publication cannot consume a newer mark of
// the same reason. The frame's source interval is the temporal authority;
// optional first-event metadata is not claimed coherent under multi-producer
// use.
class DiscontinuityLatch final {
public:
    void mark(DiscontinuityReason reason,
        std::uint64_t firstSequence = 0,
        std::uint64_t firstQpc = 0) noexcept
    {
        mark(reasonMask(reason), firstSequence, firstQpc);
    }

    void mark(std::uint32_t reasons,
        std::uint64_t firstSequence = 0,
        std::uint64_t firstQpc = 0) noexcept
    {
        if (reasons == 0) return;
        (void)firstSequence;
        (void)firstQpc;
        tickets_.mark(reasons);
    }

    [[nodiscard]] PendingDiscontinuity pending() const noexcept
    {
        PendingDiscontinuity result;
        result.ticketSnapshot = tickets_.pending();
        result.reasonMask = result.ticketSnapshot.mask;
        result.rejectedAttempts =
            rejectedAttempts_.load(std::memory_order_acquire);
        return result;
    }

    void rejected() noexcept
    {
        if (tickets_.pending().mask != 0)
            rejectedAttempts_.fetch_add(1, std::memory_order_relaxed);
    }

    void commit(const PendingDiscontinuity& published) noexcept
    {
        if (!published) return;
        tickets_.commit(published.ticketSnapshot);
    }

    void resetQuiescent() noexcept
    {
        tickets_.resetQuiescent();
        rejectedAttempts_.store(0, std::memory_order_release);
    }

private:
    goden::c11::ReasonTickets tickets_{};
    std::atomic<std::uint64_t> rejectedAttempts_{0};
};

} // namespace nekorox
