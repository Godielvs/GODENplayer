#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "nekorox/qa_contracts.h"
#include <windows.h>

namespace nekorox::component {

enum class TelemetrySurface : std::uint8_t {
    transport,
    waveSeek,
    headerMonitor,
    workspace,
    spectrum,
    listen,
    listenCrt,
    daw,
    artwork,
    album,
    albumAnalysis,
    graphics,
    display,
    count,
};

struct SurfaceTelemetrySnapshot {
    std::uint64_t wakes = 0;
    std::uint64_t analysisFrames = 0;
    std::uint64_t paints = 0;
    std::uint64_t uploadBytes = 0;
    std::uint64_t endDraws = 0;
    std::uint64_t presents = 0;
    std::uint64_t refusedFrames = 0;
    std::uint64_t backlogFrames = 0;
    std::uint64_t allocations = 0;
    std::uint64_t inputToPresentSamples = 0;
    std::uint64_t inputToPresentTotalMicroseconds = 0;
    std::uint64_t inputToPresentMaximumMicroseconds = 0;
};

// Process-local counters plus an ETW TraceLogging provider. Disabled ETW has
// only the provider-enabled branch; counters remain available to QA probes.
class PerformanceTelemetry final {
public:
    static PerformanceTelemetry& instance() noexcept;

    void wake(TelemetrySurface surface, std::uint64_t backlog = 0) noexcept;
    void analysis(TelemetrySurface surface) noexcept;
    void paint(TelemetrySurface surface) noexcept;
    void upload(TelemetrySurface surface, std::uint64_t bytes) noexcept;
    void endDraw(TelemetrySurface surface, long result) noexcept;
    void present(
        TelemetrySurface surface,
        long result,
        bool refused = false) noexcept;
    void allocation(TelemetrySurface surface) noexcept;
    void input(TelemetrySurface surface) noexcept;
    void inputIncorporated(TelemetrySurface surface) noexcept;
    void frameAttempt(TelemetrySurface surface) noexcept;
    void frameResult(
        TelemetrySurface surface, long result, bool acceptedVisible) noexcept;
    // Only an external presentation observer (ETW/PresentMon/test adapter) may
    // call this. Present(S_OK) alone is deliberately not a display event.
    void displayed(TelemetrySurface surface, std::uint64_t frameId) noexcept;
    void readinessNotReady(
        TelemetrySurface surface, std::uint64_t epoch) noexcept;
    void waitArmed(TelemetrySurface surface, std::uint64_t epoch) noexcept;
    void readySignaled(TelemetrySurface surface, std::uint64_t epoch) noexcept;
    [[nodiscard]] std::uint64_t beginSpan(
        TelemetrySurface surface,
        std::uint64_t phase,
        std::uint64_t detail = 0) noexcept;
    void endSpan(
        TelemetrySurface surface,
        std::uint64_t phase,
        std::uint64_t startedQpc,
        std::uint64_t detail = 0) noexcept;
    void cacheReason(
        TelemetrySurface surface,
        std::uint64_t reason,
        std::uint64_t detail = 0) noexcept;
    void collectionMaterialized(
        TelemetrySurface surface,
        std::uint64_t collection,
        std::uint64_t count) noexcept;
    void event(
        nekorox::PerformanceEventKindV2 kind,
        TelemetrySurface surface,
        std::uint64_t argument0 = 0,
        std::uint64_t argument1 = 0,
        std::uint64_t argument2 = 0,
        std::uint64_t argument3 = 0,
        std::uint64_t durationQpc = 0) noexcept;
    long startUiHeartbeat() noexcept;
    void stopUiHeartbeat() noexcept;

    [[nodiscard]] SurfaceTelemetrySnapshot snapshot(
        TelemetrySurface surface) const noexcept;

private:
    struct Counters {
        std::atomic<std::uint64_t> wakes{0};
        std::atomic<std::uint64_t> analysisFrames{0};
        std::atomic<std::uint64_t> paints{0};
        std::atomic<std::uint64_t> uploadBytes{0};
        std::atomic<std::uint64_t> endDraws{0};
        std::atomic<std::uint64_t> presents{0};
        std::atomic<std::uint64_t> refusedFrames{0};
        std::atomic<std::uint64_t> backlogFrames{0};
        std::atomic<std::uint64_t> allocations{0};
        std::atomic<std::uint64_t> oldestPendingInputQpc{0};
        std::atomic<std::uint64_t> newestReceivedInputQpc{0};
        std::atomic<std::uint64_t> newestIncorporatedInputQpc{0};
        std::atomic<std::uint64_t> inputSamples{0};
        std::atomic<std::uint64_t> inputTotalMicroseconds{0};
        std::atomic<std::uint64_t> inputMaximumMicroseconds{0};
    };

    PerformanceTelemetry() noexcept;
    ~PerformanceTelemetry();
    static std::size_t index(TelemetrySurface surface) noexcept;
    static const char* name(TelemetrySurface surface) noexcept;
    static std::uint64_t qpcNow() noexcept;
    static LRESULT CALLBACK heartbeatWindowProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept;

    struct SharedEventRing;

    std::array<Counters, static_cast<std::size_t>(TelemetrySurface::count)>
        counters_{};
    std::uint64_t qpcFrequency_ = 1;
    void* eventMapping_ = nullptr;
    SharedEventRing* eventRing_ = nullptr;
    void* heartbeatWindow_ = nullptr;
    std::atomic<bool> heartbeatPostPending_{false};
    std::jthread heartbeatThread_;
};

} // namespace nekorox::component
