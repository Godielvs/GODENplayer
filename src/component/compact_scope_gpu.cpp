#include "compact_scope_gpu.h"

#include "compact_scope_ps.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <d2d1effectauthor.h>
#include <limits>
#include <new>

namespace nekorox::component {
namespace {
constexpr GUID kShaderId{0x4b60fef3, 0x454c, 0x4d64,
    {0xbb, 0xa2, 0x60, 0x8a, 0xa0, 0xaf, 0xb4, 0x2d}};
constexpr GUID kEffectId{0xc7560cc3, 0x7b32, 0x4f2a,
    {0xad, 0x5d, 0xa7, 0x6a, 0x8d, 0xa5, 0x87, 0xfa}};
constexpr wchar_t kConstantsProperty[] = L"Constants";

class Effect final : public ID2D1EffectImpl, public ID2D1DrawTransform {
public:
    static HRESULT CALLBACK make(IUnknown** out) noexcept
    {
        if (!out) return E_POINTER;
        *out = static_cast<ID2D1EffectImpl*>(new (std::nothrow) Effect);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    static HRESULT CALLBACK setConstants(IUnknown* object,
        const BYTE* data, UINT32 bytes) noexcept
    {
        if (!object || !data || bytes != sizeof(CompactScopeGpuConstants))
            return E_INVALIDARG;
        Microsoft::WRL::ComPtr<ID2D1EffectImpl> impl;
        const auto hr = object->QueryInterface(IID_PPV_ARGS(&impl));
        if (FAILED(hr)) return hr;
        std::memcpy(&static_cast<Effect*>(impl.Get())->constants_, data, bytes);
        return S_OK;
    }
    static HRESULT CALLBACK getConstants(const IUnknown* object,
        BYTE* data, UINT32 bytes, UINT32* actual) noexcept
    {
        if (actual) *actual = sizeof(CompactScopeGpuConstants);
        if (!object) return E_POINTER;
        if (!data) return S_OK;
        if (bytes < sizeof(CompactScopeGpuConstants)) return E_NOT_SUFFICIENT_BUFFER;
        Microsoft::WRL::ComPtr<ID2D1EffectImpl> impl;
        const auto hr = const_cast<IUnknown*>(object)->QueryInterface(IID_PPV_ARGS(&impl));
        if (FAILED(hr)) return hr;
        std::memcpy(data, &static_cast<Effect*>(impl.Get())->constants_,
            sizeof(CompactScopeGpuConstants));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id == __uuidof(IUnknown) || id == __uuidof(ID2D1EffectImpl))
            *out = static_cast<ID2D1EffectImpl*>(this);
        else if (id == __uuidof(ID2D1TransformNode))
            *out = static_cast<ID2D1TransformNode*>(this);
        else if (id == __uuidof(ID2D1Transform))
            *out = static_cast<ID2D1Transform*>(this);
        else if (id == __uuidof(ID2D1DrawTransform))
            *out = static_cast<ID2D1DrawTransform*>(this);
        if (!*out) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE Initialize(ID2D1EffectContext* context,
        ID2D1TransformGraph* graph) override
    {
        if (!context || !graph) return E_INVALIDARG;
        const auto hr = context->LoadPixelShader(kShaderId,
            g_compactScopePs, sizeof(g_compactScopePs));
        return FAILED(hr) ? hr : graph->SetSingleTransformNode(this);
    }
    HRESULT STDMETHODCALLTYPE PrepareForRender(D2D1_CHANGE_TYPE) override
    {
        // Only set by the explicitly gated fault-injection entry point.
        if (constants_.controls[3] < 0.0f) return E_FAIL;
        return drawInfo_ ? drawInfo_->SetPixelShaderConstantBuffer(
            reinterpret_cast<const BYTE*>(&constants_), sizeof(constants_))
            : E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE SetGraph(ID2D1TransformGraph*) override { return E_NOTIMPL; }
    UINT32 STDMETHODCALLTYPE GetInputCount() const override { return 0; }
    HRESULT STDMETHODCALLTYPE MapOutputRectToInputRects(const D2D1_RECT_L* out,
        D2D1_RECT_L*, UINT32 count) const override
    { return out && count == 0 ? S_OK : E_INVALIDARG; }
    HRESULT STDMETHODCALLTYPE MapInputRectsToOutputRect(const D2D1_RECT_L*,
        const D2D1_RECT_L*, UINT32 count, D2D1_RECT_L* out, D2D1_RECT_L* opaque) override
    {
        if (count || !out || !opaque) return E_INVALIDARG;
        *out = D2D1::RectL(static_cast<LONG>(std::floor(constants_.bounds[0])),
            static_cast<LONG>(std::floor(constants_.bounds[1])),
            static_cast<LONG>(std::ceil(constants_.bounds[2])),
            static_cast<LONG>(std::ceil(constants_.bounds[3])));
        *opaque = D2D1::RectL(0, 0, 0, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE MapInvalidRect(UINT32, D2D1_RECT_L,
        D2D1_RECT_L*) const override { return E_INVALIDARG; }
    HRESULT STDMETHODCALLTYPE SetDrawInfo(ID2D1DrawInfo* info) override
    {
        if (!info) return E_INVALIDARG;
        drawInfo_ = info;
        const auto hr = info->SetPixelShader(kShaderId);
        return FAILED(hr) ? hr : info->SetOutputBuffer(
            D2D1_BUFFER_PRECISION_8BPC_UNORM, D2D1_CHANNEL_DEPTH_4);
    }
private:
    std::atomic<ULONG> references_{1};
    Microsoft::WRL::ComPtr<ID2D1DrawInfo> drawInfo_;
    CompactScopeGpuConstants constants_{};
};

HRESULT registerEffect(ID2D1Factory1* factory) noexcept
{
    static constexpr wchar_t xml[] = LR"xml(<?xml version='1.0'?>
<Effect>
  <Property name='DisplayName' type='string' value='GODEN Compact Scope'/>
  <Property name='Author' type='string' value='GODENplayer'/>
  <Property name='Category' type='string' value='Artistic'/>
  <Property name='Description' type='string' value='Presented scope columns'/>
  <Inputs/>
  <Property name='Constants' type='blob'>
    <Property name='DisplayName' type='string' value='Scope constants'/>
  </Property>
</Effect>)xml";
    const D2D1_PROPERTY_BINDING property{
        kConstantsProperty, &Effect::setConstants, &Effect::getConstants};
    return factory->RegisterEffectFromString(kEffectId, xml, &property, 1, &Effect::make);
}
bool validColor(const D2D1_COLOR_F& c) noexcept
{
    const float values[]{c.r, c.g, c.b, c.a};
    for (float v : values) if (!std::isfinite(v) || v < 0 || v > 1) return false;
    return true;
}
} // namespace

void CompactScopeGpu::releaseTargetResources() noexcept
{
    effect_.Reset();
    context_.Reset();
    observedTarget_ = nullptr;
    device_.Reset();
    if (registered_) factory_->UnregisterEffect(kEffectId);
    registered_ = false;
    factory_.Reset();
    submitted_ = false;
}
void CompactScopeGpu::reset() noexcept
{
    releaseTargetResources();
    failedDevice_.Reset();
    fault_ = FaultForQa::none;
    scalarForQa_ = false;
}
void CompactScopeGpu::fail(HRESULT result) noexcept
{
    lastFailure_ = result;
    ++statistics_.failures;
    failedDevice_ = device_;
    releaseTargetResources();
}
bool CompactScopeGpu::bind(ID2D1RenderTarget* target, ID3D11Device* device) noexcept
{
    if (target == observedTarget_ && device == device_.Get() && effect_) return true;
    releaseTargetResources();
    device_ = device;
    auto hr = target->QueryInterface(IID_PPV_ARGS(&context_));
    Microsoft::WRL::ComPtr<ID2D1Factory> base;
    if (SUCCEEDED(hr)) {
        context_->GetFactory(&base);
        hr = base.As(&factory_);
    }
    if (SUCCEEDED(hr)) {
        hr = registerEffect(factory_.Get());
        registered_ = SUCCEEDED(hr);
    }
    if (SUCCEEDED(hr)) {
        const auto id = fault_ == FaultForQa::create ? GUID{} : kEffectId;
        if (fault_ == FaultForQa::create) fault_ = FaultForQa::none;
        hr = context_->CreateEffect(id, &effect_);
    }
    if (FAILED(hr)) { fail(hr); return false; }
    failedDevice_.Reset();
    observedTarget_ = target;
    ++statistics_.creations;
    return true;
}

bool CompactScopeGpu::tryDraw(ID2D1RenderTarget* target, ID3D11Device* device,
    SurfaceBackend backend, std::span<const CompactScopeColumn> columns,
    D2D1_COLOR_F accent, D2D1_COLOR_F bright) noexcept
{
    submitted_ = false;
    const auto fallback = [this]() { ++statistics_.fallback; return false; };
    if (scalarForQa_ || backend != SurfaceBackend::hardwareDxgi || !target ||
        !device || device == failedDevice_.Get()) {
        releaseTargetResources();
        return fallback();
    }
    if (columns.size() < 2 || columns.size() > constants_.columns.size() ||
        !validColor(accent) || !validColor(bright) ||
        target->GetAntialiasMode() != D2D1_ANTIALIAS_MODE_PER_PRIMITIVE) return fallback();
    D2D1_MATRIX_3X2_F matrix{};
    target->GetTransform(&matrix);
    if (matrix._11 != 1 || matrix._22 != 1 || matrix._12 != 0 ||
        matrix._21 != 0 || matrix._31 != 0 || matrix._32 != 0) return fallback();
    float dpiX = 0, dpiY = 0;
    target->GetDpi(&dpiX, &dpiY);
    if (!std::isfinite(dpiX) || dpiX != dpiY || dpiX < 48 || dpiX > 768) return fallback();
    const auto scale = dpiX / 96.0f;
    const auto firstX = columns.front().x * scale;
    const auto lastX = columns.back().x * scale;
    const auto step = (lastX - firstX) / static_cast<float>(columns.size() - 1);
    if (!std::isfinite(step) || step < .5f * scale) return fallback();
    auto low = std::numeric_limits<float>::max(), high = -low;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        const auto& c = columns[i];
        const float values[]{c.x * scale, c.top * scale, c.bottom * scale, c.midpoint * scale};
        for (float v : values) if (!std::isfinite(v) || std::abs(v) > 131072) return fallback();
        if (std::abs(values[0] - (firstX + static_cast<float>(i) * step)) > .125f) return fallback();
        constants_.columns[i] = {values[0], values[1], values[2], values[3]};
        low = std::min({low, values[1], values[2], values[3]});
        high = std::max({high, values[1], values[2], values[3]});
    }
    if (!bind(target, device)) return fallback();
    if (context_->GetPrimitiveBlend() != D2D1_PRIMITIVE_BLEND_SOURCE_OVER ||
        context_->GetUnitMode() != D2D1_UNIT_MODE_DIPS) return fallback();
    constants_.bounds = {firstX - 2 * scale, low - 2 * scale,
        lastX + 2 * scale, high + 2 * scale};
    constants_.controls = {static_cast<float>(columns.size()), scale, scale,
        fault_ == FaultForQa::render ? -1.0f : 0.0f};
    constants_.accent = {accent.r, accent.g, accent.b, accent.a * .16f};
    constants_.bright = {bright.r, bright.g, bright.b, bright.a * .92f};
    const auto bytes = fault_ == FaultForQa::update ? 0U : static_cast<UINT32>(sizeof(constants_));
    fault_ = FaultForQa::none;
    const auto hr = effect_->SetValueByName(kConstantsProperty, D2D1_PROPERTY_TYPE_BLOB,
        reinterpret_cast<const BYTE*>(&constants_), bytes);
    if (FAILED(hr)) { fail(hr); return fallback(); }
    context_->DrawImage(effect_.Get(), D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    ++statistics_.submitted;
    submitted_ = true;
    return true;
}
bool CompactScopeGpu::finishFrame(HRESULT drawResult) noexcept
{
    if (!submitted_) return false;
    submitted_ = false;
    if (FAILED(drawResult)) { fail(drawResult); return true; }
    ++statistics_.completedDraws;
    return false;
}
void CompactScopeGpu::setScalarForQa(bool scalar) noexcept
{
    reset();
    scalarForQa_ = scalar;
}
void CompactScopeGpu::queueFaultForQa(FaultForQa fault) noexcept
{
    reset();
    fault_ = fault;
}
} // namespace nekorox::component
