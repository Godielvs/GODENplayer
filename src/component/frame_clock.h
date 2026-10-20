#pragma once

#include "performance_telemetry.h"
#include "frame_notification.h"
#include "render_surface_contracts.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>

namespace nekorox::component {

inline constexpr std::uint32_t kFrameReasonSemantic = 1U << 0U;
inline constexpr std::uint32_t kFrameReasonInput = 1U << 1U;
inline constexpr std::uint32_t kFrameReasonLyrics = 1U << 2U;
inline constexpr std::uint32_t kFrameReasonAmbient = 1U << 3U;
inline constexpr std::uint32_t kFrameReasonAudioPump = 1U << 4U;

class PresentationCoordinatorImpl;

// A one-shot, refresh-aware presentation clock. Only one pulse may be pending
// in the window queue, so a busy UI drops obsolete ticks instead of building
// latency.
class WindowFrameClock final {
public:
    WindowFrameClock();
    ~WindowFrameClock();

    WindowFrameClock(const WindowFrameClock&) = delete;
    WindowFrameClock& operator=(const WindowFrameClock&) = delete;

    void attach(HWND window, UINT message) noexcept;
    // Owner-thread configuration; the provider must outlive attach/detach.
    void setDamageProvider(FrameNotification::DamageProvider provider,
        void* context) noexcept { notification_.setDamageProvider(provider, context); }
    void detach() noexcept;
    bool start() noexcept;
    void stop() noexcept;
    bool requestImmediate() noexcept;
    bool requestAtOrAfter(
        std::uint64_t notBeforeQpc, std::uint32_t reasonMask) noexcept;
    bool consume(WPARAM generation) noexcept;
    // Binds the DXGI frame-latency object for the current swap-chain epoch.
    // The clock duplicates the borrowed handle and owns that duplicate, so a
    // device-loss reset cannot invalidate an in-flight thread-pool wait.
    void setReadinessBinding(SurfaceBindingKey binding) noexcept;
    void setReadinessHandle(HANDLE readiness) noexcept;
    // Called by WM_PAINT. A readiness-backed surface may prepare graphics only
    // after consume() transferred exactly one permit to the UI thread.
    [[nodiscard]] bool beginPresentationOpportunity() noexcept;
    // Reconciles demand after a frame attempt. `needsAnotherFrame` includes a
    // pending semantic revision even when animation has already settled.
    void completePresentationOpportunity(bool needsAnotherFrame) noexcept;
    void completePresentationOpportunity(bool needsAnotherFrame,
        std::uint64_t notBeforeQpc, std::uint32_t reasonMask) noexcept;
    void setLatencyCritical(bool critical) noexcept;
    void setMaximumRefreshHz(double refreshHz) noexcept;
    void setTelemetrySurface(TelemetrySurface surface) noexcept;

    [[nodiscard]] std::uint64_t acquiredCredits() const noexcept {return readinessAcquired_.load(std::memory_order_acquire);}
    [[nodiscard]] std::uint64_t begunCredits() const noexcept {return readinessBegun_.load(std::memory_order_acquire);}
    [[nodiscard]] std::uint64_t retiredHeldCredits() const noexcept {return readinessRetiredHeld_.load(std::memory_order_acquire);}
    [[nodiscard]] std::uint64_t diagnosticDeliveryState() const noexcept
    {
        return std::uint64_t(running_.load()) | (std::uint64_t(messagePending_.load()) << 1) |
            (std::uint64_t(readinessPermit_.load()) << 2) | (std::uint64_t(uiOpportunity_.load()) << 3) |
            (std::uint64_t(notification_.pending()) << 4) | (std::uint64_t(readinessWaitArmed_.load()) << 5) |
            (std::uint64_t(generation_.load()) << 32);
    }
    [[nodiscard]] std::uint64_t diagnosticNotificationGeneration() const noexcept { return notification_.generation(); }
    [[nodiscard]] double intervalSeconds() const noexcept;
    [[nodiscard]] std::uint32_t targetRefreshMilliHz() const noexcept;
    [[nodiscard]] std::uint64_t deliveredPulses() const noexcept;
    [[nodiscard]] std::uint64_t skippedDeadlines() const noexcept;
    [[nodiscard]] std::uint64_t logicalNotBeforeQpc() const noexcept
    {
        return logicalNotBeforeQpc_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint32_t pendingReasonMask() const noexcept
    {
        return pendingReasonMask_.load(std::memory_order_acquire);
    }
    [[nodiscard]] static std::uint32_t diagnosticActiveCount() noexcept;
    [[nodiscard]] static bool pressureMode() noexcept;
    static void reportPresentOutcome(bool wasStillDrawing) noexcept;
    static void environmentChanged() noexcept;

private:
    friend class PresentationCoordinatorImpl;

    void updatePeriod(bool force) noexcept;
    [[nodiscard]] std::uint64_t qpcNow() const noexcept;
    void retireNotifications() noexcept;
    bool postReadyLocked() noexcept;
    std::atomic<std::uint32_t> readinessBindingGeneration_{1};
    std::atomic<std::uint64_t> readinessAcquired_{0},readinessBegun_{0},readinessRetiredHeld_{0};
    void disarmReadinessWait() noexcept;
    bool armReadinessWait() noexcept;
    static VOID CALLBACK readinessCallback(
        PTP_CALLBACK_INSTANCE instance,
        void* context,
        PTP_WAIT wait,
        TP_WAIT_RESULT result);

    std::atomic<HWND> window_{nullptr};
    FrameNotification notification_;
    std::atomic<UINT> message_{0};
    std::atomic<std::uint32_t> generation_{1};
    std::atomic<bool> running_{false};
    std::atomic<bool> messagePending_{false};
    std::atomic<bool> readinessPermit_{false};
    std::atomic<bool> uiOpportunity_{false};
    std::atomic<bool> readinessWaitArmed_{false};
    std::atomic<bool> readinessEpochChanging_{false};
    std::atomic<bool> acceptingReadinessRearm_{true};
    std::atomic<std::uint32_t> readinessArmedGeneration_{0};
    std::uint32_t activeOpportunityGeneration_ = 0;
    std::atomic<HANDLE> readinessHandle_{nullptr};
    HANDLE readinessSourceHandle_ = nullptr;
    SurfaceBindingKey readinessBinding_{};
    std::atomic<bool> environmentSuppressed_{false};
    std::atomic<std::uint64_t> deliveredPulses_{0};
    std::atomic<std::uint64_t> skippedDeadlines_{0};
    std::atomic<std::uint64_t> lastConsumedQpc_{0};
    std::atomic<std::uint64_t> logicalNotBeforeQpc_{0};
    std::atomic<std::uint32_t> pendingReasonMask_{0};
    std::atomic<bool> latencyCritical_{false};
    std::atomic<std::uint32_t> maximumRefreshMilliHz_{0};
    std::atomic<TelemetrySurface> telemetrySurface_{
        TelemetrySurface::artwork};
    std::uint64_t qpcFrequency_ = 1;
    std::atomic<std::uint64_t> periodTicks_{1};
    std::uint64_t nextDeadlineQpc_ = 0;
    std::uint64_t lastRefreshQueryQpc_ = 0;
    std::uint64_t lastEnvironmentGeneration_ = 0;
    std::uint64_t lastWindowPresentationRevision_ = 0;
    std::uint64_t lastRootLifetimeToken_ = 0;
    std::atomic<std::uint32_t> targetRefreshMilliHz_{120000};
    std::mutex readinessMutex_;
    PTP_WAIT readinessWait_ = nullptr;
};

} // namespace nekorox::component
