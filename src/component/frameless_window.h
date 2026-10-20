#pragma once

#include "sdk_compat.h"

#include <cstdint>

namespace nekorox::component {

std::intptr_t enableFramelessWindow(HWND window);
void restoreWindowFrame(HWND window, std::intptr_t originalStyle);
void applyFramelessClientInsets(
    HWND window,
    WPARAM wParam,
    LPARAM lParam) noexcept;
void drawFramelessBorder(
    HWND window,
    COLORREF color) noexcept;
LRESULT framelessResizeHitTest(HWND window, LPARAM lParam) noexcept;
void applyFramelessMinMax(HWND window, LPARAM lParam) noexcept;
void applyDpiAwareMinimum(
    HWND window,
    MINMAXINFO* info,
    int widthDip,
    int heightDip) noexcept;
void beginFramelessMove(HWND window);
void toggleFramelessMaximize(HWND window);
void showFramelessSystemMenu(HWND window, POINT screenPoint);

} // namespace nekorox::component
