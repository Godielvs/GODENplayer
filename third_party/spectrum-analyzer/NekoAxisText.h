#pragma once

#include <d2d1_1.h>
#include <dwrite.h>
#include <atlbase.h>
#include <string>

inline HRESULT NekoBuildAxisText(
    IDWriteFactory* factory, IDWriteTextFormat* format,
    const std::wstring& text, const D2D1_RECT_F& rect,
    CComPtr<IDWriteTextLayout>& output) noexcept
{
    output.Release();
    if (!factory || !format || rect.right <= rect.left ||
            rect.bottom <= rect.top) return E_INVALIDARG;
    return factory->CreateTextLayout(text.c_str(), UINT(text.size()), format,
        rect.right - rect.left, rect.bottom - rect.top, &output);
}

inline void NekoDrawAxisText(
    ID2D1DeviceContext* context, IDWriteTextLayout* layout,
    IDWriteTextFormat* format, const std::wstring& text,
    const D2D1_RECT_F& rect, ID2D1Brush* brush) noexcept
{
    if (layout) {
        context->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout,
            brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    } else {
        // A failed optional cache never removes labels or changes the format.
        context->DrawText(text.c_str(), UINT(text.size()), format, rect,
            brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}
