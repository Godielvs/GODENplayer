#pragma once
#include <d2d1.h>

namespace nekorox::component {
// Aligned/off-size render targets must fill the viewport with uniform scaling.
// Crop only the surplus source area; never distort the shader's pixel aspect.
inline D2D1_RECT_F visualSourceRectangle(D2D1_SIZE_F source,
    const D2D1_RECT_F& destination) noexcept
{
    auto result = D2D1::RectF(0, 0, source.width, source.height);
    const auto width = destination.right - destination.left;
    const auto height = destination.bottom - destination.top;
    if (width <= 0 || height <= 0 || source.width <= 0 || source.height <= 0)
        return result;
    const auto aspect = width / height;
    if (source.width / source.height > aspect) {
        const auto retained = source.height * aspect;
        result.left = (source.width - retained) * .5f;
        result.right = result.left + retained;
    } else {
        const auto retained = source.width / aspect;
        result.top = (source.height - retained) * .5f;
        result.bottom = result.top + retained;
    }
    return result;
}
}
