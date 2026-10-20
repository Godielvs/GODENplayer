#pragma once

#include <windows.h>

namespace nekorox::component {

// A bounding rectangle cannot distinguish two small disjoint invalidations
// from damage to all of the controls between them.
inline bool isDspMeterRegionOnly(HRGN update, const RECT& client,
    const RECT& footer) noexcept
{
    const auto dirty = CreateRectRgn(0, 0, 0, 0);
    const auto meters = CreateRectRgn(0, 0, client.right, 64);
    const auto footerRegion = CreateRectRgnIndirect(&footer);
    bool result = false;
    if (dirty && meters && footerRegion &&
        CombineRgn(dirty, update, nullptr, RGN_COPY) != ERROR &&
        CombineRgn(meters, meters, footerRegion, RGN_OR) != ERROR) {
        result = CombineRgn(dirty, dirty, meters, RGN_DIFF) == NULLREGION;
    }
    if (footerRegion) DeleteObject(footerRegion);
    if (meters) DeleteObject(meters);
    if (dirty) DeleteObject(dirty);
    return result;
}

inline bool isDspMeterDamageOnly(HWND window, const RECT& footer) noexcept
{
    RECT client{};
    if (!GetClientRect(window, &client)) return false;
    const auto update = CreateRectRgn(0, 0, 0, 0);
    if (!update) return false;
    const auto kind = GetUpdateRgn(window, update, FALSE);
    const auto result = kind != ERROR && kind != NULLREGION &&
        isDspMeterRegionOnly(update, client, footer);
    DeleteObject(update);
    return result;
}

} // namespace nekorox::component
