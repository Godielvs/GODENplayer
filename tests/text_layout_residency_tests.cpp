#include "../src/component/text_layout_cache.h"
#include "text_layout_test_factory.h"

#include <wincodec.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

void runTextLayoutResidencyTests()
{
    using Microsoft::WRL::ComPtr;
    using nekorox::component::TextLayoutCache;
    const auto require = [](bool value, const char* message) {
        if (!value) throw std::runtime_error(message);
    };
    const auto started = std::chrono::steady_clock::now();
    ComPtr<IDWriteFactory> factory;
    ComPtr<IDWriteTextFormat> format;
    require(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(
            factory.ReleaseAndGetAddressOf()))), "text test factory failed");
    require(SUCCEEDED(factory->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us",
        format.ReleaseAndGetAddressOf())), "text test format failed");
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

    // A single document must obey the budget even while external shared_ptrs
    // retain old documents. Measure with real DirectWrite, without truncation.
    TextLayoutCache cache;
    std::wstring manyLines;
    for (std::size_t index = 0; index < 6000; ++index) {
        manyLines += std::to_wstring(index) +
            L" música 日本語 العربية — a wrapped paragraph with more words";
        manyLines += index % 3U == 0U ? L"\r\n" : L"\n";
    }
    const auto document = cache.getOrCreate(factory.Get(),
        30U, 1U, manyLines, format.Get(), 190.0f);
    require(document && document->text == manyLines &&
        document->paragraphs.size() == 6001U &&
        cache.stats().peakResidentLayouts <= TextLayoutCache::maximumSharedLayouts,
        "extreme document escaped layout budget or lost text");
    const auto verifyGeometry = [&](const auto& measured, float layoutWidth) {
        float total = 0.0f;
        for (const auto& paragraph : measured.paragraphs) {
            ComPtr<IDWriteTextLayout> oracle;
            const auto data = paragraph.length == 0U ? L" " :
                measured.text.data() + paragraph.offset;
            require(SUCCEEDED(factory->CreateTextLayout(data,
                static_cast<UINT32>(std::max<std::size_t>(1U, paragraph.length)),
                format.Get(), layoutWidth, 1000000.0f,
                oracle.ReleaseAndGetAddressOf())), "paragraph oracle failed");
            DWRITE_TEXT_METRICS metrics{};
            require(SUCCEEDED(oracle->GetMetrics(&metrics)) &&
                paragraph.top == total &&
                paragraph.height == std::max(1.0f, metrics.height) &&
                paragraph.width == metrics.width,
                "retained paragraph changed original DirectWrite geometry");
            total += std::max(1.0f, metrics.height);
        }
        require(total == measured.height, "full scroll extent changed");
    };
    verifyGeometry(*document, 190.0f);
    std::wstring blanks = L"START\r\n";
    blanks.append(100000U, L'\n');
    blanks += L"END\n";
    const auto beforeBlank = cache.stats().layoutsCreated;
    const auto blankDocument = cache.getOrCreate(factory.Get(),
        31U, 1U, blanks, format.Get(), 190.0f);
    require(blankDocument && blankDocument->text == blanks &&
        blankDocument->paragraphs.size() == 100003U &&
        cache.stats().layoutsCreated - beforeBlank == 3U,
        "blank lines were lost or individually shaped");

    // Software WIC target: compare pixels against the original per-paragraph
    // drawing path. This never opens a player or touches the user's desktop.
    const auto comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct ComScope { bool owned; ~ComScope(){if(owned)CoUninitialize();} }
        comScope{SUCCEEDED(comResult)};
    ComPtr<IWICImagingFactory> imaging;
    ComPtr<ID2D1Factory> drawing;
    require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&imaging))) &&
        SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            drawing.ReleaseAndGetAddressOf())), "WIC/Direct2D factories failed");
    const auto pixels = [&](const auto& measured, float offset, bool retained,
                            float layoutWidth, bool transformed = false,
                            float layoutHeight = 1000000.0f) {
        ComPtr<IWICBitmap> bitmap;
        ComPtr<ID2D1RenderTarget> target;
        ComPtr<ID2D1SolidColorBrush> brush;
        const auto properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        require(SUCCEEDED(imaging->CreateBitmap(210, 120,
                GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
                bitmap.ReleaseAndGetAddressOf())) &&
            SUCCEEDED(drawing->CreateWicBitmapRenderTarget(bitmap.Get(),
                properties, target.ReleaseAndGetAddressOf())) &&
            SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1),
                brush.ReleaseAndGetAddressOf())), "offscreen target failed");
        const auto viewport = D2D1::RectF(10, 0, 200, 120);
        target->BeginDraw();
        target->Clear(D2D1::ColorF(0, 0, 0));
        if (transformed) target->SetTransform(
            D2D1::Matrix3x2F::Scale(1.1f, 1.1f) *
            D2D1::Matrix3x2F::Translation(0.25f, 0.125f));
        if (retained) {
            cache.drawVisible(target.Get(), measured, viewport,
                brush.Get(), 0.0f, offset);
        } else {
            target->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
            for (const auto& paragraph : measured.paragraphs) {
                if (paragraph.top + paragraph.height <= offset) continue;
                if (paragraph.top >= offset + 120.0f) break;
                ComPtr<IDWriteTextLayout> oracle;
                const auto data = paragraph.length == 0U ? L" " :
                    measured.text.data() + paragraph.offset;
                require(SUCCEEDED(factory->CreateTextLayout(data,
                    static_cast<UINT32>(std::max<std::size_t>(1U, paragraph.length)),
                    format.Get(), layoutWidth, layoutHeight,
                    oracle.ReleaseAndGetAddressOf())), "pixel oracle failed");
                target->DrawTextLayout(D2D1::Point2F(10, paragraph.top - offset),
                    oracle.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            target->PopAxisAlignedClip();
        }
        require(SUCCEEDED(target->EndDraw()), "layout draw failed");
        ComPtr<IWICBitmapLock> lock;
        const WICRect rectangle{0, 0, 210, 120};
        require(SUCCEEDED(bitmap->Lock(&rectangle, WICBitmapLockRead,
            lock.ReleaseAndGetAddressOf())), "layout pixels lock failed");
        UINT count = 0;
        BYTE* data = nullptr;
        require(SUCCEEDED(lock->GetDataPointer(&count, &data)), "pixels read failed");
        return std::vector<BYTE>(data, data + count);
    };
    cache.clear(); // Old shared documents remain fully drawable after eviction.
    for (const auto offset : {0.0f, document->height * .5f,
            document->height - 120.0f, 0.0f}) {
        require(pixels(*document, offset, true, 190.0f) ==
            pixels(*document, offset, false, 190.0f),
            "eviction changed visible text pixels");
    }
    const auto createdBeforeHotDraw = cache.stats().layoutsCreated;
    for (unsigned frame = 0; frame < 20U; ++frame) pixels(*document, 0.0f, true, 190.0f);
    require(cache.stats().layoutsCreated == createdBeforeHotDraw,
        "hot visible text recreated layouts");
    const auto narrowDocument = cache.getOrCreate(factory.Get(),
        30U, 1U, manyLines, format.Get(), 130.0f);
    require(static_cast<bool>(narrowDocument), "narrow document failed");
    verifyGeometry(*narrowDocument, 130.0f);
    require(pixels(*narrowDocument, 0.0f, true, 130.0f) ==
            pixels(*narrowDocument, 0.0f, false, 130.0f) &&
        cache.stats().peakResidentLayouts <= TextLayoutCache::maximumSharedLayouts,
        "resize changed pixels or exceeded pool budget");
    auto faultFactory = Microsoft::WRL::Make<TextLayoutTestFactory>();
    faultFactory->real = factory;
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    const auto faultDocument = cache.getOrCreate(faultFactory.Get(),
        55U, 1U, L"music 日本語\n\nlast line", format.Get(), 190.0f, 24.0f);
    require(static_cast<bool>(faultDocument), "failure fixture document not ready");
    cache.clear();
    faultFactory->fail = true;
    for (bool transformed : {false, true}) {
        require(pixels(*faultDocument, 0.125f, true, 190.0f, transformed, 24.0f) ==
            pixels(*faultDocument, 0.125f, false, 190.0f, transformed, 24.0f),
            "allocation-failure fallback lost text or changed natural layout pixels");
    }
    require(faultFactory->failedCalls > 0U && cache.stats().residentLayouts == 0U,
        "cache allocation fault was not exercised");
    faultFactory->fail = false;
    require(pixels(*faultDocument, 0.125f, true, 190.0f, false, 24.0f) ==
        pixels(*faultDocument, 0.125f, false, 190.0f, false, 24.0f) &&
        cache.stats().residentLayouts == 3U, "cache did not recover after failed draw");
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    std::cout << "Text layouts: 6001 distinct lines, 100003 lines with blanks, two widths, "
        "shared documents and pixel oracle PASS; peak="
        << cache.stats().peakResidentLayouts << "; test_ms="
        << std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count() << '\n';
}
