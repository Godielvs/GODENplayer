#include "album_ambient_effect.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using namespace nekorox;
using namespace nekorox::component;

namespace {

bool run(bool useWarp)
{
    const auto fail = [](const wchar_t* stage, HRESULT value) {
        std::wcerr << L"R6 probe stage " << stage << L" failed: 0x"
                   << std::hex << static_cast<unsigned long>(value) << L"\n";
        return false;
    };
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11DeviceContext> immediate;
    D3D_FEATURE_LEVEL level{};
    auto hr = D3D11CreateDevice(nullptr,
        useWarp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        d3d.ReleaseAndGetAddressOf(), &level,
        immediate.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return fail(L"D3D11CreateDevice", hr);
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(hr = d3d.As(&dxgi))) return fail(L"IDXGIDevice", hr);
    D2D1_FACTORY_OPTIONS options{};
    ComPtr<ID2D1Factory1> factory;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            options, factory.ReleaseAndGetAddressOf()))) return fail(L"D2D factory", E_FAIL);
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> context;
    if (FAILED(factory->CreateDevice(dxgi.Get(), device.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            context.ReleaseAndGetAddressOf()))) return fail(L"D2D device/context", E_FAIL);

    // Exercise the real 3:1 production geometry rather than a tiny
    // same-aspect placeholder. fitAmbientTexture(1800, 600) resolves to this
    // bounded target, so the probe also guards the zero-input output bounds.
    constexpr UINT32 width = 512;
    constexpr UINT32 height = 171;
    const auto format = D2D1::PixelFormat(
        DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    const auto targetProperties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, format, 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> output;
    if (FAILED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0,
            &targetProperties, output.ReleaseAndGetAddressOf()))) return fail(L"bitmaps", E_FAIL);
    ComPtr<ID2D1Effect> effect;
    hr = AlbumAmbientEffect::create(context.Get(), effect.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return fail(L"effect create", hr);

    AlbumAmbientPose pose{};
    pose.sinePhase = 0.5f;
    pose.cosinePhase = 0.8660254f;
    ArtPalette palette{};
    palette.valid = true;
    palette.primary = {0.1f, 0.8f, 0.5f};
    palette.secondary = {0.6f, 0.2f, 0.8f};
    palette.highlight = {0.9f, 0.6f, 0.2f};
    auto constants = AlbumAmbientEffect::constants(
        D2D1::SizeF(1800.0f, 600.0f), D2D1::SizeU(width, height),
        D2D1::RectF(0.55f, 0.05f, 0.95f, 0.95f),
        D2D1::RectF(0.50f, 0.08f, 0.96f, 0.92f),
        palette, pose, false, 42U);
    if (constants.levels[3] != 0.0f ||
        std::abs(constants.geometry[0] - 3.0f) > 0.0001f ||
        std::abs(constants.geometry[1] - 600.0f) > 0.0001f ||
        constants.textureBounds[2] != width ||
        constants.textureBounds[3] != height ||
        FAILED(hr = AlbumAmbientEffect::update(effect.Get(), constants)))
        return fail(L"zero constants", hr);
    context->SetTarget(output.Get());
    context->BeginDraw();
    context->Clear(D2D1::ColorF(0, 0.0f));
    context->DrawImage(effect.Get());
    if (FAILED(hr = context->EndDraw())) return fail(L"zero draw", hr);

    const auto readProperties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        format, 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> readback;
    if (FAILED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0,
            &readProperties, readback.ReleaseAndGetAddressOf())) ||
        FAILED(readback->CopyFromBitmap(nullptr, output.Get(), nullptr)))
        return fail(L"readback", E_FAIL);
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(hr = readback->Map(D2D1_MAP_OPTIONS_READ, &mapped)))
        return fail(L"map", hr);
    bool opaque = true;
    std::uint64_t staticRgb = 1469598103934665603ULL;
    for (UINT32 y = 0; y < height && opaque; ++y) {
        const auto* actual = mapped.bits + static_cast<std::size_t>(y) * mapped.pitch;
        for (UINT32 x = 0; x < width; ++x) {
            if (actual[x * 4U + 3U] < 250U) { opaque = false; break; }
            for (UINT32 channel = 0; channel < 3U; ++channel) {
                staticRgb ^= actual[x * 4U + channel];
                staticRgb *= 1099511628211ULL;
            }
        }
    }
    readback->Unmap();
    if (!opaque) return fail(L"static flow opacity", E_FAIL);

    pose.drive = {0.8f, 0.55f, 0.3f};
    pose.transient = {0.4f, 0.2f, 0.1f};
    pose.phase = {0.2f, 0.4f, 0.7f};
    pose.availability = 1.0f;
    pose.gain = 1.0f;
    constants = AlbumAmbientEffect::constants(
        D2D1::SizeF(1800.0f, 600.0f), D2D1::SizeU(width, height),
        D2D1::RectF(0.55f, 0.05f, 0.95f, 0.95f),
        D2D1::RectF(0.50f, 0.08f, 0.96f, 0.92f),
        palette, pose, true, 42U);
    if (constants.controls[0] > 1.0f ||
        constants.controls[1] > 0.16f ||
        FAILED(hr = AlbumAmbientEffect::update(effect.Get(), constants)))
        return fail(L"reactive constants", hr);
    context->SetTarget(output.Get());
    context->BeginDraw();
    context->DrawImage(effect.Get());
    hr = context->EndDraw();
    if (FAILED(hr)) return fail(L"reactive draw", hr);
    if (FAILED(readback->CopyFromBitmap(nullptr, output.Get(), nullptr)) ||
        FAILED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped))) {
        return fail(L"reactive readback", E_FAIL);
    }
    std::uint64_t reactiveRgb = 1469598103934665603ULL;
    for (UINT32 y = 0; y < height; ++y) {
        const auto* actual = mapped.bits + static_cast<std::size_t>(y) * mapped.pitch;
        for (UINT32 x = 0; x < width; ++x) {
            for (UINT32 channel = 0; channel < 3U; ++channel) {
                reactiveRgb ^= actual[x * 4U + channel];
                reactiveRgb *= 1099511628211ULL;
            }
        }
    }
    readback->Unmap();
    return reactiveRgb != staticRgb
        ? true : fail(L"temporal RGB response", E_FAIL);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const bool useWarp = argc == 2 && std::wstring_view(argv[1]) == L"--warp";
    if (argc > 1 && !useWarp) {
        std::wcerr << L"Usage: r6_d2d_effect_probe [--warp]\n";
        return 2;
    }
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wcout << (useWarp ? L"Driver: WARP (software test only)\n"
                          : L"Driver: hardware\n");
    const auto passed = run(useWarp);
    if (SUCCEEDED(initialized)) CoUninitialize();
    std::wcout << (passed ? L"R6 D2D ambient effect probe: PASS\n"
                         : L"R6 D2D ambient effect probe: FAIL\n");
    return passed ? 0 : 1;
}
