#include "qa_hooks.h"
#include "display_environment.h"
#include "telemetry_span.h"

#include "frame_clock.h"
#include "render_device_hub.h"
#include "motion_policy.h"
#include "monitor_text_rendering.h"

#include <dwmapi.h>
#include <wtsapi32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <vector>

namespace nekorox::component {
namespace {

constexpr wchar_t kEnvironmentWindowClass[] =
    L"GODENplayer.DisplayEnvironment.v1";
std::atomic<DisplayEnvironmentService*> g_activeService{nullptr};

UINT presentationVisibilityMessage() noexcept
{
    static const auto message = RegisterWindowMessageW(
        L"Goden.RootPresentationVisibility.V1");
    return message;
}

bool qaHooksAllowed() noexcept
{
    return qa::hooksAllowed();
}

std::uint32_t qaUnsigned(
    const wchar_t* name, std::uint32_t fallback,
    std::uint32_t minimum, std::uint32_t maximum) noexcept
{
    if (!qaHooksAllowed()) return fallback;
    wchar_t text[32]{};
    const auto length = GetEnvironmentVariableW(
        name, text, static_cast<DWORD>(std::size(text)));
    if (length == 0 || length >= std::size(text)) return fallback;
    wchar_t* end = nullptr;
    const auto parsed = std::wcstoul(text, &end, 10);
    if (end == text || *end != L'\0') return fallback;
    return std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(parsed), minimum, maximum);
}

void queryActivePath(
    const wchar_t* gdiDevice,
    LUID& adapter,
    std::uint32_t& numerator,
    std::uint32_t& denominator,
    std::atomic<std::uint64_t>& queryCounter) noexcept
{
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(
            QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS ||
        pathCount == 0) return;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    queryCounter.fetch_add(1, std::memory_order_relaxed);
    if (QueryDisplayConfig(
            QDC_ONLY_ACTIVE_PATHS,
            &pathCount,
            paths.data(),
            &modeCount,
            modes.data(),
            nullptr) != ERROR_SUCCESS) return;
    paths.resize(pathCount);
    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            _wcsicmp(source.viewGdiDeviceName, gdiDevice) != 0) continue;
        adapter = path.targetInfo.adapterId;
        if (path.targetInfo.refreshRate.Numerator != 0 &&
            path.targetInfo.refreshRate.Denominator != 0) {
            numerator = path.targetInfo.refreshRate.Numerator;
            denominator = path.targetInfo.refreshRate.Denominator;
        }
        return;
    }
}

} // namespace

double DisplayEnvironmentSnapshotV1::refreshHz() const noexcept
{
    return static_cast<double>(refreshNumerator) /
        static_cast<double>((std::max)(1U, refreshDenominator));
}

std::uint32_t DisplayEnvironmentSnapshotV1::refreshMilliHz() const noexcept
{
    return static_cast<std::uint32_t>(std::clamp(
        std::llround(refreshHz() * 1000.0), 1LL, 1000000LL));
}

DisplayEnvironmentService& DisplayEnvironmentService::instance() noexcept
{
    static DisplayEnvironmentService service;
    return service;
}

void DisplayEnvironmentService::start() noexcept
{
    std::lock_guard lock(lifecycleMutex_);
    if (started_.exchange(true, std::memory_order_acq_rel)) return;
    g_activeService.store(this, std::memory_order_release);

    WNDCLASSEXW descriptor{sizeof(descriptor)};
    descriptor.lpfnWndProc = &DisplayEnvironmentService::windowProc;
    descriptor.hInstance = GetModuleHandleW(nullptr);
    descriptor.lpszClassName = kEnvironmentWindowClass;
    RegisterClassExW(&descriptor);
    messageWindow_ = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kEnvironmentWindowClass,
        L"",
        WS_POPUP,
        0, 0, 0, 0,
        nullptr, nullptr, descriptor.hInstance, this);
    if (messageWindow_ != nullptr)
        WTSRegisterSessionNotification(messageWindow_, NOTIFY_FOR_THIS_SESSION);

    locationHook_ = SetWinEventHook(
        EVENT_OBJECT_LOCATIONCHANGE,
        EVENT_OBJECT_LOCATIONCHANGE,
        nullptr,
        &DisplayEnvironmentService::locationHook,
        GetCurrentProcessId(),
        0,
        WINEVENT_OUTOFCONTEXT);
    visibilityHook_ = SetWinEventHook(
        EVENT_OBJECT_CLOAKED,
        EVENT_OBJECT_UNCLOAKED,
        nullptr,
        &DisplayEnvironmentService::locationHook,
        GetCurrentProcessId(),
        0,
        WINEVENT_OUTOFCONTEXT);
}

void DisplayEnvironmentService::stop() noexcept
{
    std::lock_guard lock(lifecycleMutex_);
    if (!started_.exchange(false, std::memory_order_acq_rel)) return;
    g_activeService.store(nullptr, std::memory_order_release);
    if (locationHook_ != nullptr) {
        UnhookWinEvent(locationHook_);
        locationHook_ = nullptr;
    }
    if (visibilityHook_ != nullptr) {
        UnhookWinEvent(visibilityHook_);
        visibilityHook_ = nullptr;
    }
    {
        std::lock_guard identityLock(identityMutex_);
        identities_.clear();
        displayPathCache_.clear();
    }
    if (messageWindow_ != nullptr) {
        WTSUnRegisterSessionNotification(messageWindow_);
        DestroyWindow(messageWindow_);
        messageWindow_ = nullptr;
    }
}

DisplayEnvironmentSnapshotV1 DisplayEnvironmentService::snapshot(
    HWND window) noexcept
{
    DisplayEnvironmentSnapshotV1 result;
    result.topologyGeneration = topologyGeneration();
    result.suspended = suspended_.load(std::memory_order_acquire);
    result.remoteSession = GetSystemMetrics(SM_REMOTESESSION) != 0;
    result.visible = window != nullptr && IsWindowVisible(window) != FALSE;
    result.minimized = window != nullptr &&
        IsIconic(GetAncestor(window, GA_ROOT)) != FALSE;
    DWORD cloaked = 0;
    if (window != nullptr) {
        const auto root = GetAncestor(window, GA_ROOT);
        if (root != nullptr && SUCCEEDED(DwmGetWindowAttribute(
                root, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
            result.occluded = cloaked != 0;
    }

    result.dpiX = result.dpiY = window != nullptr
        ? (std::max)(1U, GetDpiForWindow(window)) : 96U;
    result.monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (window != nullptr) {
        auto root = GetAncestor(window, GA_ROOT);
        if (root == nullptr) root = window;
        std::lock_guard identityLock(identityMutex_);
        const auto found = std::find_if(
            identities_.begin(), identities_.end(),
            [root](const WindowIdentity& identity) {
                return identity.root == root;
            });
        if (found != identities_.end()) {
            result.windowPresentationRevision =
                found->presentationRevision;
            result.rootLifetimeToken = found->lifetimeToken;
        }
    }
    bool cached = false;
    if (result.monitor != nullptr) {
        std::lock_guard cacheLock(identityMutex_);
        const auto found = std::find_if(displayPathCache_.begin(),
            displayPathCache_.end(), [&](const DisplayPathCache& entry) {
                return entry.monitor == result.monitor &&
                    entry.topologyGeneration == result.topologyGeneration;
            });
        if (found != displayPathCache_.end()) {
            result.adapterLuid = found->adapterLuid;
            result.refreshNumerator = found->refreshNumerator;
            result.refreshDenominator = found->refreshDenominator;
            cached = true;
        }
    }
    if (!cached && result.monitor != nullptr) {
        TelemetrySpan span(TelemetrySurface::display,300,result.topologyGeneration);
        MONITORINFOEXW monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (GetMonitorInfoW(result.monitor, &monitorInfo)) {
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsExW(
                    monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0) &&
                mode.dmDisplayFrequency >= 24U &&
                mode.dmDisplayFrequency <= 1000U) {
                result.refreshNumerator = mode.dmDisplayFrequency;
                result.refreshDenominator = 1;
            }
            queryActivePath(
                monitorInfo.szDevice,
                result.adapterLuid,
                result.refreshNumerator,
                result.refreshDenominator,
                displayConfigQueries_);
            std::lock_guard cacheLock(identityMutex_);
            displayPathCache_.erase(std::remove_if(
                displayPathCache_.begin(), displayPathCache_.end(),
                [&](const DisplayPathCache& entry) {
                    return entry.monitor == result.monitor;
                }), displayPathCache_.end());
            displayPathCache_.push_back({
                result.monitor,
                result.topologyGeneration,
                result.adapterLuid,
                result.refreshNumerator,
                result.refreshDenominator,
            });
        }
    }

    result.dpiX = qaUnsigned(
        L"NEKOROX_QA_DISPLAY_DPI_X", result.dpiX, 48U, 960U);
    result.dpiY = qaUnsigned(
        L"NEKOROX_QA_DISPLAY_DPI_Y", result.dpiY, 48U, 960U);
    result.refreshNumerator = qaUnsigned(
        L"NEKOROX_QA_DISPLAY_REFRESH_NUMERATOR",
        result.refreshNumerator, 1U, 1000000U);
    result.refreshDenominator = qaUnsigned(
        L"NEKOROX_QA_DISPLAY_REFRESH_DENOMINATOR",
        result.refreshDenominator, 1U, 1000000U);
    return result;
}

std::uint64_t DisplayEnvironmentService::topologyGeneration() const noexcept
{
    return topologyGeneration_.load(std::memory_order_acquire);
}

WindowPresentationIdentityV1 DisplayEnvironmentService::windowIdentity(
    HWND window) const noexcept
{
    WindowPresentationIdentityV1 result;
    result.topologyGeneration = topologyGeneration();
    if (window == nullptr) return result;
    auto root = GetAncestor(window, GA_ROOT);
    if (root == nullptr) root = window;
    std::lock_guard identityLock(identityMutex_);
    const auto found = std::find_if(
        identities_.begin(), identities_.end(),
        [root](const WindowIdentity& identity) {
            return identity.root == root;
        });
    if (found != identities_.end()) {
        result.windowPresentationRevision = found->presentationRevision;
        result.rootLifetimeToken = found->lifetimeToken;
    }
    return result;
}

bool DisplayEnvironmentService::suspended() const noexcept
{
    return suspended_.load(std::memory_order_acquire);
}

bool DisplayEnvironmentService::presentationVisible(HWND window) const noexcept
{
    if (window == nullptr || suspended_.load(std::memory_order_acquire)) {
        return false;
    }
    // A visible root does not imply that every retained child surface is
    // visible. PlayerShell deliberately hides Workspace while ALBUM owns the
    // centre. Treating that hidden child as presentable can consume its
    // one-shot DXGI readiness callback without ever receiving WM_PAINT,
    // stranding the next show/resize until unrelated input arrives.
    if (!IsWindowVisible(window)) return false;
    const auto root = GetAncestor(window, GA_ROOT);
    if (root == nullptr || !IsWindow(root) ||
        !IsWindowVisible(root) || IsIconic(root)) {
        return false;
    }
    DWORD cloaked = 0;
    return FAILED(DwmGetWindowAttribute(
               root, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) ||
        cloaked == 0;
}

void DisplayEnvironmentService::registerPersistentRoot(HWND window) noexcept
{
    if (window == nullptr) return;
    auto root = GetAncestor(window, GA_ROOT);
    if (root == nullptr) root = window;
    if (!IsWindow(root)) return;
    const auto monitor = MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST);
    const auto dpi = (std::max)(1U, GetDpiForWindow(root));
    std::lock_guard identityLock(identityMutex_);
    const auto found = std::find_if(
        identities_.begin(), identities_.end(),
        [root](const WindowIdentity& identity) {
            return identity.root == root;
        });
    if (found == identities_.end()) {
        identities_.push_back({root, monitor, dpi, 1,
            nextRootLifetimeToken_.fetch_add(1, std::memory_order_acq_rel)});
    }
}

void DisplayEnvironmentService::unregisterPersistentRoot(HWND window) noexcept
{
    if (window == nullptr) return;
    auto root = GetAncestor(window, GA_ROOT);
    if (root == nullptr) root = window;
    std::lock_guard identityLock(identityMutex_);
    std::erase_if(identities_, [root](const WindowIdentity& identity) {
        return identity.root == root;
    });
}

bool DisplayEnvironmentService::notifyWindowChanged(
    HWND window, bool* presentationChanged) noexcept
{
    if (presentationChanged != nullptr) *presentationChanged = false;
    // Location-change WinEvents are emitted continuously while moving a
    // window and for transient tooltips. Refresh/DPI only change when the
    // root crosses monitor or DPI domains, so keep the expensive topology
    // generation stable for all other motion.
    if (window != nullptr) {
        auto root = GetAncestor(window, GA_ROOT);
        if (root == nullptr) root = window;
        if (!IsWindow(root)) return false;
        const auto monitor = MonitorFromWindow(
            root, MONITOR_DEFAULTTONEAREST);
        const auto dpi = (std::max)(1U, GetDpiForWindow(root));
        std::lock_guard identityLock(identityMutex_);
        identities_.erase(
            std::remove_if(identities_.begin(), identities_.end(),
                [](const WindowIdentity& identity) {
                    return identity.root == nullptr ||
                        !IsWindow(identity.root);
                }),
            identities_.end());
        const auto found = std::find_if(
            identities_.begin(), identities_.end(),
            [root](const WindowIdentity& identity) {
                return identity.root == root;
            });
        if (found != identities_.end()) {
            if (found->monitor == monitor && found->dpi == dpi) return true;
            found->monitor = monitor;
            found->dpi = dpi;
            ++found->presentationRevision;
            if (presentationChanged != nullptr) *presentationChanged = true;
        } else {
            // A transient process-owned root is not a GODENplayer surface.
            // Registration is explicit at each persistent root's lifetime.
            return false;
        }
        return true;
    }
    return false;
}

LRESULT CALLBACK DisplayEnvironmentService::windowProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
    auto* service = reinterpret_cast<DisplayEnvironmentService*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        service = static_cast<DisplayEnvironmentService*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(service));
    }
    if (service != nullptr)
        service->onEnvironmentMessage(message, wParam);
    return DefWindowProcW(window, message, wParam, lParam);
}

void CALLBACK DisplayEnvironmentService::locationHook(
    HWINEVENTHOOK, DWORD event, HWND window, LONG object,
    LONG child, DWORD, DWORD) noexcept
{
    if ((event != EVENT_OBJECT_LOCATIONCHANGE &&
            event != EVENT_OBJECT_CLOAKED &&
            event != EVENT_OBJECT_UNCLOAKED) ||
        window == nullptr ||
        object != OBJID_WINDOW || child != CHILDID_SELF ||
        GetAncestor(window, GA_ROOT) != window) return;
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId()) return;
    if (auto* service = g_activeService.load(std::memory_order_acquire)) {
        if (event == EVENT_OBJECT_LOCATIONCHANGE) {
            bool changed = false;
            (void)service->notifyWindowChanged(window, &changed);
            if (changed) WindowFrameClock::environmentChanged();
        } else {
            // Cloak/uncloak does not change monitor identity, but it must
            // suppress/rearm clocks immediately rather than wait for a poll.
            if (!service->notifyWindowChanged(window)) return;
            const auto message = presentationVisibilityMessage();
            if (message != 0) {
                PostMessageW(
                    window,
                    message,
                    event == EVENT_OBJECT_UNCLOAKED ? 1 : 0,
                    0);
            }
            WindowFrameClock::environmentChanged();
        }
    }
}

void DisplayEnvironmentService::onEnvironmentMessage(
    UINT message, WPARAM wParam) noexcept
{
    switch (message) {
    case WM_DISPLAYCHANGE:
    case WM_DEVICECHANGE: {
        MonitorTextRendering::settingsChanged();
        const auto generation =
            topologyGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
        {
            std::lock_guard cacheLock(identityMutex_);
            displayPathCache_.clear();
        }
        RenderDeviceHub::instance().notifyTopologyChanged(generation);
        WindowFrameClock::environmentChanged();
        break;
    }
    case WM_SETTINGCHANGE:
        MonitorTextRendering::settingsChanged();
        refreshSystemMotionPreference();
        WindowFrameClock::environmentChanged();
        break;
    case WM_WTSSESSION_CHANGE:
        WindowFrameClock::environmentChanged();
        break;
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMSUSPEND)
            suspended_.store(true, std::memory_order_release);
        else if (wParam == PBT_APMRESUMEAUTOMATIC ||
                 wParam == PBT_APMRESUMESUSPEND ||
                 wParam == PBT_APMRESUMECRITICAL)
            suspended_.store(false, std::memory_order_release);
        // Waitable timers must not keep presentation clocks logically active
        // across suspend.  On resume all deadlines are rebased, so an hour of
        // sleep cannot be interpreted as millions of missed frames.
        WindowFrameClock::environmentChanged();
        break;
    default:
        break;
    }
}

} // namespace nekorox::component
