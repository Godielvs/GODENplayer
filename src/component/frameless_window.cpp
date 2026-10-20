#include "frameless_window.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>

namespace nekorox::component {
namespace {

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_NCRENDERING_POLICY
#define DWMWA_NCRENDERING_POLICY 2
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif

constexpr DWORD kDwmCornerDoNotRound = 1;
constexpr DWORD kDwmNcRenderingDisabled = 1;
constexpr COLORREF kDwmBorder = RGB(0x23, 0x29, 0x2a);

void applyDwm(HWND window)
{
    const BOOL dark = TRUE;
    // The Windows build used by the reference machine does not implement
    // DWMWA_BORDER_COLOR (E_INVALIDARG) and otherwise paints its hot-pink
    // system accent outside our custom frame. Disable only DWM's non-client
    // paint; resizing and hit testing remain provided by this window.
    DwmSetWindowAttribute(window, DWMWA_NCRENDERING_POLICY,
        &kDwmNcRenderingDisabled, sizeof(kDwmNcRenderingDisabled));
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE,
        &dark, sizeof(dark));
    DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE,
        &kDwmCornerDoNotRound, sizeof(kDwmCornerDoNotRound));
    DwmSetWindowAttribute(window, DWMWA_BORDER_COLOR,
        &kDwmBorder, sizeof(kDwmBorder));
}

int resizeExtent(HWND window, double dip) noexcept
{
    const auto dpi = std::max<UINT>(96, GetDpiForWindow(window));
    const auto frame = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi);
    const auto padding = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    const auto interactionBorder = static_cast<int>(std::lround(
        dip * static_cast<double>(dpi) / 96.0));
    return std::max(interactionBorder, frame + padding);
}

int resizeSideBorder(HWND window) noexcept
{
    return resizeExtent(window, 8.0);
}

int resizeTopBorder(HWND window) noexcept
{
    return resizeExtent(window, 8.0);
}

int resizeCornerReach(HWND window) noexcept
{
    return resizeExtent(window, 20.0);
}

int visualBorder(HWND window) noexcept
{
    const auto dpi = std::max<UINT>(96, GetDpiForWindow(window));
    return std::max(
        1,
        static_cast<int>(std::lround(
            static_cast<double>(dpi) / 96.0)));
}

} // namespace

std::intptr_t enableFramelessWindow(HWND window)
{
    if (window == nullptr) return 0;
    const auto original = GetWindowLongPtrW(window, GWL_STYLE);
    const auto next = (original & ~static_cast<LONG_PTR>(WS_CAPTION)) |
        WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    SetWindowLongPtrW(window, GWL_STYLE, next);
    applyDwm(window);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
        SWP_NOACTIVATE | SWP_NOZORDER);
    // Some Windows 11 builds restore the system accent during the frame
    // transition above. Apply the neutral border again after the new frame
    // has been committed so the outermost physical pixel cannot turn pink.
    applyDwm(window);
    return original;
}

void restoreWindowFrame(HWND window, std::intptr_t originalStyle)
{
    if (window == nullptr || !IsWindow(window) || originalStyle == 0) return;
    SetWindowLongPtrW(window, GWL_STYLE, originalStyle);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
        SWP_NOACTIVATE | SWP_NOZORDER);
}

void applyFramelessClientInsets(
    HWND window,
    WPARAM wParam,
    LPARAM lParam) noexcept
{
    if (window == nullptr || lParam == 0) return;
    RECT* client = nullptr;
    if (wParam != FALSE) {
        auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
        client = &params->rgrc[0];
    } else {
        client = reinterpret_cast<RECT*>(lParam);
    }
    const auto border = visualBorder(window);
    client->left += border;
    client->top += border;
    client->right -= border;
    client->bottom -= border;
}

void drawFramelessBorder(HWND window, COLORREF color) noexcept
{
    if (window == nullptr || !IsWindow(window) || IsIconic(window)) return;
    RECT bounds{};
    if (!GetWindowRect(window, &bounds)) return;
    OffsetRect(&bounds, -bounds.left, -bounds.top);
    const auto context = GetWindowDC(window);
    if (context == nullptr) return;
    const auto brush = CreateSolidBrush(color);
    if (brush != nullptr) {
        const auto border = visualBorder(window);
        for (int index = 0; index < border; ++index) {
            FrameRect(context, &bounds, brush);
            InflateRect(&bounds, -1, -1);
        }
        DeleteObject(brush);
    }
    ReleaseDC(window, context);
}

LRESULT framelessResizeHitTest(HWND window, LPARAM lParam) noexcept
{
    if (window == nullptr || IsZoomed(window)) return HTCLIENT;
    RECT bounds{};
    if (!GetWindowRect(window, &bounds)) return HTCLIENT;
    const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    if (point.x < bounds.left || point.x >= bounds.right ||
            point.y < bounds.top || point.y >= bounds.bottom) {
        return HTCLIENT;
    }

    const auto sideBorder = resizeSideBorder(window);
    const auto topBorder = resizeTopBorder(window);
    const auto cornerReach = resizeCornerReach(window);
    const auto fromLeft = point.x - bounds.left;
    const auto fromRight = bounds.right - 1 - point.x;
    const auto fromTop = point.y - bounds.top;
    const auto fromBottom = bounds.bottom - 1 - point.y;
    const auto left = fromLeft < sideBorder;
    const auto right = fromRight < sideBorder;
    const auto top = fromTop < topBorder;
    const auto bottom = fromBottom < sideBorder;

    // Extend corners along both adjoining edges instead of reserving a large
    // square of client content. This preserves Menu/header clicks while making
    // diagonal resize acquisition much more forgiving.
    if ((top && fromLeft < cornerReach) ||
            (left && fromTop < cornerReach)) return HTTOPLEFT;
    if ((top && fromRight < cornerReach) ||
            (right && fromTop < cornerReach)) return HTTOPRIGHT;
    if ((bottom && fromLeft < cornerReach) ||
            (left && fromBottom < cornerReach)) return HTBOTTOMLEFT;
    if ((bottom && fromRight < cornerReach) ||
            (right && fromBottom < cornerReach)) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
    return HTCLIENT;
}

void applyFramelessMinMax(HWND window, LPARAM lParam) noexcept
{
    auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
    if (window == nullptr || info == nullptr) return;
    const auto monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (!GetMonitorInfoW(monitor, &monitorInfo)) return;
    const auto& work = monitorInfo.rcWork;
    const auto& area = monitorInfo.rcMonitor;
    info->ptMaxPosition.x = work.left - area.left;
    info->ptMaxPosition.y = work.top - area.top;
    info->ptMaxSize.x = work.right - work.left;
    info->ptMaxSize.y = work.bottom - work.top;
}

void applyDpiAwareMinimum(
    HWND window,
    MINMAXINFO* info,
    int widthDip,
    int heightDip) noexcept
{
    if (window == nullptr || info == nullptr) return;
    const auto dpi = static_cast<int>(
        (std::max)(1U, GetDpiForWindow(window)));
    const auto desiredWidth = MulDiv((std::max)(1, widthDip), dpi, 96);
    const auto desiredHeight = MulDiv((std::max)(1, heightDip), dpi, 96);

    auto availableWidth = static_cast<int>(info->ptMaxSize.x);
    auto availableHeight = static_cast<int>(info->ptMaxSize.y);
    if (availableWidth <= 0 || availableHeight <= 0) {
        const auto monitor = MonitorFromWindow(
            window, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{sizeof(monitorInfo)};
        if (GetMonitorInfoW(monitor, &monitorInfo)) {
            availableWidth = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
            availableHeight = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
        }
    }

    info->ptMinTrackSize.x = availableWidth > 0
        ? (std::min)(desiredWidth, availableWidth)
        : desiredWidth;
    info->ptMinTrackSize.y = availableHeight > 0
        ? (std::min)(desiredHeight, availableHeight)
        : desiredHeight;
}

void beginFramelessMove(HWND window)
{
    if (window == nullptr) return;
    ReleaseCapture();
    SendMessageW(window, WM_NCLBUTTONDOWN, HTCAPTION, 0);
}

void toggleFramelessMaximize(HWND window)
{
    if (window == nullptr) return;
    ShowWindow(window, IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE);
}

void showFramelessSystemMenu(HWND window, POINT screenPoint)
{
    if (window == nullptr) return;
    const auto menu = GetSystemMenu(window, FALSE);
    if (menu == nullptr) return;
    EnableMenuItem(menu, SC_RESTORE,
        MF_BYCOMMAND | (IsZoomed(window) ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, SC_MAXIMIZE,
        MF_BYCOMMAND | (IsZoomed(window) ? MF_GRAYED : MF_ENABLED));
    const auto command = TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, 0, window, nullptr);
    if (command != 0) PostMessageW(window, WM_SYSCOMMAND, command, 0);
}

} // namespace nekorox::component
