#pragma once
#include <dwrite.h>
#include <wrl.h>
#include <atomic>
#include <functional>

// Only layout creation is consumed by TextLayoutCache. Keep shaping/rendering
// real while injecting a cache allocation failure after the document is ready.
class TextLayoutTestFactory final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDWriteFactory> {
public:
    Microsoft::WRL::ComPtr<IDWriteFactory> real;
    bool fail = false;
    unsigned failedCalls = 0;
    std::atomic<unsigned> createdCalls{0};
    std::atomic<unsigned> uiCalls{0};
    DWORD uiThread = 0;
    std::function<void()> beforeCreate;
    HRESULT STDMETHODCALLTYPE CreateTextLayout(const WCHAR* text, UINT32 length,
        IDWriteTextFormat* format, FLOAT width, FLOAT height,
        IDWriteTextLayout** output) override
    {
        ++createdCalls;
        if (uiThread && GetCurrentThreadId() == uiThread) ++uiCalls;
        if (beforeCreate) beforeCreate();
        if (fail) { ++failedCalls; *output = nullptr; return E_OUTOFMEMORY; }
        return real->CreateTextLayout(text, length, format, width, height, output);
    }
#define QA_UNUSED_FACTORY(name, parameters) HRESULT STDMETHODCALLTYPE name parameters override { return E_NOTIMPL; }
    QA_UNUSED_FACTORY(GetSystemFontCollection, (IDWriteFontCollection**, BOOL))
    QA_UNUSED_FACTORY(CreateCustomFontCollection, (IDWriteFontCollectionLoader*, const void*, UINT32, IDWriteFontCollection**))
    QA_UNUSED_FACTORY(RegisterFontCollectionLoader, (IDWriteFontCollectionLoader*))
    QA_UNUSED_FACTORY(UnregisterFontCollectionLoader, (IDWriteFontCollectionLoader*))
    QA_UNUSED_FACTORY(CreateFontFileReference, (const WCHAR*, const FILETIME*, IDWriteFontFile**))
    QA_UNUSED_FACTORY(CreateCustomFontFileReference, (const void*, UINT32, IDWriteFontFileLoader*, IDWriteFontFile**))
    QA_UNUSED_FACTORY(CreateFontFace, (DWRITE_FONT_FACE_TYPE, UINT32, IDWriteFontFile* const*, UINT32, DWRITE_FONT_SIMULATIONS, IDWriteFontFace**))
    QA_UNUSED_FACTORY(CreateRenderingParams, (IDWriteRenderingParams**))
    QA_UNUSED_FACTORY(CreateMonitorRenderingParams, (HMONITOR, IDWriteRenderingParams**))
    QA_UNUSED_FACTORY(CreateCustomRenderingParams, (FLOAT, FLOAT, FLOAT, DWRITE_PIXEL_GEOMETRY, DWRITE_RENDERING_MODE, IDWriteRenderingParams**))
    QA_UNUSED_FACTORY(RegisterFontFileLoader, (IDWriteFontFileLoader*))
    QA_UNUSED_FACTORY(UnregisterFontFileLoader, (IDWriteFontFileLoader*))
    HRESULT STDMETHODCALLTYPE CreateTextFormat(const WCHAR* family, IDWriteFontCollection* collection,
        DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style, DWRITE_FONT_STRETCH stretch,
        FLOAT size, const WCHAR* locale, IDWriteTextFormat** output) override {
        return real->CreateTextFormat(family, collection, weight, style, stretch, size, locale, output);
    }
    QA_UNUSED_FACTORY(CreateTypography, (IDWriteTypography**))
    QA_UNUSED_FACTORY(GetGdiInterop, (IDWriteGdiInterop**))
    QA_UNUSED_FACTORY(CreateGdiCompatibleTextLayout, (const WCHAR*, UINT32, IDWriteTextFormat*, FLOAT, FLOAT, FLOAT, const DWRITE_MATRIX*, BOOL, IDWriteTextLayout**))
    QA_UNUSED_FACTORY(CreateEllipsisTrimmingSign, (IDWriteTextFormat*, IDWriteInlineObject**))
    QA_UNUSED_FACTORY(CreateTextAnalyzer, (IDWriteTextAnalyzer**))
    QA_UNUSED_FACTORY(CreateNumberSubstitution, (DWRITE_NUMBER_SUBSTITUTION_METHOD, const WCHAR*, BOOL, IDWriteNumberSubstitution**))
    QA_UNUSED_FACTORY(CreateGlyphRunAnalysis, (const DWRITE_GLYPH_RUN*, FLOAT, const DWRITE_MATRIX*, DWRITE_RENDERING_MODE, DWRITE_MEASURING_MODE, FLOAT, FLOAT, IDWriteGlyphRunAnalysis**))
#undef QA_UNUSED_FACTORY
};
