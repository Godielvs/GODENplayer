#include "../src/component/listen_layout.h"

#include <cmath>
#include <stdexcept>

void runCorrectiveR2ListenTests()
{
    using namespace nekorox::component;
    const auto content = D2D1::RectF(0, 0, 1000, 800);
    const auto layout = computeListenLayout(content);
    if (!layout.scope.visible || !layout.crt.visible || !layout.dynamics.visible || !layout.spectrogram.visible)
        throw std::runtime_error("L1 grid does not expose four panes");
    if (std::abs((layout.scope.frame.right-layout.scope.frame.left) - (layout.crt.frame.right-layout.crt.frame.left)) > 0.01f ||
        std::abs((layout.scope.frame.bottom-layout.scope.frame.top) - (layout.dynamics.frame.bottom-layout.dynamics.frame.top)) > 0.01f ||
        std::abs(layout.crt.frame.left-layout.scope.frame.right-12.0f) > 0.01f ||
        std::abs(layout.dynamics.frame.top-layout.scope.frame.bottom-12.0f) > 0.01f)
        throw std::runtime_error("L1 grid is not an equal 2x2 layout");
    for (unsigned value=1; value<=4; ++value) {
        const auto focus=static_cast<ListenLiveFocus>(value);
        const auto focused=computeListenLayout(content,focus);
        const auto* pane=focused.pane(focus);
        if (!pane || !pane->visible || pane->frame.right!=1000 || pane->frame.bottom!=800)
            throw std::runtime_error("L1 focus does not occupy full content");
    }
    const auto focused=computeListenLayout(content,ListenLiveFocus::spectrogram);
    if (focused.scope.visible || focused.crt.visible || focused.dynamics.visible)
        throw std::runtime_error("L1 focus left hidden panes visible");
    const auto square=squareWithinListenRect(D2D1::RectF(10,20,410,240));
    if(square.left!=100 || square.top!=20 || square.right!=320 || square.bottom!=240)
        throw std::runtime_error("L1 CRT viewport is not square");

}
