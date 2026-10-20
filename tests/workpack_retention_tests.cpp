#include "../src/component/workspace_retained_scene.h"

#include <cmath>
#include <stdexcept>

void runWorkpackRetentionTests()
{
    using namespace nekorox::component;
    const auto rows = retainedVisibleRows(59.5, 120.0, 20.0, 100U, 99U);
    if (rows.first != 2U || rows.last != 14U || rows.last - rows.first > 12U) {
        throw std::runtime_error("WP02 visible rows did not bound overscan");
    }
    const auto shortRange = retainedVisibleRows(0.0, 80.0, 20.0, 3U, 0U);
    if (shortRange.first != 0U || shortRange.last != 3U) {
        throw std::runtime_error("WP02 small row range is incorrect");
    }
    const auto collapsed = retainedScrollbarGeometry(
        D2D1::RectF(0.0f, 10.0f, 400.0f, 210.0f),
        400.0f, 1000.0, 250.0, 0.0);
    const auto expanded = retainedScrollbarGeometry(
        D2D1::RectF(0.0f, 10.0f, 400.0f, 210.0f),
        400.0f, 1000.0, 250.0, 1.0);
    if (!collapsed.visible || !expanded.visible ||
        collapsed.thumbHit.right != 400.0f ||
        collapsed.thumbHit.left != 378.0f ||
        collapsed.thumbHit.left != expanded.thumbHit.left ||
        collapsed.thumbHit.right != expanded.thumbHit.right ||
        collapsed.trackHit.left != 378.0f ||
        collapsed.trackHit.right != 400.0f ||
        collapsed.trackHit.top != 10.0f ||
        collapsed.trackHit.bottom != 210.0f ||
        collapsed.viewport.right != 392.0f) {
        throw std::runtime_error("WP02 collapsed scrollbar hit lane is unstable");
    }
    const auto internal = retainedScrollbarGeometry(
        D2D1::RectF(200.0f, 10.0f, 400.0f, 210.0f),
        900.0f, 1000.0, 250.0, 0.0);
    if (!internal.visible || internal.viewport.right != 400.0f ||
        internal.trackHit.left != 378.0f ||
        internal.trackHit.right != 400.0f) {
        throw std::runtime_error("WP02 internal scrollbar hit lane is unstable");
    }
    const auto hidden = retainedScrollbarGeometry(
        D2D1::RectF(0.0f, 0.0f, 200.0f, 200.0f),
        500.0f, 100.0, 0.0, 0.0);
    if (hidden.visible) {
        throw std::runtime_error("WP02 showed a scrollbar for fitting content");
    }
}
