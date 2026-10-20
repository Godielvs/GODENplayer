#include "scope_light_pass.h"

#include "scope_blur_ps.h"
#include "scope_composite_ps.h"
#include "scope_fullscreen_vs.h"
#include "scope_mask_ps.h"
#include "scope_mask_vs.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nekorox::component {
namespace {

constexpr float kKernelSigma = 1.6893945822f;
constexpr std::uint64_t kMaximumFxBytes = 16ULL * 1024ULL * 1024ULL;

float srgbToLinear(float value) noexcept
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.04045f
        ? value / 12.92f
        : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

struct alignas(16) PassConstants {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

} // namespace

bool ScopeLightPass::ensureDevice(ID3D11Device* device) noexcept
{
    if (device == nullptr) return false;
    if (device_.Get() == device && maskVertexShader_ && maskPixelShader_ &&
        fullscreenVertexShader_ && blurPixelShader_ && compositePixelShader_) {
        return true;
    }
    reset();
    device_ = device;

    const D3D11_INPUT_ELEMENT_DESC input[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
            D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
            D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(device_->CreateVertexShader(g_scopeMaskVs,
            sizeof(g_scopeMaskVs), nullptr,
            maskVertexShader_.ReleaseAndGetAddressOf())) ||
        FAILED(device_->CreatePixelShader(g_scopeMaskPs,
            sizeof(g_scopeMaskPs), nullptr,
            maskPixelShader_.ReleaseAndGetAddressOf())) ||
        FAILED(device_->CreateVertexShader(g_scopeFullscreenVs,
            sizeof(g_scopeFullscreenVs), nullptr,
            fullscreenVertexShader_.ReleaseAndGetAddressOf())) ||
        FAILED(device_->CreatePixelShader(g_scopeBlurPs,
            sizeof(g_scopeBlurPs), nullptr,
            blurPixelShader_.ReleaseAndGetAddressOf())) ||
        FAILED(device_->CreatePixelShader(g_scopeCompositePs,
            sizeof(g_scopeCompositePs), nullptr,
            compositePixelShader_.ReleaseAndGetAddressOf())) ||
        FAILED(device_->CreateInputLayout(input,
            static_cast<UINT>(std::size(input)),
            g_scopeMaskVs, sizeof(g_scopeMaskVs),
            maskInputLayout_.ReleaseAndGetAddressOf()))) {
        reset();
        return false;
    }

    D3D11_BUFFER_DESC vertexDescription{};
    vertexDescription.ByteWidth = static_cast<UINT>(sizeof(vertices_));
    vertexDescription.Usage = D3D11_USAGE_DYNAMIC;
    vertexDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vertexDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_BUFFER_DESC constantDescription{};
    constantDescription.ByteWidth = sizeof(PassConstants);
    constantDescription.Usage = D3D11_USAGE_DYNAMIC;
    constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&vertexDescription, nullptr,
            vertexBuffer_.ReleaseAndGetAddressOf())) ||
        FAILED(device_->CreateBuffer(&constantDescription, nullptr,
            constantBuffer_.ReleaseAndGetAddressOf()))) {
        reset();
        return false;
    }

    for (auto& slot : timing_) {
        D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        D3D11_QUERY_DESC timestamp{D3D11_QUERY_TIMESTAMP, 0};
        if (FAILED(device_->CreateQuery(&disjoint,
                slot.disjoint.ReleaseAndGetAddressOf())) ||
            FAILED(device_->CreateQuery(&timestamp,
                slot.begin.ReleaseAndGetAddressOf())) ||
            FAILED(device_->CreateQuery(&timestamp,
                slot.end.ReleaseAndGetAddressOf()))) {
            // GPU timing is diagnostic only. The effect remains available on
            // drivers that do not expose timestamp queries.
            releaseTimingQueries();
            break;
        }
    }

    D3D11_BLEND_DESC maximum{};
    maximum.RenderTarget[0].BlendEnable = TRUE;
    maximum.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    maximum.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    maximum.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MAX;
    maximum.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    maximum.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    maximum.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
    maximum.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN;
    D3D11_BLEND_DESC screen{};
    screen.RenderTarget[0].BlendEnable = TRUE;
    screen.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    screen.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_COLOR;
    screen.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    screen.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    screen.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    screen.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    screen.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED |
        D3D11_COLOR_WRITE_ENABLE_GREEN |
        D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (FAILED(device_->CreateBlendState(&maximum,
            maximumBlend_.ReleaseAndGetAddressOf())) ||
        FAILED(device_->CreateBlendState(&screen,
            screenBlend_.ReleaseAndGetAddressOf()))) {
        reset();
        return false;
    }

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.ScissorEnable = TRUE;
    rasterizer.DepthClipEnable = TRUE;
    if (FAILED(device_->CreateSamplerState(&sampler,
            linearSampler_.ReleaseAndGetAddressOf())) ||
        FAILED(device_->CreateRasterizerState(&rasterizer,
            rasterizer_.ReleaseAndGetAddressOf()))) {
        reset();
        return false;
    }
    return true;
}

bool ScopeLightPass::ensureTargets(IDXGISwapChain* swapChain,
    std::uint32_t width, std::uint32_t height) noexcept
{
    if (swapChain == nullptr || width == 0 || height == 0) return false;
    if (maskTexture_ && surfaceTarget_ &&
        targetWidth_ == width && targetHeight_ == height) {
        return true;
    }
    releaseTargets();

    UINT support = 0;
    constexpr auto format = DXGI_FORMAT_R16G16_FLOAT;
    if (FAILED(device_->CheckFormatSupport(format, &support)) ||
        (support & (D3D11_FORMAT_SUPPORT_TEXTURE2D |
                    D3D11_FORMAT_SUPPORT_RENDER_TARGET |
                    D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) !=
            (D3D11_FORMAT_SUPPORT_TEXTURE2D |
             D3D11_FORMAT_SUPPORT_RENDER_TARGET |
             D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) {
        return false;
    }

    const auto createTarget = [&](auto& texture, auto& target, auto& view) {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET |
            D3D11_BIND_SHADER_RESOURCE;
        return SUCCEEDED(device_->CreateTexture2D(&description, nullptr,
                texture.ReleaseAndGetAddressOf())) &&
            SUCCEEDED(device_->CreateRenderTargetView(texture.Get(), nullptr,
                target.ReleaseAndGetAddressOf())) &&
            SUCCEEDED(device_->CreateShaderResourceView(texture.Get(), nullptr,
                view.ReleaseAndGetAddressOf()));
    };
    if (!createTarget(maskTexture_, maskTarget_, maskView_) ||
        !createTarget(temporaryTexture_, temporaryTarget_, temporaryView_) ||
        !createTarget(nearTexture_, nearTarget_, nearView_) ||
        !createTarget(wideTexture_, wideTarget_, wideView_)) {
        releaseTargets();
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(backBuffer.ReleaseAndGetAddressOf())))) {
        releaseTargets();
        return false;
    }
    D3D11_TEXTURE2D_DESC backDescription{};
    backBuffer->GetDesc(&backDescription);
    D3D11_RENDER_TARGET_VIEW_DESC surfaceDescription{};
    surfaceDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    surfaceDescription.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    surfaceDescription.Texture2D.MipSlice = 0;
    surfaceSrgb_ = SUCCEEDED(device_->CreateRenderTargetView(backBuffer.Get(),
        &surfaceDescription, surfaceTarget_.ReleaseAndGetAddressOf()));
    if (!surfaceSrgb_ && FAILED(device_->CreateRenderTargetView(
            backBuffer.Get(), nullptr,
            surfaceTarget_.ReleaseAndGetAddressOf()))) {
        releaseTargets();
        return false;
    }
    targetWidth_ = width;
    targetHeight_ = height;
    surfaceWidth_ = backDescription.Width;
    surfaceHeight_ = backDescription.Height;
    maskRevision_ = 0;
    diagnostics_.resourceBytes = static_cast<std::uint64_t>(width) *
        static_cast<std::uint64_t>(height) * 4ULL * 4ULL;
    return true;
}

void ScopeLightPass::collectGpuTiming(ID3D11DeviceContext* context) noexcept
{
    if (context == nullptr) return;
    for (auto& slot : timing_) {
        if (!slot.pending || !slot.disjoint || !slot.begin || !slot.end)
            continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        UINT64 begin = 0;
        UINT64 end = 0;
        const auto disjointResult = context->GetData(slot.disjoint.Get(),
            &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        const auto beginResult = context->GetData(slot.begin.Get(),
            &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        const auto endResult = context->GetData(slot.end.Get(),
            &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (disjointResult == S_FALSE || beginResult == S_FALSE ||
            endResult == S_FALSE) continue;
        slot.pending = false;
        if (FAILED(disjointResult) || FAILED(beginResult) ||
            FAILED(endResult) || disjoint.Disjoint ||
            disjoint.Frequency == 0 || end < begin) continue;
        diagnostics_.gpuMicroseconds = static_cast<double>(end - begin) *
            1'000'000.0 / static_cast<double>(disjoint.Frequency);
        diagnostics_.gpuMeasured = true;
    }
}

std::size_t ScopeLightPass::beginGpuTiming(
    ID3D11DeviceContext* context) noexcept
{
    collectGpuTiming(context);
    if (context == nullptr) return timing_.size();
    for (std::size_t scanned = 0; scanned < timing_.size(); ++scanned) {
        const auto index = (nextTimingSlot_ + scanned) % timing_.size();
        auto& slot = timing_[index];
        if (slot.pending || !slot.disjoint || !slot.begin || !slot.end)
            continue;
        context->Begin(slot.disjoint.Get());
        context->End(slot.begin.Get());
        nextTimingSlot_ = (index + 1U) % timing_.size();
        return index;
    }
    return timing_.size();
}

void ScopeLightPass::endGpuTiming(
    ID3D11DeviceContext* context, std::size_t slotIndex) noexcept
{
    if (context == nullptr || slotIndex >= timing_.size()) return;
    auto& slot = timing_[slotIndex];
    context->End(slot.end.Get());
    context->End(slot.disjoint.Get());
    slot.pending = true;
}

void ScopeLightPass::appendSegment(float x1, float y1, float x2, float y2,
    float thickness, float core, float envelope) noexcept
{
    if (vertexCount_ + 6U > vertices_.size()) return;
    const auto dx = x2 - x1;
    const auto dy = y2 - y1;
    const auto length = std::sqrt(dx * dx + dy * dy);
    const auto nx = length > 1e-5f ? -dy / length * thickness * 0.5f : 0.0f;
    const auto ny = length > 1e-5f ? dx / length * thickness * 0.5f
        : thickness * 0.5f;
    const auto ndc = [&](float x, float y) {
        return MaskVertex{
            x / static_cast<float>(targetWidth_) * 2.0f - 1.0f,
            1.0f - y / static_cast<float>(targetHeight_) * 2.0f,
            core,
            envelope};
    };
    const auto a = ndc(x1 + nx, y1 + ny);
    const auto b = ndc(x2 + nx, y2 + ny);
    const auto c = ndc(x2 - nx, y2 - ny);
    const auto d = ndc(x1 - nx, y1 - ny);
    vertices_[vertexCount_++] = a;
    vertices_[vertexCount_++] = b;
    vertices_[vertexCount_++] = c;
    vertices_[vertexCount_++] = a;
    vertices_[vertexCount_++] = c;
    vertices_[vertexCount_++] = d;
}

bool ScopeLightPass::blur(ID3D11DeviceContext* context,
    ID3D11ShaderResourceView* source, ID3D11RenderTargetView* destination,
    float deltaX, float deltaY) noexcept
{
    PassConstants constants{deltaX, deltaY, 0.0f, 0.0f};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(constantBuffer_.Get(), 0,
            D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(constantBuffer_.Get(), 0);
    constexpr float clear[4]{};
    context->ClearRenderTargetView(destination, clear);
    context->OMSetRenderTargets(1, &destination, nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFU);
    context->IASetInputLayout(nullptr);
    context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(fullscreenVertexShader_.Get(), nullptr, 0);
    context->PSSetShader(blurPixelShader_.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, constantBuffer_.GetAddressOf());
    context->PSSetSamplers(0, 1, linearSampler_.GetAddressOf());
    context->PSSetShaderResources(0, 1, &source);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* empty = nullptr;
    context->PSSetShaderResources(0, 1, &empty);
    return true;
}

bool ScopeLightPass::buildMask(ID3D11DeviceContext* context,
    const std::array<float, kScopeSamples>& samples,
    const std::array<float, kScopeSamples>& minimum,
    const std::array<float, kScopeSamples>& maximum,
    float dpi, float scale, std::uint64_t visualRevision) noexcept
{
    vertexCount_ = 0;
    const auto amplitude = static_cast<float>(targetHeight_) * 0.47f;
    const auto center = static_cast<float>(targetHeight_) * 0.5f;
    const auto xFor = [&](std::size_t index) {
        return static_cast<float>(index) /
            static_cast<float>(kScopeSamples - 1U) *
            static_cast<float>(targetWidth_ - 1U);
    };
    const auto yFor = [&](float value) {
        return center - std::clamp(value, -1.0f, 1.0f) * amplitude;
    };
    const auto envelopeWidth = std::max(1.0f, scale * dpi / 96.0f);
    for (std::size_t index = 0; index < kScopeSamples; ++index) {
        const auto x = xFor(index);
        appendSegment(x, yFor(maximum[index]), x, yFor(minimum[index]),
            envelopeWidth, 0.0f, 1.0f);
    }
    const auto coreWidth = std::max(1.0f, 1.25f * dpi / 96.0f * scale);
    for (std::size_t index = 1; index < kScopeSamples; ++index) {
        appendSegment(xFor(index - 1U), yFor(samples[index - 1U]),
            xFor(index), yFor(samples[index]), coreWidth, 1.0f, 0.0f);
    }
    if (vertexCount_ == 0) return false;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(vertexBuffer_.Get(), 0,
            D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    std::memcpy(mapped.pData, vertices_.data(), vertexCount_ * sizeof(MaskVertex));
    context->Unmap(vertexBuffer_.Get(), 0);

    constexpr float clear[4]{};
    context->ClearRenderTargetView(maskTarget_.Get(), clear);
    context->OMSetRenderTargets(1, maskTarget_.GetAddressOf(), nullptr);
    context->OMSetBlendState(maximumBlend_.Get(), nullptr, 0xFFFFFFFFU);
    context->RSSetState(rasterizer_.Get());
    const D3D11_VIEWPORT viewport{0.0f, 0.0f,
        static_cast<float>(targetWidth_), static_cast<float>(targetHeight_),
        0.0f, 1.0f};
    const D3D11_RECT scissor{0, 0,
        static_cast<LONG>(targetWidth_), static_cast<LONG>(targetHeight_)};
    context->RSSetViewports(1, &viewport);
    context->RSSetScissorRects(1, &scissor);
    const UINT stride = sizeof(MaskVertex);
    const UINT offset = 0;
    ID3D11Buffer* buffer = vertexBuffer_.Get();
    context->IASetInputLayout(maskInputLayout_.Get());
    context->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(maskVertexShader_.Get(), nullptr, 0);
    context->PSSetShader(maskPixelShader_.Get(), nullptr, 0);
    context->Draw(static_cast<UINT>(vertexCount_), 0);

    const auto nearSigma = std::max(0.5f, 2.0f * dpi / 96.0f * scale);
    const auto wideSigma = std::max(0.5f, 8.0f * dpi / 96.0f * scale);
    const auto nearStep = nearSigma / kKernelSigma;
    const auto wideStep = wideSigma / kKernelSigma;
    if (!blur(context, maskView_.Get(), temporaryTarget_.Get(),
            nearStep / static_cast<float>(targetWidth_), 0.0f) ||
        !blur(context, temporaryView_.Get(), nearTarget_.Get(), 0.0f,
            nearStep / static_cast<float>(targetHeight_)) ||
        !blur(context, maskView_.Get(), temporaryTarget_.Get(),
            wideStep / static_cast<float>(targetWidth_), 0.0f) ||
        !blur(context, temporaryView_.Get(), wideTarget_.Get(), 0.0f,
            wideStep / static_cast<float>(targetHeight_))) {
        return false;
    }
    maskRevision_ = visualRevision;
    return true;
}

bool ScopeLightPass::composite(ID3D11DeviceContext* context,
    const D2D1_RECT_F& plotDip, float dpi,
    const D2D1_COLOR_F& accent) noexcept
{
    if (!surfaceTarget_ || surfaceWidth_ == 0 || surfaceHeight_ == 0)
        return false;
    const auto scale = std::max(1.0f, dpi) / 96.0f;
    const auto left = std::clamp<LONG>(
        static_cast<LONG>(std::lround(plotDip.left * scale)),
        0, static_cast<LONG>(surfaceWidth_));
    const auto top = std::clamp<LONG>(
        static_cast<LONG>(std::lround(plotDip.top * scale)),
        0, static_cast<LONG>(surfaceHeight_));
    const auto right = std::clamp<LONG>(
        static_cast<LONG>(std::lround(plotDip.right * scale)),
        left, static_cast<LONG>(surfaceWidth_));
    const auto bottom = std::clamp<LONG>(
        static_cast<LONG>(std::lround(plotDip.bottom * scale)),
        top, static_cast<LONG>(surfaceHeight_));
    if (right <= left || bottom <= top) return false;

    PassConstants constants{
        srgbToLinear(accent.r),
        srgbToLinear(accent.g),
        srgbToLinear(accent.b),
        surfaceSrgb_ ? 0.68f : -0.68f};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(constantBuffer_.Get(), 0,
            D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(constantBuffer_.Get(), 0);

    ID3D11RenderTargetView* surface = surfaceTarget_.Get();
    context->OMSetRenderTargets(1, &surface, nullptr);
    context->OMSetBlendState(screenBlend_.Get(), nullptr, 0xFFFFFFFFU);
    context->RSSetState(rasterizer_.Get());
    const D3D11_VIEWPORT viewport{
        static_cast<float>(left), static_cast<float>(top),
        static_cast<float>(right - left), static_cast<float>(bottom - top),
        0.0f, 1.0f};
    const D3D11_RECT scissor{left, top, right, bottom};
    context->RSSetViewports(1, &viewport);
    context->RSSetScissorRects(1, &scissor);
    context->IASetInputLayout(nullptr);
    context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(fullscreenVertexShader_.Get(), nullptr, 0);
    context->PSSetShader(compositePixelShader_.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, constantBuffer_.GetAddressOf());
    context->PSSetSamplers(0, 1, linearSampler_.GetAddressOf());
    ID3D11ShaderResourceView* views[]{
        maskView_.Get(), nearView_.Get(), wideView_.Get()};
    context->PSSetShaderResources(
        0, static_cast<UINT>(std::size(views)), views);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* empty[3]{};
    context->PSSetShaderResources(
        0, static_cast<UINT>(std::size(empty)), empty);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

bool ScopeLightPass::draw(ID3D11Device* device,
    ID3D11DeviceContext* context, IDXGISwapChain* swapChain,
    const D2D1_RECT_F& plotDip, float dpi,
    const std::array<float, kScopeSamples>& samples,
    const std::array<float, kScopeSamples>& minimum,
    const std::array<float, kScopeSamples>& maximum,
    std::uint64_t visualRevision, const D2D1_COLOR_F& accent) noexcept
{
    ++diagnostics_.submissions;
    if (context == nullptr || swapChain == nullptr || visualRevision == 0 ||
        plotDip.right <= plotDip.left || plotDip.bottom <= plotDip.top ||
        !ensureDevice(device)) {
        return false;
    }
    const auto pixelScale = std::max(1.0f, dpi) / 96.0f;
    const auto plotWidth = std::max(1.0f,
        (plotDip.right - plotDip.left) * pixelScale);
    const auto plotHeight = std::max(1.0f,
        (plotDip.bottom - plotDip.top) * pixelScale);
    const auto fxScale = std::min({
        0.5f,
        960.0f / plotWidth,
        540.0f / plotHeight});
    const auto width = std::max(1U,
        static_cast<std::uint32_t>(std::lround(plotWidth * fxScale)));
    const auto height = std::max(1U,
        static_cast<std::uint32_t>(std::lround(plotHeight * fxScale)));
    if (!ensureTargets(swapChain, width, height) ||
        diagnostics_.resourceBytes > kMaximumFxBytes) return false;
    const auto timingSlot = beginGpuTiming(context);
    bool success = true;
    if (maskRevision_ != visualRevision &&
        !buildMask(context, samples, minimum, maximum, dpi,
            fxScale, visualRevision)) {
        success = false;
    }
    if (success) success = composite(context, plotDip, dpi, accent);
    endGpuTiming(context, timingSlot);
    if (success) ++diagnostics_.successes;
    return success;
}

void ScopeLightPass::releaseTargets() noexcept
{
    surfaceTarget_.Reset();
    wideView_.Reset(); wideTarget_.Reset(); wideTexture_.Reset();
    nearView_.Reset(); nearTarget_.Reset(); nearTexture_.Reset();
    temporaryView_.Reset(); temporaryTarget_.Reset(); temporaryTexture_.Reset();
    maskView_.Reset(); maskTarget_.Reset(); maskTexture_.Reset();
    targetWidth_ = 0;
    targetHeight_ = 0;
    surfaceWidth_ = 0;
    surfaceHeight_ = 0;
    surfaceSrgb_ = false;
    maskRevision_ = 0;
    diagnostics_.resourceBytes = 0;
}

void ScopeLightPass::releaseTimingQueries() noexcept
{
    for (auto& slot : timing_) {
        slot.pending = false;
        slot.end.Reset();
        slot.begin.Reset();
        slot.disjoint.Reset();
    }
    nextTimingSlot_ = 0;
    diagnostics_.gpuMeasured = false;
    diagnostics_.gpuMicroseconds = 0.0;
}

void ScopeLightPass::resetSurface() noexcept
{
    releaseTargets();
}

void ScopeLightPass::reset() noexcept
{
    releaseTargets();
    releaseTimingQueries();
    rasterizer_.Reset();
    linearSampler_.Reset();
    screenBlend_.Reset();
    maximumBlend_.Reset();
    constantBuffer_.Reset();
    vertexBuffer_.Reset();
    maskInputLayout_.Reset();
    compositePixelShader_.Reset();
    blurPixelShader_.Reset();
    fullscreenVertexShader_.Reset();
    maskPixelShader_.Reset();
    maskVertexShader_.Reset();
    device_.Reset();
    vertexCount_ = 0;
}

} // namespace nekorox::component
