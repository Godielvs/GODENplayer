#include "chrome.h"

#include "display_environment.h"
#include "file_open_service.h"
#include "frameless_window.h"
#include "keep_playing_service.h"
#include "sdk_compat.h"
#include "theme_service.h"
#include "ui_design_system.h"
#include "workspace_notifications.h"

#include "nekorox/public_branding.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace nekorox::component {
namespace {

constexpr UINT_PTR kSubclassId = 0x4E4B5258U;
constexpr UINT_PTR kDropSubclassId = 0x4E4B4452U;
constexpr UINT kSwpStateChanged = 0x8000U;
bool g_chromeActive = true;
bool g_initialRelayoutPending = false;
bool g_initialRelayoutPosted = false;
std::uint64_t g_initialRelayoutEpoch = 1;
bool g_interactiveGeometryChange = false;
bool g_interactiveGeometryStarted = false;
bool g_interactiveGeometryLatestValid = false;
bool g_interactiveGeometryCanceled = false;
bool g_restoringCommittedGeometry = false;
bool g_committedGeometryValid = false;
unsigned g_committedGeometryCorrectionsRemaining = 0;
RECT g_interactiveGeometryStart{};
RECT g_interactiveGeometryLatest{};
RECT g_committedGeometry{};
ULONGLONG g_geometryCommitDeadline = 0;
bool g_rootPresentationStateKnown = false;
bool g_rootPresentationVisible = false;
HMONITOR g_rootDisplayMonitor = nullptr;

bool sameGeometry(const RECT& left, const RECT& right, int tolerance = 2) noexcept
{
    return std::abs(left.left - right.left) <= tolerance &&
        std::abs(left.top - right.top) <= tolerance &&
        std::abs(left.right - right.right) <= tolerance &&
        std::abs(left.bottom - right.bottom) <= tolerance;
}

void clearCommittedGeometry() noexcept
{
    g_committedGeometryValid = false;
    g_committedGeometryCorrectionsRemaining = 0;
    g_geometryCommitDeadline = 0;
}

void armCommittedGeometry(const RECT& geometry) noexcept
{
    g_committedGeometry = geometry;
    g_committedGeometryValid = true;
    g_committedGeometryCorrectionsRemaining = 8;
    g_geometryCommitDeadline = GetTickCount64() + 5000;
}

bool committedGeometryExpired() noexcept
{
    if (!g_committedGeometryValid) return true;
    if (g_committedGeometryCorrectionsRemaining != 0 &&
            GetTickCount64() <= g_geometryCommitDeadline) {
        return false;
    }
    clearCommittedGeometry();
    return true;
}

void beginInteractiveGeometry(HWND window) noexcept
{
    g_interactiveGeometryStarted = window != nullptr &&
        GetWindowRect(window, &g_interactiveGeometryStart) != FALSE;
    g_interactiveGeometryLatestValid = g_interactiveGeometryStarted;
    if (g_interactiveGeometryLatestValid) {
        g_interactiveGeometryLatest = g_interactiveGeometryStart;
    }
    g_interactiveGeometryCanceled = false;
    clearCommittedGeometry();
}

void resetInteractiveGeometry() noexcept
{
    g_interactiveGeometryStarted = false;
    g_interactiveGeometryLatestValid = false;
    g_interactiveGeometryCanceled = false;
}

bool geometryFromWindowPosition(
    HWND window,
    const WINDOWPOS& position,
    RECT& proposed) noexcept
{
    RECT current{};
    if (window == nullptr || GetWindowRect(window, &current) == FALSE) {
        return false;
    }

    proposed = current;
    if ((position.flags & SWP_NOMOVE) == 0) {
        proposed.left = position.x;
        proposed.top = position.y;
    }
    const auto width = (position.flags & SWP_NOSIZE) == 0
        ? position.cx
        : current.right - current.left;
    const auto height = (position.flags & SWP_NOSIZE) == 0
        ? position.cy
        : current.bottom - current.top;
    proposed.right = proposed.left + width;
    proposed.bottom = proposed.top + height;
    return true;
}

void rememberInteractiveGeometry(const RECT* proposed) noexcept
{
    if (!g_interactiveGeometryChange || !g_interactiveGeometryStarted ||
            g_interactiveGeometryCanceled || proposed == nullptr) {
        return;
    }
    g_interactiveGeometryLatest = *proposed;
    g_interactiveGeometryLatestValid = true;
}

void preserveInteractiveGeometry(
    HWND window,
    const WINDOWPOS& incoming,
    WINDOWPOS* finalPosition) noexcept
{
    if (!g_interactiveGeometryChange || !g_interactiveGeometryStarted ||
            g_interactiveGeometryCanceled || window == nullptr ||
            finalPosition == nullptr || IsIconic(window) || IsZoomed(window) ||
            (finalPosition->flags & kSwpStateChanged) != 0) {
        return;
    }

    RECT incomingGeometry{};
    RECT finalGeometry{};
    if (!geometryFromWindowPosition(window, incoming, incomingGeometry) ||
            !geometryFromWindowPosition(
                window, *finalPosition, finalGeometry)) {
        return;
    }

    // During the modal move/size loop Columns UI can replay the rectangle that
    // was persisted before the drag. Keep the last WM_SIZING/WM_MOVING proposal
    // authoritative when downstream code changes a newer proposal back to the
    // exact pre-drag rectangle. If the user genuinely drags back, the latest
    // modal rectangle is also the start rectangle and this guard stays idle.
    RECT authoritative = incomingGeometry;
    if (sameGeometry(authoritative, g_interactiveGeometryStart) &&
            g_interactiveGeometryLatestValid) {
        authoritative = g_interactiveGeometryLatest;
    }
    if (sameGeometry(authoritative, g_interactiveGeometryStart) ||
            !sameGeometry(finalGeometry, g_interactiveGeometryStart)) {
        return;
    }

    finalPosition->x = authoritative.left;
    finalPosition->y = authoritative.top;
    finalPosition->cx = authoritative.right - authoritative.left;
    finalPosition->cy = authoritative.bottom - authoritative.top;
    finalPosition->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
    g_interactiveGeometryLatest = authoritative;
    g_interactiveGeometryLatestValid = true;
}

void commitInteractiveGeometry(HWND window) noexcept
{
    RECT bounds{};
    if (!g_interactiveGeometryStarted || g_interactiveGeometryCanceled ||
            window == nullptr || IsIconic(window) || IsZoomed(window) ||
            GetWindowRect(window, &bounds) == FALSE) {
        resetInteractiveGeometry();
        clearCommittedGeometry();
        return;
    }

    RECT intended = bounds;
    if (sameGeometry(bounds, g_interactiveGeometryStart)) {
        if (!g_interactiveGeometryLatestValid ||
                sameGeometry(
                    g_interactiveGeometryLatest,
                    g_interactiveGeometryStart)) {
            resetInteractiveGeometry();
            clearCommittedGeometry();
            return;
        }
        intended = g_interactiveGeometryLatest;
    }

    // Arm the committed transaction before reapplying it. A host may react
    // synchronously to SetWindowPos (including with SWP_NOSENDCHANGING); the
    // nested WM_WINDOWPOSCHANGED path must already know which rectangle wins.
    armCommittedGeometry(intended);
    resetInteractiveGeometry();

    if (!sameGeometry(bounds, intended)) {
        SetWindowPos(window, nullptr,
            intended.left,
            intended.top,
            intended.right - intended.left,
            intended.bottom - intended.top,
            SWP_NOACTIVATE | SWP_NOZORDER);
    }
    if (!g_committedGeometryValid) {
        resetInteractiveGeometry();
        clearCommittedGeometry();
    }
}

void preserveCommittedGeometry(HWND window, WINDOWPOS* position) noexcept
{
    if (committedGeometryExpired() || window == nullptr || position == nullptr ||
            g_interactiveGeometryChange || IsIconic(window) || IsZoomed(window) ||
            (position->flags & kSwpStateChanged) != 0) {
        return;
    }

    RECT current{};
    if (GetWindowRect(window, &current) == FALSE) return;
    RECT proposed{};
    if (!geometryFromWindowPosition(window, *position, proposed)) return;

    // Block only the characteristic rollback: the committed user rectangle is
    // still current and a later host pass proposes the exact pre-drag bounds.
    if (!sameGeometry(current, g_committedGeometry) ||
            !sameGeometry(proposed, g_interactiveGeometryStart)) {
        return;
    }

    position->x = g_committedGeometry.left;
    position->y = g_committedGeometry.top;
    position->cx = g_committedGeometry.right - g_committedGeometry.left;
    position->cy = g_committedGeometry.bottom - g_committedGeometry.top;
    position->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
    --g_committedGeometryCorrectionsRemaining;
}

void restoreCommittedGeometryAfterChange(
    HWND window,
    const WINDOWPOS* position) noexcept
{
    if (committedGeometryExpired() || g_interactiveGeometryChange ||
            g_restoringCommittedGeometry ||
            window == nullptr || position == nullptr || IsIconic(window) ||
            IsZoomed(window) ||
            (position->flags & kSwpStateChanged) != 0) {
        return;
    }

    RECT current{};
    if (GetWindowRect(window, &current) == FALSE ||
            !sameGeometry(current, g_interactiveGeometryStart) ||
            sameGeometry(current, g_committedGeometry)) {
        return;
    }

    // SWP_NOSENDCHANGING lets a host bypass WM_WINDOWPOSCHANGING entirely.
    // Reassert the completed user transaction after downstream processing as
    // a final backstop. The recursive WM_WINDOWPOSCHANGED observes the already
    // committed rectangle and therefore terminates immediately.
    g_restoringCommittedGeometry = true;
    SetWindowPos(window, nullptr,
        g_committedGeometry.left,
        g_committedGeometry.top,
        g_committedGeometry.right - g_committedGeometry.left,
        g_committedGeometry.bottom - g_committedGeometry.top,
        SWP_NOACTIVATE | SWP_NOZORDER);
    g_restoringCommittedGeometry = false;
    if (g_committedGeometryCorrectionsRemaining != 0) {
        --g_committedGeometryCorrectionsRemaining;
    }
}

UINT rootPresentationVisibilityMessage() noexcept
{
    static const auto message = RegisterWindowMessageW(
        L"Goden.RootPresentationVisibility.V1");
    return message;
}

BOOL CALLBACK notifyRootPresentationChild(HWND child, LPARAM visible)
{
    const auto message = rootPresentationVisibilityMessage();
    if (message != 0) {
        PostMessageW(
            child,
            message,
            static_cast<WPARAM>(visible != 0),
            0);
    }
    return TRUE;
}

void publishRootPresentationVisibility(
    HWND window,
    bool visible,
    bool force = false) noexcept
{
    if (window == nullptr || !IsWindow(window)) return;
    if (!force && g_rootPresentationStateKnown &&
        g_rootPresentationVisible == visible) {
        return;
    }
    g_rootPresentationStateKnown = true;
    g_rootPresentationVisible = visible;

    EnumChildWindows(
        window,
        notifyRootPresentationChild,
        visible ? 1 : 0);

    // Native child windows do not receive WM_SHOWWINDOW/WM_SIZE when only
    // their root is minimized. Reuse their semantic refresh path so clocks,
    // realtime cursors and retained layers stop and restart immediately.
    notifyNativePanelsPlaybackChanged();
    notifyWorkspacePlaybackChanged(true);
}

UINT displayEnvironmentChangedMessage() noexcept
{
    static const auto message = RegisterWindowMessageW(
        L"Goden.DisplayEnvironmentChanged.V1");
    return message;
}

BOOL CALLBACK notifyDisplayEnvironmentChild(HWND child, LPARAM)
{
    const auto message = displayEnvironmentChangedMessage();
    if (message != 0) PostMessageW(child, message, 0, 0);
    return TRUE;
}

void publishRootDisplayEnvironment(HWND window, bool force = false) noexcept
{
    if (window == nullptr || !IsWindow(window)) return;
    const auto monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (!force && monitor == g_rootDisplayMonitor) return;
    g_rootDisplayMonitor = monitor;
    EnumChildWindows(window, notifyDisplayEnvironmentChild, 0);
    // Native/Album clocks may be stopped while paused. Their semantic refresh
    // must redraw text for the new monitor even when its DPI is unchanged.
    notifyNativePanelsPlaybackChanged();
}

UINT initialRelayoutMessage() noexcept
{
    static const auto message = RegisterWindowMessageW(
        L"NekoRoX.Chrome.InitialRelayout.V1");
    return message;
}

void invalidateInitialRelayout() noexcept
{
    ++g_initialRelayoutEpoch;
    if (g_initialRelayoutEpoch == 0) ++g_initialRelayoutEpoch;
    g_initialRelayoutPending = false;
    g_initialRelayoutPosted = false;
}

void postInitialRelayout(HWND window) noexcept
{
    if (g_interactiveGeometryChange || !g_initialRelayoutPending ||
            g_initialRelayoutPosted ||
            window == nullptr || !IsWindow(window)) {
        return;
    }
    const auto message = initialRelayoutMessage();
    if (message != 0 && PostMessageW(window, message,
            static_cast<WPARAM>(g_initialRelayoutEpoch), 0) != FALSE) {
        g_initialRelayoutPosted = true;
    }
}

HWND largestVisibleDirectChild(HWND window) noexcept
{
    HWND result = nullptr;
    std::int64_t largestArea = 0;
    for (auto child = GetWindow(window, GW_CHILD);
            child != nullptr;
            child = GetWindow(child, GW_HWNDNEXT)) {
        if (!IsWindowVisible(child)) continue;
        RECT bounds{};
        if (!GetWindowRect(child, &bounds)) continue;
        const auto width = std::max(0L, bounds.right - bounds.left);
        const auto height = std::max(0L, bounds.bottom - bounds.top);
        const auto area =
            static_cast<std::int64_t>(width) *
            static_cast<std::int64_t>(height);
        if (area > largestArea) {
            largestArea = area;
            result = child;
        }
    }
    return result;
}

void fitRestoredRootToCurrentMonitor(HWND window) noexcept
{
    if (window == nullptr || IsIconic(window) || IsZoomed(window)) return;
    RECT bounds{};
    if (!GetWindowRect(window, &bounds)) return;
    const auto monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitorInfo)) return;

    const auto& work = monitorInfo.rcWork;
    const auto workWidth = static_cast<int>(
        (std::max)(1L, work.right - work.left));
    const auto workHeight = static_cast<int>(
        (std::max)(1L, work.bottom - work.top));
    const auto dpi = static_cast<int>((std::max)(1U, GetDpiForWindow(window)));
    const auto minimumWidth = (std::min)(MulDiv(720, dpi, 96), workWidth);
    const auto minimumHeight = (std::min)(MulDiv(620, dpi, 96), workHeight);
    const auto width = std::clamp(static_cast<int>(
        bounds.right - bounds.left), minimumWidth, workWidth);
    const auto height = std::clamp(static_cast<int>(
        bounds.bottom - bounds.top), minimumHeight, workHeight);
    const auto left = std::clamp(static_cast<int>(bounds.left),
        static_cast<int>(work.left), static_cast<int>(work.right) - width);
    const auto top = std::clamp(static_cast<int>(bounds.top),
        static_cast<int>(work.top), static_cast<int>(work.bottom) - height);
    if (left == bounds.left && top == bounds.top &&
            width == bounds.right - bounds.left &&
            height == bounds.bottom - bounds.top) {
        return;
    }
    SetWindowPos(window, nullptr, left, top, width, height,
        SWP_NOACTIVATE | SWP_NOZORDER);
}

void completeInitialRelayout(HWND window, std::uint64_t epoch) noexcept
{
    if (epoch != g_initialRelayoutEpoch) return;
    g_initialRelayoutPosted = false;
    if (g_interactiveGeometryChange || !g_initialRelayoutPending ||
            window == nullptr ||
            !IsWindow(window) || !IsWindowVisible(window) ||
            IsIconic(window)) {
        return;
    }

    // A copied foobar profile can contain physical-pixel bounds saved on a
    // different-DPI machine. Clamp that restored rectangle in the current
    // monitor's pixel space before Columns UI measures its children. This is
    // what prevents a 96-DPI profile from opening tiny, partially off-screen,
    // or proportionally crushed on a 150/200% 16:10 notebook.
    fitRestoredRootToCurrentMonitor(window);

    RECT bounds{};
    if (!GetWindowRect(window, &bounds)) return;
    const auto width = bounds.right - bounds.left;
    const auto height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) return;
    const auto content = largestVisibleDirectChild(window);
    if (content == nullptr) return;

    // Removing WS_CAPTION changes the client contract after Columns UI has
    // already measured its root. A same-size top-level SetWindowPos can be
    // coalesced, so synchronize the existing UI host with the recalculated
    // client contract instead of nudging the visible window by a pixel.
    g_initialRelayoutPending = false;
    SetWindowPos(window, nullptr, 0, 0, width, height,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
    RECT client{};
    if (GetClientRect(window, &client)) {
        const auto clientWidth = std::max(0L, client.right - client.left);
        const auto clientHeight = std::max(0L, client.bottom - client.top);
        if (clientWidth <= 0 || clientHeight <= 0 ||
                SetWindowPos(content, nullptr,
                    0, 0, clientWidth, clientHeight,
                    SWP_NOACTIVATE | SWP_NOZORDER) == FALSE) {
            g_initialRelayoutPending = true;
        }
    } else {
        g_initialRelayoutPending = true;
    }
    RedrawWindow(window, nullptr, nullptr,
        RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
}

COLORREF colorRef(float red, float green, float blue) noexcept
{
    const auto channel = [](float value) {
        return static_cast<BYTE>(std::lround(
            std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    return RGB(channel(red), channel(green), channel(blue));
}

COLORREF chromeBorderColor()
{
    const auto separator = UiDesignSystemV1::color(
        g_chromeActive
            ? UiColorToken::separatorStrong
            : UiColorToken::separator);
    return colorRef(
        separator.red,
        separator.green,
        separator.blue);
}

void installDropWindow(HWND window);

BOOL CALLBACK installDropChild(HWND window, LPARAM)
{
    installDropWindow(window);
    return TRUE;
}

LRESULT CALLBACK dropSubclass(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR,
    DWORD_PTR)
{
    switch (message) {
    case WM_DROPFILES:
        openDroppedFiles(window, reinterpret_cast<HDROP>(wParam));
        return 0;
    case WM_APPCOMMAND:
        if (GET_APPCOMMAND_LPARAM(lParam) ==
                APPCOMMAND_MEDIA_NEXTTRACK &&
            KeepPlayingService::instance().handleNextCommand()) {
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if (wParam == VK_MEDIA_NEXT_TRACK &&
            KeepPlayingService::instance().handleNextCommand()) {
            return 0;
        }
        break;
    case WM_PARENTNOTIFY:
        if (LOWORD(wParam) == WM_CREATE && lParam != 0) {
            const auto child = reinterpret_cast<HWND>(lParam);
            installDropWindow(child);
            EnumChildWindows(child, installDropChild, 0);
        }
        break;
    case WM_NCHITTEST: {
        // Columns UI children cover almost the entire frameless client area.
        // Let edge hits pass through the child graph so the root window can
        // return HTLEFT/HTRIGHT/etc. and start the actual window resize.
        const auto root = GetAncestor(window, GA_ROOT);
        if (root != nullptr && root != window &&
                framelessResizeHitTest(root, lParam) != HTCLIENT) {
            return HTTRANSPARENT;
        }
        break;
    }
    case WM_NCDESTROY:
        DisplayEnvironmentService::instance().unregisterPersistentRoot(window);
        DragAcceptFiles(window, FALSE);
        RemoveWindowSubclass(window, dropSubclass, kDropSubclassId);
        break;
    default:
        break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

void installDropWindow(HWND window)
{
    if (window == nullptr || !IsWindow(window)) return;
    DragAcceptFiles(window, TRUE);
    SetWindowSubclass(window, dropSubclass, kDropSubclassId, 0);
}

BOOL CALLBACK uninstallDropChild(HWND window, LPARAM)
{
    DragAcceptFiles(window, FALSE);
    RemoveWindowSubclass(window, dropSubclass, kDropSubclassId);
    return TRUE;
}

LRESULT CALLBACK chromeSubclass(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR,
    DWORD_PTR)
{
    if (message == rootPresentationVisibilityMessage()) {
        const auto visible = wParam != 0 &&
            DisplayEnvironmentService::instance().presentationVisible(window);
        publishRootPresentationVisibility(window, visible, true);
        return 0;
    }
    if (message == initialRelayoutMessage()) {
        completeInitialRelayout(window, static_cast<std::uint64_t>(wParam));
        return 0;
    }

    switch (message) {
    case WM_SETTEXT:
        if (lParam != 0) {
            const auto* source = reinterpret_cast<const wchar_t*>(lParam);
            const auto branded = nekorox::godenPublicWindowTitle(source);
            if (branded != source) {
                return DefSubclassProc(
                    window,
                    message,
                    wParam,
                    reinterpret_cast<LPARAM>(branded.c_str()));
            }
        }
        break;
    case WM_DROPFILES:
        openDroppedFiles(window, reinterpret_cast<HDROP>(wParam));
        return 0;
    case WM_PARENTNOTIFY:
        if (LOWORD(wParam) == WM_CREATE && lParam != 0) {
            const auto child = reinterpret_cast<HWND>(lParam);
            installDropWindow(child);
            EnumChildWindows(child, installDropChild, 0);
        }
        break;
    case WM_SHOWWINDOW:
        if (wParam != FALSE) postInitialRelayout(window);
        publishRootPresentationVisibility(
            window,
            wParam != FALSE && !IsIconic(window));
        break;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) postInitialRelayout(window);
        publishRootPresentationVisibility(
            window,
            wParam != SIZE_MINIMIZED && IsWindowVisible(window));
        break;
    case WM_ENTERSIZEMOVE:
        // Once the user starts an interactive move/resize, their geometry is
        // authoritative. A startup/topology relayout already queued in the
        // message loop must not replay SetWindowPos over the live operation.
        g_interactiveGeometryChange = true;
        invalidateInitialRelayout();
        beginInteractiveGeometry(window);
        break;
    case WM_SIZING:
    case WM_MOVING: {
        // Store the rectangle after every downstream constraint has run. This
        // is the authoritative live transaction even if the host later posts
        // a stale placement while Windows is still inside the modal loop.
        const auto result = DefSubclassProc(window, message, wParam, lParam);
        rememberInteractiveGeometry(reinterpret_cast<const RECT*>(lParam));
        return result;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g_interactiveGeometryChange && wParam == VK_ESCAPE) {
            g_interactiveGeometryCanceled = true;
        }
        break;
    case WM_EXITSIZEMOVE: {
        // Let foobar/Columns UI finish the modal transaction first. Committing
        // before DefSubclassProc allowed its release-time placement to win
        // after we had already sampled the expanded rectangle.
        const auto result = DefSubclassProc(window, message, wParam, lParam);
        g_interactiveGeometryChange = false;
        invalidateInitialRelayout();
        commitInteractiveGeometry(window);
        DisplayEnvironmentService::instance().notifyWindowChanged(window);
        publishRootDisplayEnvironment(window);
        return result;
    }
    case WM_WINDOWPOSCHANGING: {
        // Run downstream host/subclass logic first, then validate its final
        // proposal against both the live and committed transactions.
        const auto incoming = *reinterpret_cast<WINDOWPOS*>(lParam);
        const auto result = DefSubclassProc(window, message, wParam, lParam);
        preserveInteractiveGeometry(
            window, incoming, reinterpret_cast<WINDOWPOS*>(lParam));
        preserveCommittedGeometry(
            window, reinterpret_cast<WINDOWPOS*>(lParam));
        return result;
    }
    case WM_DPICHANGED:
        // The host owns the suggested top-level bounds; our subclass must not
        // race it. Mark topology/layout dirty so native children rebuild text,
        // hit testing and physical-pixel artwork for the new monitor.
        clearCommittedGeometry();
        DisplayEnvironmentService::instance().notifyWindowChanged();
        publishRootDisplayEnvironment(window, true);
        // Per-monitor DPI changes already carry authoritative suggested bounds.
        // Do not re-arm the one-shot startup profile repair after the user has
        // established a window rectangle.
        InvalidateRect(window, nullptr, FALSE);
        break;
    case WM_DISPLAYCHANGE:
        clearCommittedGeometry();
        DisplayEnvironmentService::instance().notifyWindowChanged();
        publishRootDisplayEnvironment(window, true);
        // Display topology invalidates the old transaction, but must not replay
        // startup profile geometry into a live session.
        break;
    case WM_DEVICECHANGE:
    case WM_SETTINGCHANGE:
        // Audio-device, theme and ordinary system-setting broadcasts do not
        // authorize changing the user's window rectangle.
        DisplayEnvironmentService::instance().notifyWindowChanged();
        publishRootDisplayEnvironment(window, true);
        break;
    case WM_SYSCOMMAND:
        switch (wParam & 0xfff0U) {
        case SC_MAXIMIZE:
        case SC_MINIMIZE:
        case SC_RESTORE:
            clearCommittedGeometry();
            break;
        default:
            break;
        }
        break;
    case WM_WINDOWPOSCHANGED: {
        // Run the host first, then defend the completed user rectangle even if
        // it used SWP_NOSENDCHANGING and skipped our earlier guard.
        const auto result = DefSubclassProc(window, message, wParam, lParam);
        restoreCommittedGeometryAfterChange(
            window, reinterpret_cast<const WINDOWPOS*>(lParam));
        DisplayEnvironmentService::instance().notifyWindowChanged(window);
        publishRootDisplayEnvironment(window);
        return result;
    }
    case WM_PAINT:
        // Columns UI may construct or restore its root after initquit::on_init.
        // The first visible paint is the earliest deterministic point where
        // the complete child graph exists, so finish the one-shot relayout
        // immediately after that frame instead of relying on a timer.
        postInitialRelayout(window);
        break;
    case WM_NCCALCSIZE:
        applyFramelessClientInsets(window, wParam, lParam);
        return 0;
    case WM_NCHITTEST: {
        const auto resize = framelessResizeHitTest(window, lParam);
        return resize != HTCLIENT ? resize : HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        applyFramelessMinMax(window, lParam);
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        // Never demand a window larger than the monitor work area (important
        // for 200% DPI on a 1080p display), but otherwise prevent Columns UI
        // from proportionally collapsing every pane into an unusable mosaic.
        applyDpiAwareMinimum(window, limits, 720, 620);
        return 0;
    }
    case WM_ACTIVATE:
        g_chromeActive = LOWORD(wParam) != WA_INACTIVE;
        drawFramelessBorder(window, chromeBorderColor());
        break;
    case WM_NCACTIVATE:
        g_chromeActive = wParam != FALSE;
        drawFramelessBorder(window, chromeBorderColor());
        return TRUE;
    case WM_NCPAINT:
        drawFramelessBorder(window, chromeBorderColor());
        return 0;
    case WM_NCDESTROY:
        invalidateInitialRelayout();
        g_interactiveGeometryChange = false;
        resetInteractiveGeometry();
        g_restoringCommittedGeometry = false;
        clearCommittedGeometry();
        DragAcceptFiles(window, FALSE);
        EnumChildWindows(window, uninstallDropChild, 0);
        RemoveWindowSubclass(window, chromeSubclass, kSubclassId);
        break;
    default:
        break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

} // namespace

ChromeController& ChromeController::instance()
{
    static ChromeController controller;
    return controller;
}

void ChromeController::start()
{
    auto window = static_cast<HWND>(core_api::get_main_window());
    if (window == nullptr || window_ != nullptr) return;
    if (SetWindowSubclass(window, chromeSubclass, kSubclassId, 0)) {
        window_ = window;
        DisplayEnvironmentService::instance().registerPersistentRoot(window);
        ++g_initialRelayoutEpoch;
        if (g_initialRelayoutEpoch == 0) ++g_initialRelayoutEpoch;
        g_initialRelayoutPending = true;
        g_initialRelayoutPosted = false;
        g_interactiveGeometryChange = false;
        resetInteractiveGeometry();
        g_restoringCommittedGeometry = false;
        clearCommittedGeometry();
        originalStyle_ = enableFramelessWindow(window);
        g_chromeActive =
            GetForegroundWindow() == window ||
            GetAncestor(GetForegroundWindow(), GA_ROOT) == window;
        DragAcceptFiles(window, TRUE);
        EnumChildWindows(window, installDropChild, 0);
        drawFramelessBorder(window, chromeBorderColor());
        postInitialRelayout(window);
        publishRootPresentationVisibility(
            window,
            IsWindowVisible(window) && !IsIconic(window),
            true);
        const auto length = GetWindowTextLengthW(window);
        if (length >= 0) {
            std::wstring title(static_cast<std::size_t>(length) + 1U, L'\0');
            const auto copied = GetWindowTextW(
                window,
                title.data(),
                static_cast<int>(title.size()));
            title.resize(static_cast<std::size_t>(std::max(0, copied)));
            const auto branded = nekorox::godenPublicWindowTitle(title);
            if (branded != title) SetWindowTextW(window, branded.c_str());
        }
    }
}

void ChromeController::stop()
{
    auto window = static_cast<HWND>(window_);
    if (window != nullptr && IsWindow(window)) {
        DisplayEnvironmentService::instance().unregisterPersistentRoot(window);
        DragAcceptFiles(window, FALSE);
        EnumChildWindows(window, uninstallDropChild, 0);
        RemoveWindowSubclass(window, chromeSubclass, kSubclassId);
        restoreWindowFrame(window, originalStyle_);
    }
    invalidateInitialRelayout();
    g_interactiveGeometryChange = false;
    resetInteractiveGeometry();
    g_restoringCommittedGeometry = false;
    clearCommittedGeometry();
    g_rootPresentationStateKnown = false;
    g_rootPresentationVisible = false;
    originalStyle_ = 0;
    window_ = nullptr;
}

void ChromeController::refreshAccent()
{
    auto window = static_cast<HWND>(window_);
    if (window != nullptr && IsWindow(window)) {
        // Every themed child owns an on-demand frame clock. Invalidating the
        // full HWND tree here multiplied artwork reconstruction work and could
        // starve wheel input during a palette transition. The non-client
        // border is the only surface owned by this controller.
        drawFramelessBorder(window, chromeBorderColor());
    }
}

} // namespace nekorox::component
