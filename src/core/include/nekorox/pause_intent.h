#pragma once

#include <cstdint>
#include <optional>

namespace nekorox {

// Main-thread-owned command state. Backend callbacks can lag rapid toggles;
// reconciliation is a queued action, never another call inside the callback.
class PauseIntent final {
public:
    enum class CallbackAction { commit, reconcile, ignore };

    bool request(bool backendPaused, std::uint64_t now,
        std::uint64_t freshnessTicks) noexcept
    {
        const bool base = fresh(now, freshnessTicks) ? *desired_ : backendPaused;
        desired_ = !base;
        issued_ = now;
        reconcileRequested_ = false;
        reconcileUsed_ = false;
        return *desired_;
    }

    CallbackAction observe(bool paused, std::uint64_t now,
        std::uint64_t freshnessTicks) noexcept
    {
        if (!desired_ || paused == *desired_ || !fresh(now, freshnessTicks)) {
            cancel();
            return CallbackAction::commit;
        }
        if (!reconcileUsed_) {
            reconcileUsed_ = true;
            reconcileRequested_ = true;
            return CallbackAction::reconcile;
        }
        return CallbackAction::ignore;
    }

    std::optional<bool> takeReconciliation(std::uint64_t now,
        std::uint64_t freshnessTicks) noexcept
    {
        if (!reconcileRequested_ || !fresh(now, freshnessTicks)) return {};
        reconcileRequested_ = false;
        return desired_;
    }

    void cancel() noexcept
    {
        desired_.reset();
        reconcileRequested_ = false;
        reconcileUsed_ = false;
    }

private:
    bool fresh(std::uint64_t now, std::uint64_t freshnessTicks) const noexcept
    {
        return desired_.has_value() && now >= issued_ &&
            now - issued_ <= freshnessTicks;
    }
    std::optional<bool> desired_;
    std::uint64_t issued_ = 0;
    bool reconcileRequested_ = false;
    bool reconcileUsed_ = false;
};

} // namespace nekorox
