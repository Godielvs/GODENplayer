#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace goden::pixel {
struct BoxDip { double left, top, right, bottom; };
struct AxisTransform { double xx=1, xy=0, yx=0, yy=1, dx=0, dy=0; };
struct PixelPlacement {
    std::int32_t x=0, y=0, width=0, height=0;
    bool valid=false;
};
// Exact integer boundaries for the hand-authored matrix at fractional DPI.
// Adjacent cells share the same rounded boundary: no cracks or overlaps.
[[nodiscard]] constexpr std::int32_t edge(std::uint16_t sourceEdge,
    std::uint16_t sourceExtent, std::int32_t physicalExtent) noexcept {
    if (sourceExtent == 0 || physicalExtent <= 0) return 0;
    return static_cast<std::int32_t>((static_cast<std::int64_t>(sourceEdge) *
        physicalExtent * 2 + sourceExtent) / (sourceExtent * 2));
}
[[nodiscard]] inline PixelPlacement place(std::uint16_t gridW,
    std::uint16_t gridH, BoxDip slot, double dpiX, double dpiY,
    AxisTransform transform = {}) noexcept {
    const double values[]{slot.left,slot.top,slot.right,slot.bottom,dpiX,dpiY,
        transform.xx,transform.xy,transform.yx,transform.yy,transform.dx,transform.dy};
    for (const auto v : values) if (!std::isfinite(v)) return {};
    if (gridW==0 || gridH==0 || dpiX<=0 || dpiY<=0 ||
        slot.right<=slot.left || slot.bottom<=slot.top ||
        transform.xx<=0 || transform.yy<=0 ||
        std::abs(transform.xy)>1e-8 || std::abs(transform.yx)>1e-8) return {};
    // Source aspect ratio is preserved in local DIP space. Callers pass a
    // deliberately sized glyph slot, NOT a whole 32/40-DIP hit rectangle.
    const auto s = std::min((slot.right-slot.left)/gridW,
                            (slot.bottom-slot.top)/gridH);
    const auto w = gridW*s*transform.xx*dpiX/96.0;
    const auto h = gridH*s*transform.yy*dpiY/96.0;
    const auto cx = (((slot.left+slot.right)*0.5)*transform.xx+transform.dx)*dpiX/96.0;
    const auto cy = (((slot.top+slot.bottom)*0.5)*transform.yy+transform.dy)*dpiY/96.0;
    // Bounded art resources. No reduction below the authored master: choose
    // micro/compact before reaching this function. This prevents missing cells.
    if (w < gridW-0.49 || h < gridH-0.49 || w>4096 || h>4096 ||
        std::abs(cx)>1e8 || std::abs(cy)>1e8) return {};
    const auto pw=static_cast<std::int32_t>(std::lround(w));
    const auto ph=static_cast<std::int32_t>(std::lround(h));
    if (pw<gridW || ph<gridH) return {};
    return {static_cast<std::int32_t>(std::lround(cx-pw*0.5)),
            static_cast<std::int32_t>(std::lround(cy-ph*0.5)),pw,ph,true};
}
} // namespace goden::pixel
