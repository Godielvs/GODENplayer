#pragma once

#include <windows.h>
#include <commctrl.h>

#include <atomic>

namespace nekorox::component {

// Deliver frame notices at WM_PAINT priority, after hardware input, instead of
// doing graphics work from a continuous stream of posted messages. The target
// keeps its normal paint handler and the clock keeps its readiness/epoch gate.
// attach/reset are UI-thread operations; stop producers before reset.
class FrameNotification final {
public:
    // Evaluated on the owner thread. Return false for complete owner damage;
    // true supplies a visible region for a retained partial repaint.
    using DamageProvider = bool (*)(HWND, void*, RECT&) noexcept;
    FrameNotification() = default;
    ~FrameNotification() { reset(); }
    FrameNotification(const FrameNotification&) = delete;
    FrameNotification& operator=(const FrameNotification&) = delete;

    bool attach(HWND target, UINT message) noexcept
    {
        reset();
        if (!target || !message || !wakeMessage() || GetWindowThreadProcessId(target, nullptr) != GetCurrentThreadId()) return false;
        message_ = message;
        if (!SetWindowSubclass(target, &subclassProc, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this))) return false;
        window_.store(target, std::memory_order_release);
        return true;
    }

    bool request(WPARAM generation) noexcept
    {
        const auto window = window_.load(std::memory_order_acquire);
        if (!window) return false;
        generation_.store(generation, std::memory_order_relaxed);
        if (pending_.exchange(true, std::memory_order_acq_rel)) return true;
        // Mark internal paint on the UI thread *after* its current BeginPaint:
        // invalidating from the producer during a paint can lose that hint.
        // This coalesced doorbell never consumes/rearms the clock or draws.
        if (PostMessageW(window, wakeMessage(), 0, 0)) return true;
        pending_.store(false, std::memory_order_release);
        return false;
    }

    void cancel() noexcept { pending_.store(false, std::memory_order_release); }
    void setDamageProvider(DamageProvider provider, void* context) noexcept
    {
        damageProvider_ = provider;
        damageContext_ = context;
    }
    [[nodiscard]] bool pending() const noexcept { return pending_.load(std::memory_order_acquire); }
    [[nodiscard]] WPARAM generation() const noexcept { return generation_.load(std::memory_order_acquire); }

    void reset() noexcept
    {
        cancel();
        if (const auto window = window_.exchange(nullptr, std::memory_order_acq_rel))
            RemoveWindowSubclass(window, &subclassProc, reinterpret_cast<UINT_PTR>(this));
        message_ = 0;
        damageProvider_ = nullptr;
        damageContext_ = nullptr;
    }

private:
    static UINT wakeMessage() noexcept
    {
        static const auto message = RegisterWindowMessageW(L"GodenFrameNotificationWakeV1");
        return message;
    }

    static LRESULT CALLBACK subclassProc(HWND window, UINT message, WPARAM w, LPARAM l,
        UINT_PTR id, DWORD_PTR data) noexcept
    {
        const auto owner = reinterpret_cast<FrameNotification*>(data);
        if (message == wakeMessage()) {
            if (owner->pending_.load(std::memory_order_acquire)) {
                // Use real, visible owner damage. Internal paint or a fixed
                // token can be discarded/clipped by the host. A retained owner
                // may choose its live region instead of repainting all text
                // and controls for every scope tick.
                RECT region{};
                const auto partial = owner->damageProvider_ &&
                    owner->damageProvider_(window, owner->damageContext_, region) &&
                    region.right > region.left && region.bottom > region.top;
                InvalidateRect(window, partial ? &region : nullptr, FALSE);
            }
        } else if (message == WM_NCDESTROY) {
            owner->cancel();
            owner->window_.store(nullptr, std::memory_order_release);
            RemoveWindowSubclass(window, &subclassProc, id);
        } else if (message == WM_PAINT && owner->pending_.exchange(false, std::memory_order_acq_rel)) {
            const auto notice = owner->message_;
            const auto generation = owner->generation_.load(std::memory_order_acquire);
            // Snapshot before delivery: the handler may detach/destroy us.
            SendMessageW(window, notice, generation, 0);
            if (!IsWindow(window)) return 0;
        }
        return DefSubclassProc(window, message, w, l);
    }

    std::atomic<HWND> window_{nullptr};
    std::atomic<bool> pending_{false};
    std::atomic<WPARAM> generation_{0};
    UINT message_ = 0;
    DamageProvider damageProvider_ = nullptr;
    void* damageContext_ = nullptr;
};

} // namespace nekorox::component
