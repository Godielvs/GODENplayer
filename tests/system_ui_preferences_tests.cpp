// Headless WIC raster and real DirectWrite parameters; no global OS settings
// are changed. The motion implementation fragment is copied verbatim by the
// runner, with cfg/notification/SPI boundaries replaced by bounded test doubles.
#include "monitor_text_rendering.h"
#include "motion_policy.h"
#include "window_monitor_identity.h"
#include <wincodec.h>
#include <iostream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace nekorox::component;
namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
bool queryWorks = true, smoothingEnabled = true;
UINT smoothingType = FE_FONTSMOOTHINGCLEARTYPE;
unsigned textQueries = 0;
BOOL WINAPI textQuery(UINT action, UINT, PVOID value, UINT)
{
    ++textQueries;
    if (!queryWorks) return FALSE;
    if (action == SPI_GETFONTSMOOTHING) *static_cast<BOOL*>(value) = smoothingEnabled;
    else if (action == SPI_GETFONTSMOOTHINGTYPE) *static_cast<UINT*>(value) = smoothingType;
    else throw std::runtime_error("Unexpected system parameter query");
    return TRUE;
}
}

#include "motion-production.inc"

int main()
try {
    using namespace motion_fixture;
    run();
    require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM failed");
    struct ComScope { ~ComScope() { CoUninitialize(); } } scope;
    ComPtr<IDWriteFactory> writing;
    ComPtr<ID2D1Factory> drawing;
    ComPtr<IWICImagingFactory> imaging;
    require(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(writing.GetAddressOf()))) &&
        SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, drawing.GetAddressOf())) &&
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&imaging))), "Factories failed");
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    require(SUCCEEDED(imaging->CreateBitmap(320, 64, GUID_WICPixelFormat32bppPBGRA,
        WICBitmapCacheOnLoad, bitmap.GetAddressOf())), "WIC bitmap failed");
    const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    require(SUCCEEDED(drawing->CreateWicBitmapRenderTarget(bitmap.Get(), properties,
        target.GetAddressOf())), "WIC target failed");
    const auto monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    require(monitor != nullptr, "No monitor available");
    MonitorTextRendering policy(textQuery);
    require(policy.apply(target.Get(), writing.Get(), monitor), "Initial policy not applied");
    ComPtr<IDWriteRenderingParams> actual, oracle;
    target->GetTextRenderingParams(actual.GetAddressOf());
    require(SUCCEEDED(writing->CreateMonitorRenderingParams(monitor, oracle.GetAddressOf())) &&
        actual && actual->GetPixelGeometry() == oracle->GetPixelGeometry() &&
        actual->GetGamma() == oracle->GetGamma() &&
        actual->GetClearTypeLevel() == oracle->GetClearTypeLevel(), "Monitor parameters differ");
    const auto expected = oracle->GetPixelGeometry() == DWRITE_PIXEL_GEOMETRY_FLAT
        ? D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE : D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE;
    require(target->GetTextAntialiasMode() == expected, "Monitor text mode differs");
    const auto initialQueries = textQueries;
    for (unsigned index = 0; index < 10000; ++index)
        require(!policy.apply(target.Get(), writing.Get(), monitor), "Steady frame reapplied policy");
    require(textQueries == initialQueries, "Steady frames queried OS settings");

    smoothingEnabled = false;
    MonitorTextRendering::settingsChanged();
    require(policy.apply(target.Get(), writing.Get(), monitor) &&
        target->GetTextAntialiasMode() == D2D1_TEXT_ANTIALIAS_MODE_ALIASED,
        "Font smoothing off not respected");
    smoothingEnabled = true;
    smoothingType = FE_FONTSMOOTHINGSTANDARD;
    MonitorTextRendering::settingsChanged();
    require(policy.apply(target.Get(), writing.Get(), monitor) &&
        target->GetTextAntialiasMode() == D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE,
        "Standard smoothing did not select grayscale");

    ComPtr<IDWriteTextFormat> format;
    ComPtr<ID2D1SolidColorBrush> brush;
    require(SUCCEEDED(writing->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        21.0f, L"en-us", format.GetAddressOf())) &&
        SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),
            brush.GetAddressOf())), "Text resources failed");
    target->BeginDraw();
    target->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    target->DrawText(L"GODEN music 日本語", 15, format.Get(), D2D1::RectF(1, 1, 319, 63), brush.Get());
    require(SUCCEEDED(target->EndDraw()), "Grayscale draw failed");
    const WICRect rect{0, 0, 320, 64};
    ComPtr<IWICBitmapLock> locked;
    require(SUCCEEDED(bitmap->Lock(&rect, WICBitmapLockRead, locked.GetAddressOf())), "Pixels failed");
    UINT size = 0, stride = 0;
    BYTE* data = nullptr;
    require(SUCCEEDED(locked->GetDataPointer(&size, &data)) &&
        SUCCEEDED(locked->GetStride(&stride)), "Pixel data failed");
    unsigned lit = 0, edges = 0;
    for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 320; ++x) {
        const auto* pixel = data + y * stride + x * 4;
        require(pixel[0] == pixel[1] && pixel[1] == pixel[2], "Grayscale produced color fringes");
        lit += pixel[0] > 0;
        edges += pixel[0] > 0 && pixel[0] < 255;
    }
    require(lit > 100 && edges > 50, "Raster is empty or unsmoothed");
    locked.Reset();
    queryWorks = false;
    MonitorTextRendering::settingsChanged();
    require(policy.apply(target.Get(), writing.Get(), monitor) &&
        target->GetTextAntialiasMode() == D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE,
        "Unavailable SPI did not fall back safely");
    queryWorks = true;
    smoothingType = FE_FONTSMOOTHINGCLEARTYPE;
    require(policy.apply(target.Get(), writing.Get(), nullptr) &&
        target->GetTextAntialiasMode() == D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE,
        "Unknown monitor assumed primary subpixels");
    policy.reset();
    require(policy.apply(target.Get(), writing.Get(), monitor), "Reset did not reapply policy");
    std::cout << "PASS motion production path + monitor parameters/cache + grayscale pixels\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
