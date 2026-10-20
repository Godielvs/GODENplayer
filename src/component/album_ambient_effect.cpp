#include "album_ambient_effect.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <d2d1effectauthor.h>
#include <mutex>
#include <unordered_set>

#include "album_ambient_a1_ps.h"

namespace nekorox::component {
namespace {

constexpr GUID kShaderId{
    0x6e90c49c, 0x8048, 0x43e8,
    {0x99, 0xf8, 0x43, 0x7e, 0x5d, 0xa3, 0x31, 0x10}};
constexpr wchar_t kConstantsProperty[] = L"Constants";

class EffectImpl final : public ID2D1EffectImpl, public ID2D1DrawTransform {
public:
    static HRESULT CALLBACK make(IUnknown** result) noexcept
    {
        if (!result) return E_POINTER;
        *result = static_cast<ID2D1EffectImpl*>(new (std::nothrow) EffectImpl);
        return *result ? S_OK : E_OUTOFMEMORY;
    }

    static HRESULT CALLBACK setConstants(IUnknown* object,
        const BYTE* data, UINT32 size) noexcept
    {
        if (!object || !data || size != sizeof(AlbumAmbientConstants))
            return E_INVALIDARG;
        Microsoft::WRL::ComPtr<ID2D1EffectImpl> implementation;
        const auto hr = object->QueryInterface(
            IID_PPV_ARGS(implementation.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) return hr;
        auto* self = static_cast<EffectImpl*>(implementation.Get());
        std::memcpy(&self->constants_, data, sizeof(self->constants_));
        return S_OK;
    }

    static HRESULT CALLBACK getConstants(const IUnknown* object,
        BYTE* data, UINT32 size, UINT32* actualSize) noexcept
    {
        if (actualSize) *actualSize = sizeof(AlbumAmbientConstants);
        if (!object) return E_POINTER;
        if (!data) return S_OK;
        if (size < sizeof(AlbumAmbientConstants)) return E_NOT_SUFFICIENT_BUFFER;
        Microsoft::WRL::ComPtr<ID2D1EffectImpl> implementation;
        const auto hr = const_cast<IUnknown*>(object)->QueryInterface(
            IID_PPV_ARGS(implementation.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) return hr;
        const auto* self = static_cast<const EffectImpl*>(implementation.Get());
        std::memcpy(data, &self->constants_, sizeof(self->constants_));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (id == __uuidof(IUnknown) || id == __uuidof(ID2D1EffectImpl))
            *result = static_cast<ID2D1EffectImpl*>(this);
        else if (id == __uuidof(ID2D1TransformNode))
            *result = static_cast<ID2D1TransformNode*>(this);
        else if (id == __uuidof(ID2D1Transform))
            *result = static_cast<ID2D1Transform*>(this);
        else if (id == __uuidof(ID2D1DrawTransform))
            *result = static_cast<ID2D1DrawTransform*>(this);
        if (!*result) return E_NOINTERFACE;
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
        context_ = context;
        auto hr = context->LoadPixelShader(
            kShaderId, g_albumAmbientA1Ps, sizeof(g_albumAmbientA1Ps));
        if (FAILED(hr)) return hr;
        return graph->SetSingleTransformNode(this);
    }
    HRESULT STDMETHODCALLTYPE PrepareForRender(D2D1_CHANGE_TYPE) override
    {
        return drawInfo_ ? drawInfo_->SetPixelShaderConstantBuffer(
            reinterpret_cast<const BYTE*>(&constants_), sizeof(constants_))
            : E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE SetGraph(ID2D1TransformGraph*) override
    {
        return E_NOTIMPL;
    }

    UINT32 STDMETHODCALLTYPE GetInputCount() const override { return 0U; }
    HRESULT STDMETHODCALLTYPE MapOutputRectToInputRects(
        const D2D1_RECT_L* output, D2D1_RECT_L*,
        UINT32 inputCount) const override
    {
        return output && inputCount == 0U ? S_OK : E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE MapInputRectsToOutputRect(
        const D2D1_RECT_L*, const D2D1_RECT_L*, UINT32 inputCount,
        D2D1_RECT_L* output, D2D1_RECT_L* opaque) override
    {
        if (inputCount != 0U || !output || !opaque) return E_INVALIDARG;
        const auto left = static_cast<LONG>(std::floor(constants_.textureBounds[0]));
        const auto top = static_cast<LONG>(std::floor(constants_.textureBounds[1]));
        const auto right = static_cast<LONG>(std::ceil(constants_.textureBounds[2]));
        const auto bottom = static_cast<LONG>(std::ceil(constants_.textureBounds[3]));
        *output = D2D1::RectL(left, top, (std::max)(left, right),
            (std::max)(top, bottom));
        *opaque = *output;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE MapInvalidRect(UINT32,
        D2D1_RECT_L, D2D1_RECT_L*) const override
    {
        return E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE SetDrawInfo(ID2D1DrawInfo* info) override
    {
        if (!info) return E_INVALIDARG;
        drawInfo_ = info;
        auto hr = info->SetPixelShader(kShaderId);
        if (FAILED(hr)) return hr;
        // The texture graph remains small; request the precision mandated by
        // A1 and fail closed to the retained static background if unsupported.
        return info->SetOutputBuffer(
            D2D1_BUFFER_PRECISION_16BPC_FLOAT,
            D2D1_CHANNEL_DEPTH_4);
    }

private:
    std::atomic<ULONG> references_{1};
    Microsoft::WRL::ComPtr<ID2D1EffectContext> context_;
    Microsoft::WRL::ComPtr<ID2D1DrawInfo> drawInfo_;
    AlbumAmbientConstants constants_{};
};

HRESULT ensureRegistered(ID2D1Factory1* factory) noexcept
{
    static std::mutex mutex;
    static std::unordered_set<void*> registered;
    if (!factory) return E_POINTER;
    std::lock_guard lock(mutex);
    if (registered.contains(factory)) return S_OK;
    static constexpr wchar_t xml[] = LR"xml(<?xml version='1.0'?>
<Effect>
  <Property name='DisplayName' type='string' value='GODEN Album Flow A3.1'/>
  <Property name='Author' type='string' value='GODENplayer'/>
  <Property name='Category' type='string' value='Artistic'/>
  <Property name='Description' type='string' value='Bounded audio-reactive album ambient'/>
  <Inputs/>
  <Property name='Constants' type='blob'>
    <Property name='DisplayName' type='string' value='Ambient constants'/>
  </Property>
</Effect>)xml";
    const D2D1_PROPERTY_BINDING binding{
        kConstantsProperty, &EffectImpl::setConstants, &EffectImpl::getConstants};
    const auto hr = factory->RegisterEffectFromString(
        AlbumAmbientEffect::effectId, xml, &binding, 1U, &EffectImpl::make);
    if (SUCCEEDED(hr)) {
        registered.insert(factory);
        return S_OK;
    }
    return hr;
}

float linear(float value) noexcept
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.04045f ? value / 12.92f
        : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

std::array<float, 4> tint(const RgbColor& value) noexcept
{
    return {linear(value.red), linear(value.green), linear(value.blue), 0.0f};
}

std::array<float, 4> linearTint(const RgbColor& value) noexcept
{
    return {std::clamp(value.red, 0.0f, 1.0f),
        std::clamp(value.green, 0.0f, 1.0f),
        std::clamp(value.blue, 0.0f, 1.0f), 1.0f};
}

RgbColor mix(const RgbColor& left, const RgbColor& right, float amount) noexcept
{
    return {left.red + (right.red - left.red) * amount,
        left.green + (right.green - left.green) * amount,
        left.blue + (right.blue - left.blue) * amount};
}

} // namespace

HRESULT AlbumAmbientEffect::create(ID2D1DeviceContext* context,
    ID2D1Effect** effect) noexcept
{
    if (!context || !effect) return E_INVALIDARG;
    *effect = nullptr;
    Microsoft::WRL::ComPtr<ID2D1Factory> baseFactory;
    context->GetFactory(baseFactory.ReleaseAndGetAddressOf());
    Microsoft::WRL::ComPtr<ID2D1Factory1> factory;
    auto hr = baseFactory.As(&factory);
    if (FAILED(hr)) return hr;
    hr = ensureRegistered(factory.Get());
    if (FAILED(hr)) return hr;
    return context->CreateEffect(effectId, effect);
}

AlbumAmbientConstants AlbumAmbientEffect::constants(
    const D2D1_SIZE_F& viewportDip,
    const D2D1_SIZE_U& texturePixels,
    const D2D1_RECT_F& lyricsRectNormalized,
    const D2D1_RECT_F& tracksRectNormalized,
    const ArtPalette& palette, const AlbumAmbientPose& pose,
    bool reactiveMode, std::uint64_t artworkHash) noexcept
{
    AlbumAmbientConstants result{};
    const auto viewportWidth = std::max(1.0f, viewportDip.width);
    const auto viewportHeight = std::max(1.0f, viewportDip.height);
    const auto textureWidth = static_cast<float>((std::max)(1U, texturePixels.width));
    const auto textureHeight = static_cast<float>((std::max)(1U, texturePixels.height));
    result.geometry = {viewportWidth / viewportHeight, viewportHeight,
        textureWidth, textureHeight};
    result.levels = {std::clamp(pose.drive[0], 0.0f, 1.0f),
        std::clamp(pose.drive[1], 0.0f, 1.0f),
        std::clamp(pose.drive[2], 0.0f, 1.0f),
        std::clamp(pose.availability, 0.0f, 1.0f)};
    result.hits = {std::clamp(pose.transient[0], 0.0f, 1.0f),
        std::clamp(pose.transient[1], 0.0f, 1.0f),
        std::clamp(pose.transient[2], 0.0f, 1.0f), 0.0f};
    const auto seed = static_cast<float>((artworkHash & 0xffffffffULL) *
        (6.283185307179586 / 4294967296.0));
    result.phases = {pose.phase[0], pose.phase[1], pose.phase[2], seed};
    const auto normalized = [](const D2D1_RECT_F& rectangle) {
        return std::array<float, 4>{
            std::clamp(rectangle.left, 0.0f, 1.0f),
            std::clamp(rectangle.top, 0.0f, 1.0f),
            std::clamp(rectangle.right, 0.0f, 1.0f),
            std::clamp(rectangle.bottom, 0.0f, 1.0f)};
    };
    result.lyricsRect = normalized(lyricsRectNormalized);
    result.trackRect = normalized(tracksRectNormalized);
    const ArtPalette fallback{};
    const auto& usable = (palette.valid || palette.flowValid)
        ? palette : fallback;
    result.colorLow = usable.flowValid
        ? linearTint(usable.flowLow) : tint(usable.primary);
    result.colorMid = usable.flowValid
        ? linearTint(usable.flowMid) : tint(usable.secondary);
    result.colorHigh = usable.flowValid
        ? linearTint(usable.flowHigh) : tint(usable.highlight);
    if (usable.flowValid) {
        result.colorBase = linearTint(usable.flowBase);
    } else {
        const auto base = tint(usable.primary);
        result.colorBase = {base[0] * 0.025f, base[1] * 0.025f,
            base[2] * 0.025f, 1.0f};
    }
    result.controls = {std::clamp(pose.gain, 0.0f, 1.0f),
        reactiveMode ? 0.16f : 0.11f, 0.018f, 24.0f};
    result.textureBounds = {0.0f, 0.0f, textureWidth, textureHeight};
    return result;
}

HRESULT AlbumAmbientEffect::update(ID2D1Effect* effect,
    const AlbumAmbientConstants& constants) noexcept
{
    return effect ? effect->SetValueByName(kConstantsProperty,
        D2D1_PROPERTY_TYPE_BLOB,
        reinterpret_cast<const BYTE*>(&constants), sizeof(constants))
        : E_POINTER;
}

} // namespace nekorox::component
