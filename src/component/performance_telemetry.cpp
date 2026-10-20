#include "qa_hooks.h"
#include "performance_telemetry.h"
#include "rollout_flags.h"

#include <windows.h>
#include <TraceLoggingProvider.h>

#include <algorithm>
#include <cstring>
#include <string>

TRACELOGGING_DEFINE_PROVIDER(
    g_godenPerformanceProvider,
    "Goden.foobar2000.Performance",
    (0xa0cc36b4, 0x4618, 0x4da6, 0x9f, 0x1b, 0xb5, 0xf3, 0xaf, 0xaa, 0x65, 0x81));

namespace nekorox::component {

struct PerformanceTelemetry::SharedEventRing {
    nekorox::PerformanceEventRingHeaderV2 header{};
    std::array<nekorox::PerformanceEventV2,
        nekorox::kPerformanceEventCapacityV2> records{};
};

namespace {

constexpr UINT kHeartbeatMessage = WM_APP + 0x71;

bool qaTraceAllowed() noexcept
{
    if (!qa::hooksAllowed()) return false;
    wchar_t hooks[8]{};
    wchar_t profile[8]{};
    return GetEnvironmentVariableW(L"NEKOROX_QA_HOOKS", hooks,
               static_cast<DWORD>(std::size(hooks))) == 1U &&
        hooks[0] == L'1' &&
        GetEnvironmentVariableW(L"NEKOROX_QA_PROFILE", profile,
               static_cast<DWORD>(std::size(profile))) == 1U &&
        profile[0] == L'1';
}

bool diagnosticTraceAllowed() noexcept
{
    wchar_t enabled[8]{};
    return GetEnvironmentVariableW(L"GODEN_UI_DIAGNOSTICS", enabled,
        static_cast<DWORD>(std::size(enabled))) == 1U && enabled[0] == L'1';
}

} // namespace

PerformanceTelemetry::PerformanceTelemetry() noexcept
{
    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
        qpcFrequency_ = static_cast<std::uint64_t>(frequency.QuadPart);
    }
    TraceLoggingRegister(g_godenPerformanceProvider);
    if (diagnosticTraceAllowed() || (qaTraceAllowed() &&
            rolloutFlagEnabled(RolloutFlag::qaTraceV2))) {
        const auto name = std::wstring(
            nekorox::kPerformanceEventMappingPrefixV2) +
            std::to_wstring(GetCurrentProcessId());
        eventMapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
            PAGE_READWRITE, 0, static_cast<DWORD>(sizeof(SharedEventRing)),
            name.c_str());
        if (eventMapping_ != nullptr) {
            eventRing_ = static_cast<SharedEventRing*>(MapViewOfFile(
                eventMapping_, FILE_MAP_ALL_ACCESS, 0, 0,
                sizeof(SharedEventRing)));
        }
        if (eventRing_ != nullptr) {
            std::memset(eventRing_, 0, sizeof(*eventRing_));
            eventRing_->header.magic = nekorox::kPerformanceEventMagicV2;
            eventRing_->header.version = 2;
            eventRing_->header.recordBytes =
                sizeof(nekorox::PerformanceEventV2);
            eventRing_->header.capacity =
                nekorox::kPerformanceEventCapacityV2;
            eventRing_->header.ownerPid = GetCurrentProcessId();
            eventRing_->header.qpcFrequency = qpcFrequency_;
            eventRing_->header.startupEpoch = qpcNow() ^ GetTickCount64();
        } else if (eventMapping_ != nullptr) {
            CloseHandle(eventMapping_);
            eventMapping_ = nullptr;
        }
    }
}

PerformanceTelemetry::~PerformanceTelemetry()
{
    stopUiHeartbeat();
    if (eventRing_ != nullptr) UnmapViewOfFile(eventRing_);
    if (eventMapping_ != nullptr) CloseHandle(eventMapping_);
    TraceLoggingUnregister(g_godenPerformanceProvider);
}

PerformanceTelemetry& PerformanceTelemetry::instance() noexcept
{
    static PerformanceTelemetry telemetry;
    return telemetry;
}

std::size_t PerformanceTelemetry::index(TelemetrySurface surface) noexcept
{
    return std::min(
        static_cast<std::size_t>(surface),
        static_cast<std::size_t>(TelemetrySurface::count) - 1U);
}

const char* PerformanceTelemetry::name(TelemetrySurface surface) noexcept
{
    switch (surface) {
    case TelemetrySurface::transport: return "Transport";
    case TelemetrySurface::waveSeek: return "WaveSeek";
    case TelemetrySurface::headerMonitor: return "HeaderMonitor";
    case TelemetrySurface::workspace: return "Workspace";
    case TelemetrySurface::spectrum: return "Spectrum";
    case TelemetrySurface::listen: return "Listen";
    case TelemetrySurface::listenCrt: return "ListenCRT";
    case TelemetrySurface::daw: return "DAW";
    case TelemetrySurface::artwork: return "Artwork";
    case TelemetrySurface::album: return "Album";
    case TelemetrySurface::albumAnalysis: return "AlbumAnalysis";
    case TelemetrySurface::graphics: return "Graphics";
    case TelemetrySurface::display: return "Display";
    default: return "Unknown";
    }
}

std::uint64_t PerformanceTelemetry::qpcNow() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart > 0
        ? static_cast<std::uint64_t>(value.QuadPart)
        : 0;
}

void PerformanceTelemetry::wake(
    TelemetrySurface surface,
    std::uint64_t backlog) noexcept
{
    auto& value = counters_[index(surface)];
    value.wakes.fetch_add(1, std::memory_order_relaxed);
    value.backlogFrames.fetch_add(backlog, std::memory_order_relaxed);
    event(nekorox::PerformanceEventKindV2::wake, surface, backlog);
    if (TraceLoggingProviderEnabled(g_godenPerformanceProvider, 0, 0)) {
        TraceLoggingWrite(
            g_godenPerformanceProvider, "Wake",
            TraceLoggingString(name(surface), "Surface"),
            TraceLoggingUInt64(backlog, "Backlog"));
    }
}

void PerformanceTelemetry::analysis(TelemetrySurface surface) noexcept
{
    counters_[index(surface)].analysisFrames.fetch_add(
        1, std::memory_order_relaxed);
    event(nekorox::PerformanceEventKindV2::consume, surface);
}

void PerformanceTelemetry::paint(TelemetrySurface surface) noexcept
{
    counters_[index(surface)].paints.fetch_add(1, std::memory_order_relaxed);
    event(nekorox::PerformanceEventKindV2::paint, surface);
}

void PerformanceTelemetry::upload(
    TelemetrySurface surface,
    std::uint64_t bytes) noexcept
{
    counters_[index(surface)].uploadBytes.fetch_add(
        bytes, std::memory_order_relaxed);
}

void PerformanceTelemetry::endDraw(
    TelemetrySurface surface,
    long result) noexcept
{
    counters_[index(surface)].endDraws.fetch_add(1, std::memory_order_relaxed);
    event(nekorox::PerformanceEventKindV2::endDraw, surface,
        static_cast<std::uint32_t>(result));
    if (TraceLoggingProviderEnabled(g_godenPerformanceProvider, 0, 0)) {
        TraceLoggingWrite(
            g_godenPerformanceProvider, "EndDraw",
            TraceLoggingString(name(surface), "Surface"),
            TraceLoggingHexInt32(result, "Result"));
    }
}

void PerformanceTelemetry::present(
    TelemetrySurface surface,
    long result,
    bool refused) noexcept
{
    auto& value = counters_[index(surface)];
    value.presents.fetch_add(1, std::memory_order_relaxed);
    if (refused) {
        value.refusedFrames.fetch_add(1, std::memory_order_relaxed);
    }
    const auto acceptedVisible = !refused && SUCCEEDED(result);
    if (TraceLoggingProviderEnabled(g_godenPerformanceProvider, 0, 0)) {
        TraceLoggingWrite(
            g_godenPerformanceProvider, "Present",
            TraceLoggingString(name(surface), "Surface"),
            TraceLoggingHexInt32(result, "Result"),
            TraceLoggingBool(refused, "Refused"),
            TraceLoggingUInt64(0, "InputToPresentUs"));
    }
    event(nekorox::PerformanceEventKindV2::present, surface,
        static_cast<std::uint32_t>(result), refused ? 1U : 0U,
        0, acceptedVisible ? 1U : 0U);
    if (acceptedVisible) {
        event(nekorox::PerformanceEventKindV2::acceptedVisible,
            surface, static_cast<std::uint32_t>(result));
    }
}

void PerformanceTelemetry::allocation(TelemetrySurface surface) noexcept
{
    counters_[index(surface)].allocations.fetch_add(
        1, std::memory_order_relaxed);
}

void PerformanceTelemetry::input(TelemetrySurface surface) noexcept
{
    auto& value = counters_[index(surface)];
    const auto now = qpcNow();
    value.newestReceivedInputQpc.store(now, std::memory_order_release);
    std::uint64_t empty = 0;
    value.oldestPendingInputQpc.compare_exchange_strong(
        empty, now, std::memory_order_release, std::memory_order_relaxed);
    event(nekorox::PerformanceEventKindV2::input, surface, now);
}

void PerformanceTelemetry::inputIncorporated(
    TelemetrySurface surface) noexcept
{
    auto& value = counters_[index(surface)];
    const auto incorporated = value.newestReceivedInputQpc.load(
        std::memory_order_acquire);
    value.newestIncorporatedInputQpc.store(
        incorporated, std::memory_order_release);
    event(nekorox::PerformanceEventKindV2::inputIncorporated,
        surface, incorporated,
        value.oldestPendingInputQpc.load(std::memory_order_acquire));
}

void PerformanceTelemetry::frameAttempt(TelemetrySurface surface) noexcept
{
    event(nekorox::PerformanceEventKindV2::frameAttempt, surface);
}

void PerformanceTelemetry::frameResult(
    TelemetrySurface surface, long result, bool acceptedVisible) noexcept
{
    event(nekorox::PerformanceEventKindV2::frameResult, surface,
        static_cast<std::uint32_t>(result), acceptedVisible ? 1U : 0U);
}

void PerformanceTelemetry::displayed(
    TelemetrySurface surface, std::uint64_t frameId) noexcept
{
    auto& value = counters_[index(surface)];
    const auto now = qpcNow();
    auto inputQpc = value.oldestPendingInputQpc.load(std::memory_order_acquire);
    const auto incorporated = value.newestIncorporatedInputQpc.load(
        std::memory_order_acquire);
    std::uint64_t latencyMicroseconds = 0;
    if (inputQpc != 0 && incorporated >= inputQpc &&
        value.oldestPendingInputQpc.compare_exchange_strong(
            inputQpc, 0, std::memory_order_acq_rel) && now >= inputQpc) {
        latencyMicroseconds = (now - inputQpc) * 1000000ULL /
            std::max<std::uint64_t>(1, qpcFrequency_);
        value.inputSamples.fetch_add(1, std::memory_order_relaxed);
        value.inputTotalMicroseconds.fetch_add(
            latencyMicroseconds, std::memory_order_relaxed);
        auto maximum = value.inputMaximumMicroseconds.load(
            std::memory_order_relaxed);
        while (latencyMicroseconds > maximum &&
            !value.inputMaximumMicroseconds.compare_exchange_weak(
                maximum, latencyMicroseconds, std::memory_order_relaxed)) {
        }
        const auto newest = value.newestReceivedInputQpc.load(
            std::memory_order_acquire);
        if (newest > incorporated) {
            std::uint64_t empty = 0;
            value.oldestPendingInputQpc.compare_exchange_strong(
                empty, newest, std::memory_order_release,
                std::memory_order_relaxed);
        }
    }
    event(nekorox::PerformanceEventKindV2::displayed,
        surface, frameId, latencyMicroseconds);
}

void PerformanceTelemetry::readinessNotReady(
    TelemetrySurface surface, std::uint64_t epoch) noexcept
{
    event(nekorox::PerformanceEventKindV2::readinessNotReady,
        surface, epoch);
}

void PerformanceTelemetry::waitArmed(
    TelemetrySurface surface, std::uint64_t epoch) noexcept
{
    event(nekorox::PerformanceEventKindV2::waitArmed, surface, epoch);
}

void PerformanceTelemetry::readySignaled(
    TelemetrySurface surface, std::uint64_t epoch) noexcept
{
    event(nekorox::PerformanceEventKindV2::readySignaled, surface, epoch);
}

std::uint64_t PerformanceTelemetry::beginSpan(
    TelemetrySurface surface,
    std::uint64_t phase,
    std::uint64_t detail) noexcept
{
    if (eventRing_ == nullptr) return 0;
    const auto started = qpcNow();
    event(nekorox::PerformanceEventKindV2::spanStart,
        surface, phase, detail, started);
    return started;
}

void PerformanceTelemetry::endSpan(
    TelemetrySurface surface,
    std::uint64_t phase,
    std::uint64_t startedQpc,
    std::uint64_t detail) noexcept
{
    if (eventRing_ == nullptr || startedQpc == 0) return;
    const auto finished = qpcNow();
    event(nekorox::PerformanceEventKindV2::spanEnd,
        surface, phase, detail, startedQpc, finished,
        finished >= startedQpc ? finished - startedQpc : 0);
}

void PerformanceTelemetry::cacheReason(
    TelemetrySurface surface,
    std::uint64_t reason,
    std::uint64_t detail) noexcept
{
    event(nekorox::PerformanceEventKindV2::cacheReason,
        surface, reason, detail);
}

void PerformanceTelemetry::collectionMaterialized(
    TelemetrySurface surface,
    std::uint64_t collection,
    std::uint64_t count) noexcept
{
    event(nekorox::PerformanceEventKindV2::collectionMaterialization,
        surface, collection, count);
}

void PerformanceTelemetry::event(
    nekorox::PerformanceEventKindV2 kind,
    TelemetrySurface surface,
    std::uint64_t argument0,
    std::uint64_t argument1,
    std::uint64_t argument2,
    std::uint64_t argument3,
    std::uint64_t durationQpc) noexcept
{
    if (eventRing_ == nullptr) return;
    auto* latest = reinterpret_cast<volatile LONG64*>(
        &eventRing_->header.latestSequence);
    const auto sequence = static_cast<std::uint64_t>(
        InterlockedIncrement64(latest));
    auto& record = eventRing_->records[
        sequence % nekorox::kPerformanceEventCapacityV2];
    auto* committed = reinterpret_cast<volatile LONG64*>(&record.sequence);
    InterlockedExchange64(committed, 0);
    record.qpc = qpcNow();
    record.threadId = GetCurrentThreadId();
    record.kind = kind;
    record.surface = static_cast<std::uint16_t>(surface);
    record.argument0 = argument0;
    record.argument1 = argument1;
    record.argument2 = argument2;
    record.argument3 = argument3;
    record.durationQpc = durationQpc;
    if (kind == nekorox::PerformanceEventKindV2::heartbeat) {
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(
            &eventRing_->header.heartbeatQpc),
            static_cast<LONG64>(record.qpc));
    }
    MemoryBarrier();
    InterlockedExchange64(committed, static_cast<LONG64>(sequence));
}

LRESULT CALLBACK PerformanceTelemetry::heartbeatWindowProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
    PerformanceTelemetry* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<PerformanceTelemetry*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<PerformanceTelemetry*>(GetWindowLongPtrW(
            window, GWLP_USERDATA));
    }
    if (message == kHeartbeatMessage && self != nullptr) {
        self->heartbeatPostPending_.store(false, std::memory_order_release);
        self->event(nekorox::PerformanceEventKindV2::heartbeat,
            TelemetrySurface::transport);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

long PerformanceTelemetry::startUiHeartbeat() noexcept
{
    if (eventRing_ == nullptr) return E_ACCESSDENIED;
    if (heartbeatWindow_ != nullptr) return S_FALSE;
    constexpr wchar_t className[] = L"GodenQaHeartbeatV2";
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = &PerformanceTelemetry::heartbeatWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = className;
    RegisterClassExW(&windowClass);
    const auto window = CreateWindowExW(0, className, L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, windowClass.hInstance, this);
    if (window == nullptr) return HRESULT_FROM_WIN32(GetLastError());
    heartbeatWindow_ = window;
    heartbeatPostPending_.store(false, std::memory_order_release);
    try {
        heartbeatThread_ = std::jthread([this, window](std::stop_token token) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            while (!token.stop_requested()) {
                Sleep(50U);
                if (token.stop_requested()) break;
                bool expected = false;
                if (!heartbeatPostPending_.compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel)) {
                    continue;
                }
                if (!PostMessageW(window, kHeartbeatMessage, 0, 0)) {
                    heartbeatPostPending_.store(
                        false, std::memory_order_release);
                }
            }
        });
    } catch (...) {
        DestroyWindow(window);
        heartbeatWindow_ = nullptr;
        return E_OUTOFMEMORY;
    }
    event(nekorox::PerformanceEventKindV2::heartbeat,
        TelemetrySurface::transport);
    return S_OK;
}

void PerformanceTelemetry::stopUiHeartbeat() noexcept
{
    const auto window = static_cast<HWND>(heartbeatWindow_);
    if (window == nullptr) return;
    if (heartbeatThread_.joinable()) {
        heartbeatThread_.request_stop();
        heartbeatThread_.join();
    }
    heartbeatPostPending_.store(false, std::memory_order_release);
    DestroyWindow(window);
    heartbeatWindow_ = nullptr;
}

SurfaceTelemetrySnapshot PerformanceTelemetry::snapshot(
    TelemetrySurface surface) const noexcept
{
    const auto& value = counters_[index(surface)];
    SurfaceTelemetrySnapshot result;
    result.wakes = value.wakes.load(std::memory_order_relaxed);
    result.analysisFrames = value.analysisFrames.load(std::memory_order_relaxed);
    result.paints = value.paints.load(std::memory_order_relaxed);
    result.uploadBytes = value.uploadBytes.load(std::memory_order_relaxed);
    result.endDraws = value.endDraws.load(std::memory_order_relaxed);
    result.presents = value.presents.load(std::memory_order_relaxed);
    result.refusedFrames = value.refusedFrames.load(std::memory_order_relaxed);
    result.backlogFrames = value.backlogFrames.load(std::memory_order_relaxed);
    result.allocations = value.allocations.load(std::memory_order_relaxed);
    result.inputToPresentSamples = value.inputSamples.load(
        std::memory_order_relaxed);
    result.inputToPresentTotalMicroseconds = value.inputTotalMicroseconds.load(
        std::memory_order_relaxed);
    result.inputToPresentMaximumMicroseconds = value.inputMaximumMicroseconds.load(
        std::memory_order_relaxed);
    return result;
}

} // namespace nekorox::component
