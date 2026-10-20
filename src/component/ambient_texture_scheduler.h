#pragma once

#include <algorithm>
#include <cstdint>

namespace nekorox::component {

// Keeps semantic dirtiness separate from when expensive decorative work is
// admissible. A deferred revision remains pending, but cannot turn an expired
// texture deadline into a refresh-rate presentation loop.
class AmbientTextureScheduler final {
public:
    void setPeriod(std::uint64_t periodQpc) noexcept
    {
        periodQpc_ = std::max<std::uint64_t>(1, periodQpc);
    }

    void reset(std::uint64_t nowQpc) noexcept
    {
        deadlineQpc_ = nowQpc;
        deferred_ = false;
    }

    [[nodiscard]] bool due(bool pending, std::uint64_t nowQpc) const noexcept
    {
        return pending && (!deadlineQpc_ || nowQpc >= deadlineQpc_);
    }

    // Returns true only when defer state changes. Repeated observations before
    // the owned opportunity never slide the deadline forward.
    [[nodiscard]] bool defer(bool pending, std::uint64_t nowQpc) noexcept
    {
        if (!pending) {
            const auto changed = deferred_;
            deferred_ = false;
            return changed;
        }
        const auto entered = !deferred_;
        deferred_ = true;
        if (!deadlineQpc_ || nowQpc >= deadlineQpc_) {
            deadlineQpc_ = nowQpc + periodQpc_;
        }
        return entered;
    }

    // Resume the latest revision immediately. There is no queue of missed
    // revisions: the controller still owns only its newest visual revision.
    [[nodiscard]] bool resume(bool pending, std::uint64_t nowQpc) noexcept
    {
        if (!deferred_) return false;
        deferred_ = false;
        if (pending && (!deadlineQpc_ || deadlineQpc_ > nowQpc)) {
            deadlineQpc_ = nowQpc;
        }
        return true;
    }

    void accepted(std::uint64_t nowQpc) noexcept
    {
        deferred_ = false;
        if (!deadlineQpc_) {
            deadlineQpc_ = nowQpc + periodQpc_;
            return;
        }
        if (nowQpc >= deadlineQpc_) {
            const auto elapsedPeriods =
                (nowQpc - deadlineQpc_) / periodQpc_ + 1;
            deadlineQpc_ += elapsedPeriods * periodQpc_;
        }
    }

    [[nodiscard]] std::uint64_t deadlineQpc() const noexcept
    {
        return deadlineQpc_;
    }
    [[nodiscard]] std::uint64_t periodQpc() const noexcept
    {
        return periodQpc_;
    }
    [[nodiscard]] bool deferred() const noexcept { return deferred_; }

private:
    std::uint64_t deadlineQpc_ = 0;
    std::uint64_t periodQpc_ = 1;
    bool deferred_ = false;
};

} // namespace nekorox::component
