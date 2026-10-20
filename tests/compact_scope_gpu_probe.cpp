// Actual production effect, real D3D/D2D contexts and readback. No player,
// personal profile, driver reset or global graphics state is touched.
#include "compact_scope_gpu.h"
#include <dxgi1_2.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace nekorox::component;
using Microsoft::WRL::ComPtr;
namespace {
unsigned assertions = 0;
void require(bool ok, const char* message)
{ ++assertions; if (!ok) throw std::runtime_error(message); }
void check(HRESULT hr)
{ if (FAILED(hr)) throw std::runtime_error("HRESULT " + std::to_string(unsigned(hr))); }
constexpr GUID effectId{0xc7560cc3, 0x7b32, 0x4f2a,
    {0xad, 0x5d, 0xa7, 0x6a, 0x8d, 0xa5, 0x87, 0xfa}};

struct Fixture {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> d2d;
    ComPtr<ID2D1DeviceContext> context;
    ComPtr<ID2D1Bitmap1> output, read;
    UINT width = 320, height = 110;
    bool warp;
    explicit Fixture(bool software = false, ID2D1Factory1* sharedFactory = nullptr) : warp(software)
    {
        check(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
            nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, &immediate));
        if (sharedFactory) factory = sharedFactory;
        else check(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, factory.GetAddressOf()));
        ComPtr<IDXGIDevice> dxgi;
        check(device.As(&dxgi));
        check(factory->CreateDevice(dxgi.Get(), &d2d));
        newContext();
    }
    explicit Fixture(Fixture& shared)
        : device(shared.device), immediate(shared.immediate), factory(shared.factory),
          d2d(shared.d2d), warp(shared.warp)
    { newContext(); }
    void newContext()
    {
        context.Reset();
        check(d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context));
        resize(width, height);
    }
    void resize(UINT w, UINT h)
    {
        width = w; height = h;
        context->SetTarget(nullptr);
        output.Reset(); read.Reset();
        auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        check(context->CreateBitmap({width, height}, nullptr, 0, &properties, &output));
        properties.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        check(context->CreateBitmap({width, height}, nullptr, 0, &properties, &read));
        context->SetTarget(output.Get());
        context->SetDpi(96, 96);
    }
    bool registered()
    {
        ComPtr<ID2D1Properties> properties;
        return SUCCEEDED(factory->GetEffectProperties(effectId, &properties));
    }
    std::vector<BYTE> pixels()
    {
        check(read->CopyFromBitmap(nullptr, output.Get(), nullptr));
        D2D1_MAPPED_RECT mapped{};
        check(read->Map(D2D1_MAP_OPTIONS_READ, &mapped));
        std::vector<BYTE> result(width * height * 4);
        for (UINT y = 0; y < height; ++y)
            std::copy_n(mapped.bits + mapped.pitch * y, width * 4, result.data() + y * width * 4);
        check(read->Unmap());
        return result;
    }
};

std::array<CompactScopeColumn, 148> waveform()
{
    std::array<CompactScopeColumn, 148> result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto y = 44.0f + 25.0f * std::sin(float(i) * .1f);
        result[i] = {20.5f + float(i), y - 3, y + 2, y};
    }
    return result;
}
bool draw(Fixture& f, CompactScopeGpu& gpu, std::span<const CompactScopeColumn> data,
    D2D1_COLOR_F bright = D2D1::ColorF(.8f, .6f, .9f), bool allowDrawFailure = false)
{
    f.context->BeginDraw();
    f.context->Clear(D2D1::ColorF(.02f, .02f, .03f, 1));
    const auto used = gpu.tryDraw(f.context.Get(), f.device.Get(),
        f.warp ? SurfaceBackend::warpDxgi : SurfaceBackend::hardwareDxgi,
        data, D2D1::ColorF(.4f, .2f, .6f), bright);
    const auto hr = f.context->EndDraw();
    const auto repaint = gpu.finishFrame(hr);
    if (allowDrawFailure) {
        require(used && FAILED(hr) && repaint, "Deferred failure must request scalar repaint");
    } else {
        check(hr);
        require(!repaint, "Successful draw must not request recovery");
    }
    return used;
}
}

int main()
{
    try {
        const auto columns = waveform();
        Fixture f;
        CompactScopeGpu a, b;
        require(!f.registered(), "Factory begins without the scope effect");
        require(draw(f, a, columns), "Hardware uses effect");
        const auto purple = f.pixels();
        require(draw(f, b, columns, D2D1::ColorF(.9f, .03f, .01f)), "Second owner uses effect");
        const auto red = f.pixels();
        require(purple != red, "Owners keep independent colors/constants");
        require(f.registered(), "Effect registered on shared factory");
        a.reset();
        require(f.registered(), "One owner cannot unregister another owner's effect");
        require(draw(f, b, columns, D2D1::ColorF(.9f, .03f, .01f)), "Surviving owner still renders");
        require(red == f.pixels(), "Surviving owner pixels unchanged");
        const auto creates = b.statistics().creations;
        f.resize(420, 140);
        require(draw(f, b, columns), "Resize renders");
        require(b.statistics().creations == creates, "Resize on same context reuses effect");
        f.context->SetDpi(144, 144);
        require(draw(f, b, columns), "Square fractional DPI renders");
        require(b.statistics().creations == creates, "DPI update needs no effect recreation");
        f.newContext();
        require(draw(f, b, columns), "New context on same device renders");
        require(b.statistics().creations == creates + 1, "Context change recreates exactly once");
        b.reset();
        require(!f.registered(), "Last owner balances factory registration");

        Fixture sibling(f);
        require(sibling.context.Get() != f.context.Get() &&
            sibling.device.Get() == f.device.Get() && sibling.factory.Get() == f.factory.Get(),
            "Independent contexts share device and factory");
        CompactScopeGpu firstContext, secondContext;
        require(draw(f, firstContext, columns), "First context draws independently");
        require(draw(sibling, secondContext, columns), "Second context draws independently");
        const auto siblingPixels = sibling.pixels();
        firstContext.reset();
        f.newContext();
        require(draw(sibling, secondContext, columns), "Other context survives owner/target destruction");
        require(siblingPixels == sibling.pixels(), "Other context pixels remain unchanged");
        secondContext.reset();
        require(!f.registered(), "Both contexts balance shared factory registration");

        CompactScopeGpu invalid;
        f.context->SetDpi(120, 144);
        require(!draw(f, invalid, columns), "Non-square DPI uses scalar");
        f.context->SetDpi(96, 96);
        f.context->SetTransform(D2D1::Matrix3x2F::Translation(1, 2));
        require(!draw(f, invalid, columns), "Transformed target uses scalar");
        f.context->SetTransform(D2D1::Matrix3x2F::Identity());
        f.context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        require(!draw(f, invalid, columns), "Aliased target uses scalar");
        f.context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        f.context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
        require(!draw(f, invalid, columns), "Copy compositing uses scalar");
        f.context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        f.context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        require(!draw(f, invalid, columns), "Pixel unit mode uses scalar");
        f.context->SetUnitMode(D2D1_UNIT_MODE_DIPS);
        auto bad = columns;
        bad[3].midpoint = std::numeric_limits<float>::quiet_NaN();
        require(!draw(f, invalid, bad), "Nonfinite data uses scalar");
        bad = columns; bad[3].x += 3;
        require(!draw(f, invalid, bad), "Nonuniform columns use scalar");
        require(!draw(f, invalid, std::span<const CompactScopeColumn>{}), "Empty columns use scalar");
        std::array<CompactScopeColumn, 513> oversized{};
        require(!draw(f, invalid, oversized), "Oversized columns use scalar");
        require(!draw(f, invalid, columns, D2D1::ColorF(2, 0, 0)), "Unsupported color uses scalar");
        require(draw(f, invalid, columns), "Compatible state recovers without blacklisting");
        invalid.reset();

        for (const auto fault : {CompactScopeGpu::FaultForQa::create,
                CompactScopeGpu::FaultForQa::update, CompactScopeGpu::FaultForQa::render}) {
            CompactScopeGpu gpu;
            gpu.queueFaultForQa(fault);
            const auto used = draw(f, gpu, columns, D2D1::ColorF(.8f, .6f, .9f),
                fault == CompactScopeGpu::FaultForQa::render);
            require(used == (fault == CompactScopeGpu::FaultForQa::render), "Correct failure phase");
            require(gpu.statistics().failures == 1 && FAILED(gpu.lastFailure()), "One recorded failure");
            const auto created = gpu.statistics().creations;
            for (unsigned i = 0; i < 8; ++i) require(!draw(f, gpu, columns), "Same device stays scalar");
            gpu.releaseTargetResources();
            f.newContext();
            require(!draw(f, gpu, columns), "Target/DPI reset must preserve same-device failure");
            require(gpu.statistics().creations == created && gpu.statistics().failures == 1,
                "No recreate/failure loop");
            Fixture replacement(false, f.factory.Get());
            require(replacement.device.Get() != f.device.Get(), "New hardware device identity");
            require(replacement.factory.Get() == f.factory.Get(), "Recovery retains factory identity");
            require(draw(replacement, gpu, columns), "New physical device instance allows recovery");
            require(gpu.statistics().completedDraws == 1, "Recovered draw completes");
        }
        Fixture software(true);
        CompactScopeGpu warp;
        require(!draw(software, warp, columns), "WARP retains scalar path");
        require(!software.registered() && warp.statistics().creations == 0,
            "No effect allocation on software backend");
        CompactScopeGpu toggle;
        toggle.setScalarForQa(true);
        require(!draw(f, toggle, columns), "Forced scalar baseline");
        toggle.setScalarForQa(false);
        require(draw(f, toggle, columns), "QA toggle restores GPU");
        toggle.reset();
        require(!f.registered(), "No effect registrations after teardown");
        std::cout << "Compact scope GPU PASS: " << assertions
            << " checks; independent owners, context/DPI/resize, fallback, three fault stages, no retry loop.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
