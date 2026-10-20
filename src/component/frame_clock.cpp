#include "frame_clock.h"
#include "ui_animation_policy.h"

#include "display_environment.h"
#include "performance_telemetry.h"
#include "rollout_flags.h"

#include "nekorox/presentation_timing.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <memory>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace nekorox::component {
namespace {

constexpr std::uint64_t kRefreshQuerySeconds = 2;
constexpr double kPressureWindowSeconds = 0.5;
constexpr double kPressureRecoverySeconds = 2.0;
std::atomic<std::uint32_t> g_activeFrameClocks{0};

} // namespace

class PresentationCoordinatorImpl final {
public:
    PresentationCoordinatorImpl()
    {
        LARGE_INTEGER frequency{};
        if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
            qpcFrequency_ = static_cast<std::uint64_t>(frequency.QuadPart);
        }
        timer_ = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (timer_ == nullptr) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        if (timer_ != nullptr) {
            RegisterWaitForSingleObject(
                &wait_, timer_, &PresentationCoordinatorImpl::waitCallback,
                this, INFINITE, WT_EXECUTEDEFAULT);
        }
    }

    ~PresentationCoordinatorImpl()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            clocks_.clear();
            if (timer_ != nullptr) CancelWaitableTimer(timer_);
        }
        if (wait_ != nullptr) {
            UnregisterWaitEx(wait_, INVALID_HANDLE_VALUE);
            wait_ = nullptr;
        }
        if (timer_ != nullptr) {
            CloseHandle(timer_);
            timer_ = nullptr;
        }
    }

    bool add(WindowFrameClock* clock) noexcept
    {
        if (clock == nullptr || timer_ == nullptr || wait_ == nullptr) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::find(clocks_.begin(), clocks_.end(), clock) == clocks_.end()) {
            clock->nextDeadlineQpc_ =
                clock->environmentSuppressed_.load(std::memory_order_acquire)
                    ? 0
                    : qpcNow() + effectivePeriod(*clock);
            clocks_.push_back(clock);
        }
        armNextLocked();
        return true;
    }

    void remove(WindowFrameClock* clock) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clocks_.erase(
            std::remove(clocks_.begin(), clocks_.end(), clock),
            clocks_.end());
        if (clocks_.empty()) {
            if (timer_ != nullptr) CancelWaitableTimer(timer_);
        } else {
            armNextLocked();
        }
    }

    bool pressureMode() const noexcept
    {
        return pressure_.load(std::memory_order_acquire);
    }

    void reportPresentOutcome(bool wasStillDrawing) noexcept
    {
        if (wasStillDrawing) {
            const auto refusals = consecutivePresentRefusals_.fetch_add(
                1, std::memory_order_acq_rel) + 1;
            cleanSinceQpc_.store(0, std::memory_order_release);
            if (refusals >= 2) pressure_.store(true, std::memory_order_release);
        } else {
            consecutivePresentRefusals_.store(0, std::memory_order_release);
        }
    }

    void environmentChanged() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (DisplayEnvironmentService::instance().suspended()) {
            if (timer_ != nullptr) CancelWaitableTimer(timer_);
            return;
        }

        const auto now = qpcNow();
        for (auto* clock : clocks_) {
            if (clock == nullptr ||
                !clock->running_.load(std::memory_order_acquire)) continue;
            const auto window = clock->window_.load(std::memory_order_acquire);
            const auto suppressed =
                !DisplayEnvironmentService::instance().presentationVisible(
                    window);
            const auto wasSuppressed = clock->environmentSuppressed_.exchange(
                suppressed, std::memory_order_acq_rel);
            if (suppressed) {
                clock->nextDeadlineQpc_ = 0;
                if (!wasSuppressed) {
                    clock->retireNotifications();
                }
                continue;
            }
            clock->updatePeriod(true);
            clock->nextDeadlineQpc_ = now + effectivePeriod(*clock);
            if (clock->readinessHandle_.load(std::memory_order_acquire)) {
                { std::lock_guard<std::mutex> readinessLock(clock->readinessMutex_);
                  clock->acceptingReadinessRearm_.store(true,std::memory_order_release); }
                clock->armReadinessWait();
            }
        }
        armNextLocked();
    }

    void deadlinesChanged() noexcept
    {
        // Logical scheduling only changes which already-known deadline should
        // wake the coordinator first. It must not refresh display state or
        // rebase peer clocks: a fast ambient producer would otherwise keep
        // pushing slower cursor and label deadlines into the future.
        std::lock_guard<std::mutex> lock(mutex_);
        armNextLocked();
    }

private:
    static VOID CALLBACK waitCallback(void* context, BOOLEAN timedOut)
    {
        if (timedOut || context == nullptr) return;
        static_cast<PresentationCoordinatorImpl*>(context)->onWake();
    }

    void onWake() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (DisplayEnvironmentService::instance().suspended()) {
            if (timer_ != nullptr) CancelWaitableTimer(timer_);
            return;
        }
        const auto now = qpcNow();
        std::uint64_t dueCount = 0;
        std::uint64_t missedCount = 0;
        bool anyMiss = false;

        for (auto* clock : clocks_) {
            if (clock == nullptr ||
                !clock->running_.load(std::memory_order_acquire) ||
                clock->environmentSuppressed_.load(
                    std::memory_order_acquire)) {
                continue;
            }
            if (clock->readinessHandle_.load(std::memory_order_acquire) !=
                    nullptr) {
                const auto logical = clock->logicalNotBeforeQpc_.load(
                    std::memory_order_acquire);
                if (logical != 0 && now >= logical) {
                    clock->logicalNotBeforeQpc_.store(
                        0, std::memory_order_release);
                    clock->armReadinessWait();
                }
                continue;
            }
            clock->updatePeriod(false);
            auto period = effectivePeriod(*clock);
            if (clock->nextDeadlineQpc_ == 0) {
                clock->nextDeadlineQpc_ = now + period;
            }
            const auto logical = clock->logicalNotBeforeQpc_.load(
                std::memory_order_acquire);
            const auto eligible = logical == 0
                ? clock->nextDeadlineQpc_
                : std::max(clock->nextDeadlineQpc_, logical);
            if (now < eligible) continue;
            if (logical != 0) clock->logicalNotBeforeQpc_.store(
                0, std::memory_order_release);

            ++dueCount;
            const auto latePeriods =
                (now - clock->nextDeadlineQpc_) / std::max<std::uint64_t>(1, period);
            PerformanceTelemetry::instance().wake(
                clock->telemetrySurface_.load(std::memory_order_relaxed),
                latePeriods);
            if (latePeriods != 0) {
                missedCount += latePeriods;
                clock->skippedDeadlines_.fetch_add(
                    latePeriods, std::memory_order_relaxed);
                anyMiss = true;
            }
            clock->nextDeadlineQpc_ += (latePeriods + 1U) * period;

            // Publishing the generation and claiming its notice are one
            // operation with rebind/stop. A delayed old producer must never
            // overwrite the helper's newer generation and strand its permit.
            std::lock_guard<std::mutex> readinessLock(clock->readinessMutex_);
            if (!clock->running_.load(std::memory_order_acquire) ||
                clock->environmentSuppressed_.load(std::memory_order_acquire) ||
                clock->readinessEpochChanging_.load(std::memory_order_acquire) ||
                clock->readinessHandle_.load(std::memory_order_acquire)) continue;
            bool expected = false;
            if (!clock->messagePending_.compare_exchange_strong(
                    expected, true,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                ++missedCount;
                clock->skippedDeadlines_.fetch_add(1, std::memory_order_relaxed);
                anyMiss = true;
                continue;
            }

            const auto window = clock->window_.load(std::memory_order_acquire);
            const auto message = clock->message_.load(std::memory_order_acquire);
            const auto generation =
                clock->generation_.load(std::memory_order_acquire);
            if (window == nullptr || message == 0 ||
                !clock->notification_.request(generation)) {
                clock->messagePending_.store(false, std::memory_order_release);
            }
        }

        updatePressure(now, dueCount, missedCount, anyMiss);
        armNextLocked();
    }

    void updatePressure(
        std::uint64_t now,
        std::uint64_t dueCount,
        std::uint64_t missedCount,
        bool anyMiss) noexcept
    {
        if (windowStartQpc_ == 0) windowStartQpc_ = now;
        windowDue_ += dueCount;
        windowMissed_ += missedCount;
        if (anyMiss) {
            ++consecutiveDeadlineMisses_;
            cleanSinceQpc_.store(0, std::memory_order_release);
        } else {
            consecutiveDeadlineMisses_ = 0;
            auto clean = cleanSinceQpc_.load(std::memory_order_acquire);
            if (clean == 0) cleanSinceQpc_.store(now, std::memory_order_release);
        }

        const auto elapsed = static_cast<double>(now - windowStartQpc_) /
            static_cast<double>(qpcFrequency_);
        if (consecutiveDeadlineMisses_ >= 3) {
            pressure_.store(true, std::memory_order_release);
        }
        if (elapsed >= kPressureWindowSeconds) {
            const auto ratio = windowDue_ != 0
                ? static_cast<double>(windowMissed_) /
                    static_cast<double>(windowDue_)
                : 0.0;
            if (windowDue_ >= 10 && ratio > 0.10) {
                pressure_.store(true, std::memory_order_release);
            }
            const auto clean = cleanSinceQpc_.load(std::memory_order_acquire);
            const auto cleanSeconds = clean != 0 && now > clean
                ? static_cast<double>(now - clean) /
                    static_cast<double>(qpcFrequency_)
                : 0.0;
            if (pressure_.load(std::memory_order_acquire) &&
                ratio < 0.02 && cleanSeconds >= kPressureRecoverySeconds &&
                consecutivePresentRefusals_.load(std::memory_order_acquire) == 0) {
                pressure_.store(false, std::memory_order_release);
            }
            windowStartQpc_ = now;
            windowDue_ = 0;
            windowMissed_ = 0;
        }
    }

    std::uint64_t effectivePeriod(const WindowFrameClock& clock) const noexcept
    {
        auto nominal = std::max<std::uint64_t>(
            1, clock.periodTicks_.load(std::memory_order_acquire));
        const auto maximumMilliHz = clock.maximumRefreshMilliHz_.load(
            std::memory_order_acquire);
        if (maximumMilliHz != 0) {
            nominal = std::max<std::uint64_t>(
                nominal,
                static_cast<std::uint64_t>(std::llround(
                    static_cast<double>(qpcFrequency_) * 1000.0 /
                    static_cast<double>(maximumMilliHz))));
        }
        if (!pressure_.load(std::memory_order_acquire) ||
            clock.latencyCritical_.load(std::memory_order_acquire)) {
            return nominal;
        }
        return nekorox::pressurePresentationPeriodTicks(
            nominal,
            qpcFrequency_,
            clock.targetRefreshMilliHz_.load(std::memory_order_acquire));
    }

    void armNextLocked() noexcept
    {
        if (timer_ == nullptr || clocks_.empty()) return;
        if (DisplayEnvironmentService::instance().suspended()) {
            CancelWaitableTimer(timer_);
            return;
        }
        const auto now = qpcNow();
        auto earliest = (std::numeric_limits<std::uint64_t>::max)();
        for (const auto* clock : clocks_) {
            if (clock != nullptr &&
                clock->running_.load(std::memory_order_acquire) &&
                !clock->environmentSuppressed_.load(
                    std::memory_order_acquire)) {
                const auto logical = clock->logicalNotBeforeQpc_.load(
                    std::memory_order_acquire);
                if (clock->readinessHandle_.load(std::memory_order_acquire) !=
                        nullptr) {
                    if (logical != 0) earliest = std::min(earliest, logical);
                } else if (clock->nextDeadlineQpc_ != 0) {
                    earliest = std::min(earliest, logical == 0
                        ? clock->nextDeadlineQpc_
                        : std::max(clock->nextDeadlineQpc_, logical));
                }
            }
        }
        if (earliest == (std::numeric_limits<std::uint64_t>::max)()) {
            CancelWaitableTimer(timer_);
            return;
        }
        const auto delayTicks = earliest > now ? earliest - now : 1U;
        const auto delay100ns = std::max<LONGLONG>(
            1, static_cast<LONGLONG>(std::llround(
                static_cast<double>(delayTicks) * 10000000.0 /
                static_cast<double>(qpcFrequency_))));
        LARGE_INTEGER due{};
        due.QuadPart = -delay100ns;
        SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
    }

    std::uint64_t qpcNow() const noexcept
    {
        LARGE_INTEGER value{};
        QueryPerformanceCounter(&value);
        return value.QuadPart > 0
            ? static_cast<std::uint64_t>(value.QuadPart)
            : 0;
    }

    mutable std::mutex mutex_;
    std::vector<WindowFrameClock*> clocks_;
    HANDLE timer_ = nullptr;
    HANDLE wait_ = nullptr;
    std::uint64_t qpcFrequency_ = 1;
    std::uint64_t windowStartQpc_ = 0;
    std::uint64_t windowDue_ = 0;
    std::uint64_t windowMissed_ = 0;
    std::uint32_t consecutiveDeadlineMisses_ = 0;
    std::atomic<std::uint32_t> consecutivePresentRefusals_{0};
    std::atomic<std::uint64_t> cleanSinceQpc_{0};
    std::atomic<bool> pressure_{false};
};

namespace {

PresentationCoordinatorImpl& presentationCoordinator()
{
    static PresentationCoordinatorImpl coordinator;
    return coordinator;
}

} // namespace

WindowFrameClock::WindowFrameClock()
{
    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
        qpcFrequency_ = static_cast<std::uint64_t>(frequency.QuadPart);
    }
    periodTicks_.store(
        std::max<std::uint64_t>(1, qpcFrequency_ / 120U),
        std::memory_order_relaxed);
    readinessWait_ = CreateThreadpoolWait(
        &WindowFrameClock::readinessCallback, this, nullptr);
}

WindowFrameClock::~WindowFrameClock()
{
    stop();
    setReadinessBinding({});
    notification_.reset();
    if (readinessWait_ != nullptr) {
        CloseThreadpoolWait(readinessWait_);
        readinessWait_ = nullptr;
    }
    window_.store(nullptr, std::memory_order_release);
    message_.store(0, std::memory_order_release);
}

void WindowFrameClock::attach(HWND window, UINT message) noexcept
{
    if (window_.load(std::memory_order_acquire) == window &&
        message_.load(std::memory_order_acquire) == message) return;
    stop();
    setReadinessBinding({});
    if (!notification_.attach(window, message)) {
        window_.store(nullptr, std::memory_order_release);
        message_.store(0, std::memory_order_release);
        return;
    }
    window_.store(window, std::memory_order_release);
    message_.store(message, std::memory_order_release);
}

void WindowFrameClock::detach() noexcept
{
    stop();
    setReadinessBinding({});
    notification_.reset();
    message_.store(0, std::memory_order_release);
    window_.store(nullptr, std::memory_order_release);
}

bool WindowFrameClock::start() noexcept
{
    if (running_.load(std::memory_order_acquire)) return true;
    if (!window_.load(std::memory_order_acquire) ||
        !message_.load(std::memory_order_acquire)) return false;
    updatePeriod(true);
    environmentSuppressed_.store(
        !DisplayEnvironmentService::instance().presentationVisible(
            window_.load(std::memory_order_acquire)),std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(readinessMutex_);
        generation_.fetch_add(1,std::memory_order_acq_rel);
        messagePending_.store(false,std::memory_order_release);
        notification_.cancel();
        running_.store(true,std::memory_order_release);
        acceptingReadinessRearm_.store(true,std::memory_order_release);
    }
    if (!presentationCoordinator().add(this)) {
        running_.store(false,std::memory_order_release);return false;
    }
    g_activeFrameClocks.fetch_add(1,std::memory_order_acq_rel);
    if (readinessHandle_.load(std::memory_order_acquire)) armReadinessWait();
    return true;
}

void WindowFrameClock::retireNotifications() noexcept
{
    std::lock_guard<std::mutex> lock(readinessMutex_);
    // Notification generations are not swap-chain identities. Return a grant
    // delivered to WM_PAINT but not yet used; never invent a new grant.
    if (uiOpportunity_.exchange(false, std::memory_order_acq_rel))
        readinessPermit_.store(true, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_acq_rel);
    messagePending_.store(false, std::memory_order_release);
    notification_.cancel();
}
bool WindowFrameClock::postReadyLocked() noexcept
{
    if (!running_.load(std::memory_order_acquire) ||
        environmentSuppressed_.load(std::memory_order_acquire) ||
        readinessEpochChanging_.load(std::memory_order_acquire) ||
        !readinessPermit_.load(std::memory_order_acquire)) return false;
    if (messagePending_.load(std::memory_order_acquire)) return true;
    // Hold genuine DXGI credits until the slow content deadline. Input clears
    // that deadline without inventing or discarding presentation ownership.
    const auto notBefore = logicalNotBeforeQpc_.load(std::memory_order_acquire);
    if (notBefore && qpcNow() < notBefore) return true;
    const auto window=window_.load(std::memory_order_acquire);
    const auto message=message_.load(std::memory_order_acquire);
    if (!window || !message) return false;
    messagePending_.store(true,std::memory_order_release);
    if (!notification_.request(generation_.load(std::memory_order_acquire))) {
        messagePending_.store(false,std::memory_order_release);
        return false; // Grant remains held; a subsequent request can repost it.
    }
    return true;
}

void WindowFrameClock::stop() noexcept
{
    if (!running_.exchange(false,std::memory_order_acq_rel)) return;
    disarmReadinessWait();
    presentationCoordinator().remove(this);
    g_activeFrameClocks.fetch_sub(1,std::memory_order_acq_rel);
    retireNotifications();
    nextDeadlineQpc_=0;
    logicalNotBeforeQpc_.store(0,std::memory_order_release);
    const auto reasons=pendingReasonMask_.exchange(0,std::memory_order_acq_rel);
    if (reasons) PerformanceTelemetry::instance().cacheReason(
        telemetrySurface_.load(std::memory_order_acquire),0xA100U,reasons);
}

bool WindowFrameClock::requestImmediate() noexcept
{
    logicalNotBeforeQpc_.store(0, std::memory_order_release);
    pendingReasonMask_.fetch_or(
        kFrameReasonSemantic, std::memory_order_acq_rel);
    const auto wasRunning = running_.load(std::memory_order_acquire);
    if (!wasRunning && !start()) return false;
    const auto window = window_.load(std::memory_order_acquire);
    const auto suppressed =
        !DisplayEnvironmentService::instance().presentationVisible(window);
    const auto wasSuppressed = environmentSuppressed_.exchange(
        suppressed, std::memory_order_acq_rel);
    if (suppressed) {
        if (!wasSuppressed) {
            // A readiness wait is one-shot. If it fires while a child HWND is
            // hidden, no WM_PAINT follows to complete/rearm the opportunity.
            // Retire that epoch now; the first request after show establishes
            // a fresh wait and presents the latest semantic revision.
            retireNotifications();
            disarmReadinessWait();
            presentationCoordinator().environmentChanged();
        }
        return false;
    }
    if (wasSuppressed) {
        acceptingReadinessRearm_.store(true,std::memory_order_release);
        presentationCoordinator().environmentChanged();
    }
    if (readinessHandle_.load(std::memory_order_acquire) != nullptr &&
        rolloutFlagEnabled(RolloutFlag::presentationOpportunityR2)) {
        // Immediate is semantic demand. The one readiness authority decides
        // when that demand may prepare and present; it is not a second clock.
        return armReadinessWait();
    }
    if (wasRunning && rolloutFlagEnabled(RolloutFlag::presentationPacingV1)) {
        const auto consumed = lastConsumedQpc_.load(std::memory_order_acquire);
        const auto now = qpcNow();
        if (consumed != 0 && now > consumed &&
            now - consumed < periodTicks_.load(std::memory_order_acquire)) {
            // The coordinator already owns the next display deadline. Keep
            // the semantic revision dirty without posting another early pulse.
            return true;
        }
    }
    std::lock_guard<std::mutex> readinessLock(readinessMutex_);
    if (!running_.load(std::memory_order_acquire) ||
        environmentSuppressed_.load(std::memory_order_acquire) ||
        readinessEpochChanging_.load(std::memory_order_acquire)) return false;
    bool expected = false;
    if (!messagePending_.compare_exchange_strong(
            expected, true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return true;
    }
    const auto message = message_.load(std::memory_order_acquire);
    const auto generation = generation_.load(std::memory_order_acquire);
    if (window == nullptr || message == 0 ||
        !notification_.request(generation)) {
        messagePending_.store(false, std::memory_order_release);
        return false;
    }
    PerformanceTelemetry::instance().wake(
        telemetrySurface_.load(std::memory_order_relaxed));
    return true;
}

bool WindowFrameClock::requestAtOrAfter(
    std::uint64_t notBeforeQpc, std::uint32_t reasonMask) noexcept
{
    if (notBeforeQpc == 0 || reasonMask == 0) return requestImmediate();
    constexpr auto urgent=kFrameReasonSemantic|kFrameReasonInput|kFrameReasonLyrics;
    if ((pendingReasonMask_.load(std::memory_order_acquire)&urgent)!=0 ||
        messagePending_.load(std::memory_order_acquire) || uiOpportunity_.load(std::memory_order_acquire)) {
        pendingReasonMask_.fetch_or(reasonMask,std::memory_order_acq_rel);
        return true;
    }
    auto current = logicalNotBeforeQpc_.load(std::memory_order_acquire);
    while ((current == 0 || notBeforeQpc < current) &&
        !logicalNotBeforeQpc_.compare_exchange_weak(
            current, notBeforeQpc, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
    }
    pendingReasonMask_.fetch_or(reasonMask, std::memory_order_acq_rel);
    if (!running_.load(std::memory_order_acquire) && !start()) return false;
    presentationCoordinator().deadlinesChanged();
    return true;
}

bool WindowFrameClock::consume(WPARAM generation) noexcept
{
    {
        std::lock_guard<std::mutex> lock(readinessMutex_);
        // A stale notification must not clear the flag of a newer notification.
        if (!running_.load(std::memory_order_acquire) ||
            static_cast<std::uint32_t>(generation)!=generation_.load(std::memory_order_acquire))
            return false;
        messagePending_.store(false,std::memory_order_release);
        if (environmentSuppressed_.load(std::memory_order_acquire)) return false;
        if (readinessHandle_.load(std::memory_order_acquire) &&
            rolloutFlagEnabled(RolloutFlag::presentationOpportunityR2)) {
            if (!readinessPermit_.exchange(false,std::memory_order_acq_rel)) return false;
            uiOpportunity_.store(true,std::memory_order_release);
        }
    }
    lastConsumedQpc_.store(qpcNow(),std::memory_order_release);
    const auto reasons=pendingReasonMask_.exchange(0,std::memory_order_acq_rel);
    if (reasons) PerformanceTelemetry::instance().cacheReason(
        telemetrySurface_.load(std::memory_order_acquire),0xA101U,reasons);
    deliveredPulses_.fetch_add(1,std::memory_order_relaxed);
    return true;
}

void WindowFrameClock::setReadinessHandle(HANDLE readiness) noexcept
{
    SurfaceBindingKey legacy{};
    if (readiness != nullptr) {
        legacy.lifetime = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(readiness));
        legacy.swapchainEpoch = 1;
        legacy.source = readiness;
    }
    setReadinessBinding(legacy);
}

void WindowFrameClock::setReadinessBinding(
    SurfaceBindingKey binding) noexcept
{
    if (!binding.valid()) binding = {};
    {
        std::lock_guard<std::mutex> lock(readinessMutex_);
        if (readinessBinding_ == binding) return;
    }
    HANDLE duplicate = nullptr;
    if (binding.source != nullptr) {
        (void)DuplicateHandle(GetCurrentProcess(), binding.source,
            GetCurrentProcess(), &duplicate, SYNCHRONIZE, FALSE, 0);
        if (duplicate == nullptr) binding = {};
    }
    auto* wait = readinessWait_;
    HANDLE retired = nullptr;
    {
        std::lock_guard<std::mutex> lock(readinessMutex_);
        if (readinessBinding_ == binding) {
            if (duplicate != nullptr) CloseHandle(duplicate);
            return;
        }
        acceptingReadinessRearm_.store(false, std::memory_order_release);
        readinessEpochChanging_.store(true, std::memory_order_release);
        readinessBindingGeneration_.fetch_add(1,std::memory_order_acq_rel);
        if(readinessPermit_.load(std::memory_order_acquire)||uiOpportunity_.load(std::memory_order_acquire))
            readinessRetiredHeld_.fetch_add(1,std::memory_order_relaxed);
        readinessWaitArmed_.store(false, std::memory_order_release);
        if (wait != nullptr) SetThreadpoolWait(wait, nullptr, nullptr);
        // Remove the borrowed HANDLE from callback authority before its owner
        // is allowed to close the swap-chain epoch.
        retired = readinessHandle_.exchange(nullptr, std::memory_order_acq_rel);
        readinessSourceHandle_ = nullptr;
        readinessBinding_ = {};
    }
    if (wait != nullptr) WaitForThreadpoolWaitCallbacks(wait, TRUE);
    if (retired != nullptr) CloseHandle(retired);
    {
        std::lock_guard<std::mutex> lock(readinessMutex_);
        readinessSourceHandle_ = binding.source;
        readinessBinding_ = binding;
        readinessHandle_.store(duplicate, std::memory_order_release);
        readinessPermit_.store(false, std::memory_order_release);
        uiOpportunity_.store(false, std::memory_order_release);
        generation_.fetch_add(1, std::memory_order_acq_rel);
        messagePending_.store(false, std::memory_order_release);
        notification_.cancel();
        readinessEpochChanging_.store(false, std::memory_order_release);
        acceptingReadinessRearm_.store(
            duplicate != nullptr, std::memory_order_release);
    }
    if (running_.load(std::memory_order_acquire) && duplicate != nullptr) {
        armReadinessWait();
    }
    presentationCoordinator().environmentChanged();
}

bool WindowFrameClock::beginPresentationOpportunity() noexcept
{
    if (!rolloutFlagEnabled(RolloutFlag::presentationOpportunityR2) ||
        !readinessHandle_.load(std::memory_order_acquire)) return true;
    if (!running_.load(std::memory_order_acquire) && !start()) return false;
    {
        std::lock_guard<std::mutex> lock(readinessMutex_);
        if (uiOpportunity_.exchange(false,std::memory_order_acq_rel)) {
            activeOpportunityGeneration_=readinessBindingGeneration_.load(std::memory_order_acquire);
            readinessBegun_.fetch_add(1,std::memory_order_relaxed);
            return true;
        }
    }
    PerformanceTelemetry::instance().readinessNotReady(
        telemetrySurface_.load(std::memory_order_relaxed),
        readinessBindingGeneration_.load(std::memory_order_acquire));
    armReadinessWait();return false;
}

void WindowFrameClock::completePresentationOpportunity(
    bool needsAnotherFrame) noexcept
{
    completePresentationOpportunity(needsAnotherFrame, 0, 0);
}

void WindowFrameClock::completePresentationOpportunity(
    bool needsAnotherFrame, std::uint64_t notBeforeQpc,
    std::uint32_t reasonMask) noexcept
{
    if (!rolloutFlagEnabled(RolloutFlag::presentationOpportunityR2) ||
        readinessHandle_.load(std::memory_order_acquire) == nullptr) return;
    if (activeOpportunityGeneration_ !=
        readinessBindingGeneration_.load(std::memory_order_acquire)) return;
    if (!needsAnotherFrame) {
        stop();
        return;
    }
    // A free DXGI queue slot is ownership, not a display tick. Present(0)
    // can retire hundreds of frames per second without displaying them.
    // Continuous animation still follows the monitor when no content cap is
    // requested; real input below may wake its retained credit immediately.
    const auto capped = displayPresentationDeadline(
        lastConsumedQpc_.load(std::memory_order_acquire), qpcFrequency_,
        maximumRefreshMilliHz_.load(std::memory_order_acquire),
        periodTicks_.load(std::memory_order_acquire));
    const auto urgent = pendingReasonMask_.load(std::memory_order_acquire) &
        (kFrameReasonInput | kFrameReasonSemantic | kFrameReasonLyrics);
    if (urgent != 0) { notBeforeQpc = 0; reasonMask = 0; }
    else if (capped != 0 && capped > qpcNow()) {
        notBeforeQpc = std::max(notBeforeQpc, capped);
        reasonMask |= kFrameReasonAmbient;
    }
    if (notBeforeQpc != 0 && reasonMask != 0) {
        logicalNotBeforeQpc_.store(notBeforeQpc, std::memory_order_release);
        pendingReasonMask_.fetch_or(reasonMask, std::memory_order_acq_rel);
        presentationCoordinator().deadlinesChanged();
    }
    if (!running_.load(std::memory_order_acquire)) start();
    armReadinessWait();
}

void WindowFrameClock::disarmReadinessWait() noexcept
{
    auto* wait=readinessWait_;if (!wait) return;
    {
        std::lock_guard<std::mutex> lock(readinessMutex_);
        acceptingReadinessRearm_.store(false,std::memory_order_release);
        SetThreadpoolWait(wait,nullptr,nullptr);
    }
    // FALSE lets an already queued signal callback seal its held credit. A
    // cancelled callback could have consumed an auto-reset signal. The callback
    // never re-arms while acceptingReadinessRearm_ is false.
    WaitForThreadpoolWaitCallbacks(wait,FALSE);
    readinessWaitArmed_.store(false,std::memory_order_release);
}

bool WindowFrameClock::armReadinessWait() noexcept
{
    const auto logical=logicalNotBeforeQpc_.load(std::memory_order_acquire);
    if (logical && qpcNow()<logical) return true;
    std::lock_guard<std::mutex> lock(readinessMutex_);
    if (!readinessWait_ || !acceptingReadinessRearm_.load(std::memory_order_acquire) ||
        !running_.load(std::memory_order_acquire) ||
        readinessEpochChanging_.load(std::memory_order_acquire) ||
        environmentSuppressed_.load(std::memory_order_acquire)) return false;
    const auto readiness=readinessHandle_.load(std::memory_order_acquire);
    if (!readiness) return true;
    if (readinessPermit_.load(std::memory_order_acquire)) return postReadyLocked();
    if (readinessWaitArmed_.load(std::memory_order_acquire) ||
        uiOpportunity_.load(std::memory_order_acquire)) return true;
    if (!window_.load(std::memory_order_acquire) || !message_.load(std::memory_order_acquire)) return false;
    const auto ms=static_cast<DWORD>(std::clamp<std::uint64_t>(
        (periodTicks_.load(std::memory_order_acquire)*1000U+qpcFrequency_-1U)/
            std::max<std::uint64_t>(1,qpcFrequency_),1U,100U));
    ULARGE_INTEGER encoded{};encoded.QuadPart=static_cast<ULONGLONG>(-static_cast<LONGLONG>(ms)*10000LL);
    FILETIME timeout{encoded.LowPart,encoded.HighPart};
    const auto epoch=readinessBindingGeneration_.load(std::memory_order_acquire);
    readinessArmedGeneration_.store(epoch,std::memory_order_release);
    readinessWaitArmed_.store(true,std::memory_order_release);
    SetThreadpoolWait(readinessWait_,readiness,&timeout);
    PerformanceTelemetry::instance().waitArmed(telemetrySurface_.load(std::memory_order_relaxed),epoch);
    return true;
}

VOID CALLBACK WindowFrameClock::readinessCallback(
    PTP_CALLBACK_INSTANCE,void* value,PTP_WAIT,TP_WAIT_RESULT result)
{
    auto* owner=static_cast<WindowFrameClock*>(value);if (!owner) return;
    std::uint32_t epoch=0;bool rearm=false;
    {
        std::lock_guard<std::mutex> lock(owner->readinessMutex_);
        owner->readinessWaitArmed_.store(false,std::memory_order_release);
        epoch=owner->readinessArmedGeneration_.load(std::memory_order_acquire);
        if (owner->readinessEpochChanging_.load(std::memory_order_acquire) ||
            epoch!=owner->readinessBindingGeneration_.load(std::memory_order_acquire) ||
            !owner->readinessHandle_.load(std::memory_order_acquire)) return;
        if (result==WAIT_OBJECT_0) {
            // A genuine signal remains valid when stop/hide merely retires its
            // notification. Its ownership ends on use or real binding change.
            if (!owner->readinessPermit_.load(std::memory_order_acquire) &&
                !owner->uiOpportunity_.load(std::memory_order_acquire)) {
                owner->readinessPermit_.store(true,std::memory_order_release);
                owner->readinessAcquired_.fetch_add(1,std::memory_order_relaxed);
            }
            owner->postReadyLocked();
        } else {
            rearm=owner->acceptingReadinessRearm_.load(std::memory_order_acquire) &&
                owner->running_.load(std::memory_order_acquire);
        }
    }
    if (result==WAIT_OBJECT_0) {
        PerformanceTelemetry::instance().readySignaled(
            owner->telemetrySurface_.load(std::memory_order_relaxed),epoch);
    } else {
        // WAIT_TIMEOUT or failure is not a drawing authorization.
        PerformanceTelemetry::instance().readinessNotReady(
            owner->telemetrySurface_.load(std::memory_order_relaxed),epoch);
        if (rearm) owner->armReadinessWait();
    }
}

void WindowFrameClock::setLatencyCritical(bool critical) noexcept
{
    latencyCritical_.store(critical, std::memory_order_release);
}

void WindowFrameClock::setMaximumRefreshHz(double refreshHz) noexcept
{
    const auto milliHz = std::isfinite(refreshHz) && refreshHz > 0.0
        ? static_cast<std::uint32_t>(std::clamp(
            std::llround(refreshHz * 1000.0),
            1LL,
            static_cast<long long>(
                (std::numeric_limits<std::uint32_t>::max)())))
        : 0U;
    const auto previous = maximumRefreshMilliHz_.exchange(
        milliHz, std::memory_order_acq_rel);
    if (previous != milliHz &&
        running_.load(std::memory_order_acquire)) {
        requestImmediate();
    }
}

void WindowFrameClock::setTelemetrySurface(TelemetrySurface surface) noexcept
{
    telemetrySurface_.store(surface, std::memory_order_release);
}

double WindowFrameClock::intervalSeconds() const noexcept
{
    return static_cast<double>(
        periodTicks_.load(std::memory_order_acquire)) /
        static_cast<double>(std::max<std::uint64_t>(1, qpcFrequency_));
}

std::uint32_t WindowFrameClock::targetRefreshMilliHz() const noexcept
{
    return targetRefreshMilliHz_.load(std::memory_order_acquire);
}

std::uint64_t WindowFrameClock::deliveredPulses() const noexcept
{
    return deliveredPulses_.load(std::memory_order_relaxed);
}

std::uint64_t WindowFrameClock::skippedDeadlines() const noexcept
{
    return skippedDeadlines_.load(std::memory_order_relaxed);
}

std::uint32_t WindowFrameClock::diagnosticActiveCount() noexcept
{
    return g_activeFrameClocks.load(std::memory_order_acquire);
}

bool WindowFrameClock::pressureMode() noexcept
{
    return presentationCoordinator().pressureMode();
}

void WindowFrameClock::reportPresentOutcome(bool wasStillDrawing) noexcept
{
    presentationCoordinator().reportPresentOutcome(wasStillDrawing);
}

void WindowFrameClock::environmentChanged() noexcept
{
    presentationCoordinator().environmentChanged();
}

void WindowFrameClock::updatePeriod(bool force) noexcept
{
    const auto now = qpcNow();
    auto& display = DisplayEnvironmentService::instance();
    const auto window = window_.load(std::memory_order_acquire);
    const auto identity = display.windowIdentity(window);
    const auto environmentGeneration = identity.topologyGeneration;
    if (!force && lastRefreshQueryQpc_ != 0 &&
        environmentGeneration == lastEnvironmentGeneration_ &&
        identity.windowPresentationRevision ==
            lastWindowPresentationRevision_ &&
        identity.rootLifetimeToken == lastRootLifetimeToken_ &&
        now - lastRefreshQueryQpc_ < qpcFrequency_ * kRefreshQuerySeconds) return;
    lastRefreshQueryQpc_ = now;
    lastEnvironmentGeneration_ = environmentGeneration;
    lastWindowPresentationRevision_ = identity.windowPresentationRevision;
    lastRootLifetimeToken_ = identity.rootLifetimeToken;

    const auto environment = display.snapshot(window);
    const auto numerator = (std::max)(1U, environment.refreshNumerator);
    const auto denominator = (std::max)(1U, environment.refreshDenominator);
    targetRefreshMilliHz_.store(
        environment.refreshMilliHz(), std::memory_order_release);
    periodTicks_.store(
        nekorox::presentationPeriodTicks(
            qpcFrequency_, numerator, denominator),
        std::memory_order_release);
}

std::uint64_t WindowFrameClock::qpcNow() const noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart > 0
        ? static_cast<std::uint64_t>(value.QuadPart)
        : 0;
}

} // namespace nekorox::component
