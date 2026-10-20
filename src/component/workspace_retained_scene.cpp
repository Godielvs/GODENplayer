#include "workspace_retained_scene.h"

#include <algorithm>
#include <cmath>

namespace nekorox::component {

VisibleRowRange retainedVisibleRows(
    double offset,
    double viewportHeight,
    double rowHeight,
    std::size_t rowCount,
    std::size_t requestedOverscan) noexcept
{
    if (rowHeight <= 0.0 || viewportHeight <= 0.0 || rowCount == 0U) return {};
    const auto first = std::min(rowCount, static_cast<std::size_t>(
        std::floor(std::max(0.0, offset) / rowHeight)));
    const auto visible = static_cast<std::size_t>(
        std::ceil(viewportHeight / rowHeight));
    const auto overscan = std::clamp<std::size_t>(requestedOverscan, 2U, 6U);
    return {first, std::min(rowCount, first + visible + overscan)};
}

RetainedScrollbarGeometry retainedScrollbarGeometry(
    D2D1_RECT_F viewport,
    float outerWidth,
    double contentHeight,
    double offset,
    double expansion) noexcept
{
    RetainedScrollbarGeometry result;
    const auto interactionRight = viewport.right >= outerWidth - 0.5f
        ? outerWidth : viewport.right;
    const auto interactionLeft = std::max(
        viewport.left, interactionRight - 22.0f);
    if (viewport.right >= outerWidth - 0.5f) {
        viewport.right = std::max(viewport.left + 1.0f, viewport.right - 8.0f);
    }
    result.viewport = viewport;
    result.contentHeight = contentHeight;
    const auto viewportHeight = static_cast<double>(viewport.bottom - viewport.top);
    if (contentHeight <= viewportHeight || viewportHeight <= 0.0) return result;
    result.visible = true;
    const auto thumbHeight = std::max(
        28.0, viewportHeight * viewportHeight / contentHeight);
    const auto range = contentHeight - viewportHeight;
    const auto position = range > 0.0
        ? std::clamp(offset / range, 0.0, 1.0) : 0.0;
    const auto top = viewport.top + static_cast<float>(
        (viewportHeight - thumbHeight) * position);
    const auto thickness = static_cast<float>(
        2.0 + std::clamp(expansion, 0.0, 1.0) * 6.0);
    result.thumb = D2D1::RectF(
        viewport.right - thickness, top, viewport.right,
        top + static_cast<float>(thumbHeight));
    result.trackHit = D2D1::RectF(
        interactionLeft, viewport.top, interactionRight, viewport.bottom);
    result.thumbHit = D2D1::RectF(
        interactionLeft, top, interactionRight,
        top + static_cast<float>(thumbHeight));
    return result;
}

} // namespace nekorox::component
