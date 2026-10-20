#pragma once

#include <d2d1.h>

#include <cstddef>

namespace nekorox::component {

struct VisibleRowRange {
    std::size_t first = 0;
    std::size_t last = 0;
};

[[nodiscard]] VisibleRowRange retainedVisibleRows(
    double offset,
    double viewportHeight,
    double rowHeight,
    std::size_t rowCount,
    std::size_t requestedOverscan) noexcept;

struct RetainedScrollbarGeometry {
    bool visible = false;
    D2D1_RECT_F viewport{};
    D2D1_RECT_F thumb{};
    D2D1_RECT_F trackHit{};
    D2D1_RECT_F thumbHit{};
    double contentHeight = 0.0;
};

[[nodiscard]] RetainedScrollbarGeometry retainedScrollbarGeometry(
    D2D1_RECT_F viewport,
    float outerWidth,
    double contentHeight,
    double offset,
    double expansion) noexcept;

} // namespace nekorox::component
