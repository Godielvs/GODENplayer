#pragma once

#include <algorithm>
#include <array>
#include <d2d1.h>

namespace nekorox::component {

enum class ListenLiveFocus : unsigned char {
    none = 0,
    scope = 1,
    crt = 2,
    dynamics = 3,
    spectrogram = 4,
};

struct ListenPaneLayout {
    D2D1_RECT_F frame{};
    D2D1_RECT_F header{};
    D2D1_RECT_F plot{};
    D2D1_RECT_F focusHit{};
    bool visible = false;
};

struct ListenLiveLayout {
    ListenPaneLayout scope{};
    ListenPaneLayout crt{};
    ListenPaneLayout dynamics{};
    ListenPaneLayout spectrogram{};

    [[nodiscard]] const ListenPaneLayout* pane(ListenLiveFocus focus) const noexcept
    {
        switch (focus) {
        case ListenLiveFocus::scope: return &scope;
        case ListenLiveFocus::crt: return &crt;
        case ListenLiveFocus::dynamics: return &dynamics;
        case ListenLiveFocus::spectrogram: return &spectrogram;
        default: return nullptr;
        }
    }
};

[[nodiscard]] inline bool validListenRect(const D2D1_RECT_F& rectangle) noexcept
{
    return rectangle.right > rectangle.left && rectangle.bottom > rectangle.top;
}

[[nodiscard]] inline ListenPaneLayout makeListenPane(
    const D2D1_RECT_F& frame) noexcept
{
    ListenPaneLayout pane;
    pane.frame = frame;
    pane.visible = validListenRect(frame);
    if (!pane.visible) return pane;
    pane.header = D2D1::RectF(frame.left, frame.top, frame.right,
        (std::min)(frame.bottom, frame.top + 30.0f));
    pane.plot = D2D1::RectF(
        (std::min)(frame.right, frame.left + 12.0f),
        (std::min)(frame.bottom, frame.top + 32.0f),
        (std::max)(frame.left, frame.right - 12.0f),
        (std::max)(frame.top, frame.bottom - 12.0f));
    if (!validListenRect(pane.plot)) pane.plot = {};
    pane.focusHit = D2D1::RectF(
        (std::max)(frame.left, frame.right - 28.0f), frame.top + 3.0f,
        frame.right - 3.0f, (std::min)(frame.bottom, frame.top + 27.0f));
    return pane;
}

[[nodiscard]] inline ListenLiveLayout computeListenLayout(
    const D2D1_RECT_F& content,
    ListenLiveFocus focus = ListenLiveFocus::none,
    float dpi = 96.0f) noexcept
{
    ListenLiveLayout result;
    const auto safeFocus = static_cast<unsigned>(focus) <= 4U
        ? focus : ListenLiveFocus::none;
    const auto reserveCrtCaption = [](ListenPaneLayout& pane) {
        if (!pane.visible) return;
        pane.plot.bottom = (std::max)(pane.plot.top, pane.frame.bottom - 34.0f);
    };
    if (safeFocus != ListenLiveFocus::none) {
        const auto focused = makeListenPane(content);
        if (safeFocus == ListenLiveFocus::scope) result.scope = focused;
        else if (safeFocus == ListenLiveFocus::crt) {
            result.crt = focused;
            reserveCrtCaption(result.crt);
        }
        else if (safeFocus == ListenLiveFocus::dynamics) result.dynamics = focused;
        else if (safeFocus == ListenLiveFocus::spectrogram) result.spectrogram = focused;
        return result;
    }
    (void)dpi;
    const auto width = (std::max)(0.0f, content.right - content.left);
    const auto height = (std::max)(0.0f, content.bottom - content.top);
    const auto gapX = (std::min)(12.0f, width);
    const auto gapY = (std::min)(12.0f, height);
    const auto splitX = content.left + (width - gapX) * 0.5f;
    const auto splitY = content.top + (height - gapY) * 0.5f;
    result.scope = makeListenPane(D2D1::RectF(
        content.left, content.top, splitX, splitY));
    result.crt = makeListenPane(D2D1::RectF(
        splitX + gapX, content.top, content.right, splitY));
    reserveCrtCaption(result.crt);
    result.dynamics = makeListenPane(D2D1::RectF(
        content.left, splitY + gapY, splitX, content.bottom));
    result.spectrogram = makeListenPane(D2D1::RectF(
        splitX + gapX, splitY + gapY, content.right, content.bottom));
    return result;
}

[[nodiscard]] inline D2D1_RECT_F squareWithinListenRect(
    const D2D1_RECT_F& rectangle) noexcept
{
    const auto side = (std::max)(0.0f, (std::min)(
        rectangle.right - rectangle.left,
        rectangle.bottom - rectangle.top));
    const auto x = (rectangle.left + rectangle.right - side) * 0.5f;
    const auto y = (rectangle.top + rectangle.bottom - side) * 0.5f;
    return D2D1::RectF(x, y, x + side, y + side);
}

} // namespace nekorox::component
