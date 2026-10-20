#include "crt_offscreen.h"

#include "crt_beam_ps.h"
#include "crt_beam_vs.h"
#include "crt_composite_ps.h"
#include "crt_decay_ps.h"
#include "crt_fullscreen_vs.h"

#include "nekorox/visual_sample_guard.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>

namespace nekorox::tools {
namespace {

constexpr float kClearPhosphor[4]{0.0f, 0.0f, 0.0f, 0.0f};
constexpr float kClearOutput[4]{0.0f, 0.0f, 0.0f, 1.0f};
constexpr float kBlendFactor[4]{0.0f, 0.0f, 0.0f, 0.0f};

} // namespace

CrtOffscreenRenderer::~CrtOffscreenRenderer()
{
    shutdown();
}

std::wstring CrtOffscreenRenderer::hresultMessage(
    const wchar_t* operation,
    HRESULT hr)
{
    std::wostringstream stream;
    stream << operation << L" failed (0x" << std::hex
           << static_cast<unsigned long>(hr) << L")";
    return stream.str();
}

bool CrtOffscreenRenderer::initialize(
    std::uint32_t width,
    std::uint32_t height,
    CrtXyOrientation orientation,
    bool forceWarp,
    std::wstring& error)
{
    shutdown();
    error.clear();
    if (width == 0 || height == 0) {
        error = L"Output dimensions must be non-zero";
        return false;
    }
    outputWidth_ = width;
    outputHeight_ = height;
    orientation_ = orientation;
    if (!createDevice(forceWarp, error) ||
        !createShaders(error) ||
        !createStates(error) ||
        !createSizeResources(error)) {
        shutdown();
        return false;
    }
    stats_.outputWidth = outputWidth_;
    stats_.outputHeight = outputHeight_;
    stats_.renderWidth = renderWidth_;
    stats_.renderHeight = renderHeight_;
    return reset(error);
}

bool CrtOffscreenRenderer::createDevice(
    bool forceWarp,
    std::wstring& error)
{
    constexpr std::array<D3D_FEATURE_LEVEL, 4> levels{{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    }};
    D3D_FEATURE_LEVEL actual{};
    const auto create = [&](D3D_DRIVER_TYPE type) {
        return D3D11CreateDevice(
            nullptr,
            type,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels.data(),
            static_cast<UINT>(levels.size()),
            D3D11_SDK_VERSION,
            device_.ReleaseAndGetAddressOf(),
            &actual,
            context_.ReleaseAndGetAddressOf());
    };

    auto result = forceWarp
        ? create(D3D_DRIVER_TYPE_WARP)
        : create(D3D_DRIVER_TYPE_HARDWARE);
    if (FAILED(result) && !forceWarp) {
        result = create(D3D_DRIVER_TYPE_WARP);
    }
    if (FAILED(result)) {
        error = hresultMessage(L"D3D11CreateDevice", result);
        return false;
    }
    stats_.softwareRasterizer =
        device_->GetCreationFlags() == 0 && forceWarp;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    if (SUCCEEDED(device_.As(&dxgiDevice)) &&
        SUCCEEDED(dxgiDevice->GetAdapter(adapter.ReleaseAndGetAddressOf())) &&
        SUCCEEDED(adapter->GetDesc(&description))) {
        stats_.adapterName = description.Description;
        stats_.softwareRasterizer =
            (description.VendorId == 0x1414 && description.DeviceId == 0x008c) ||
            forceWarp;
    } else {
        stats_.adapterName = stats_.softwareRasterizer
            ? L"Microsoft Basic Render Driver (WARP)"
            : L"D3D11 hardware adapter";
    }
    return true;
}

bool CrtOffscreenRenderer::createShaders(std::wstring& error)
{
    auto result = device_->CreateVertexShader(
        g_crtFullscreenVs,
        sizeof(g_crtFullscreenVs),
        nullptr,
        fullscreenVertexShader_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        error = hresultMessage(L"CreateVertexShader(FullscreenVS)", result);
        return false;
    }
    result = device_->CreateVertexShader(
        g_crtBeamVs,
        sizeof(g_crtBeamVs),
        nullptr,
        beamVertexShader_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        error = hresultMessage(L"CreateVertexShader(BeamVS)", result);
        return false;
    }
    result = device_->CreatePixelShader(
        g_crtDecayPs,
        sizeof(g_crtDecayPs),
        nullptr,
        decayPixelShader_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        error = hresultMessage(L"CreatePixelShader(DecayPS)", result);
        return false;
    }
    result = device_->CreatePixelShader(
        g_crtBeamPs,
        sizeof(g_crtBeamPs),
        nullptr,
        beamPixelShader_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        error = hresultMessage(L"CreatePixelShader(BeamPS)", result);
        return false;
    }
    result = device_->CreatePixelShader(
        g_crtCompositePs,
        sizeof(g_crtCompositePs),
        nullptr,
        compositePixelShader_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        error = hresultMessage(L"CreatePixelShader(CompositePS)", result);
        return false;
    }

    constexpr std::array<D3D11_INPUT_ELEMENT_DESC, 4> elements{{
        {"BEAM_START", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
            D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"BEAM_END", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
            D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"BEAM_CHARGE", 0, DXGI_FORMAT_R32_FLOAT, 0, 16,
            D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"BEAM_AGE", 0, DXGI_FORMAT_R32_FLOAT, 0, 20,
            D3D11_INPUT_PER_INSTANCE_DATA, 1},
    }};
    result = device_->CreateInputLayout(
        elements.data(),
        static_cast<UINT>(elements.size()),
        g_crtBeamVs,
        sizeof(g_crtBeamVs),
        beamInputLayout_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        error = hresultMessage(L"CreateInputLayout(Beam)", result);
        return false;
    }

    D3D11_BUFFER_DESC segmentDescription{};
    segmentDescription.ByteWidth = static_cast<UINT>(
        sizeof(CrtBeamSegment) * kMaximumSegments);
    segmentDescription.Usage = D3D11_USAGE_DYNAMIC;
    segmentDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    segmentDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    result = device_->CreateBuffer(
        &segmentDescription,
        nullptr,
        segmentBuffer_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        error = hresultMessage(L"CreateBuffer(segments)", result);
        return false;
    }

    D3D11_BUFFER_DESC constantDescription{};
    constantDescription.ByteWidth = sizeof(CrtXyShaderConstants);
    constantDescription.Usage = D3D11_USAGE_DYNAMIC;
    constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    result = device_->CreateBuffer(
        &constantDescription,
        nullptr,
        constantsBuffer_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        error = hresultMessage(L"CreateBuffer(constants)", result);
        return false;
    }
    return true;
}

bool CrtOffscreenRenderer::createStates(std::wstring& error)
{
    D3D11_SAMPLER_DESC samplerDescription{};
    samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
    auto result = device_->CreateSamplerState(
        &samplerDescription,
        linearSampler_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        error = hresultMessage(L"CreateSamplerState", result);
        return false;
    }

    D3D11_BLEND_DESC blendDescription{};
    blendDescription.RenderTarget[0].BlendEnable = TRUE;
    blendDescription.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blendDescription.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    blendDescription.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDescription.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDescription.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blendDescription.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDescription.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN;
    result = device_->CreateBlendState(
        &blendDescription,
        additiveBlend_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        error = hresultMessage(L"CreateBlendState(additive)", result);
        return false;
    }

    blendDescription = {};
    blendDescription.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_ALL;
    result = device_->CreateBlendState(
        &blendDescription,
        opaqueBlend_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        error = hresultMessage(L"CreateBlendState(opaque)", result);
        return false;
    }

    D3D11_RASTERIZER_DESC rasterizerDescription{};
    rasterizerDescription.FillMode = D3D11_FILL_SOLID;
    rasterizerDescription.CullMode = D3D11_CULL_NONE;
    rasterizerDescription.DepthClipEnable = TRUE;
    result = device_->CreateRasterizerState(
        &rasterizerDescription,
        rasterizer_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        error = hresultMessage(L"CreateRasterizerState", result);
        return false;
    }
    return true;
}

bool CrtOffscreenRenderer::createSizeResources(std::wstring& error)
{
    renderWidth_ = crtXyRenderWidth(
        outputWidth_, outputHeight_, stats_.softwareRasterizer);
    renderHeight_ = crtXyRenderHeight(
        outputWidth_, outputHeight_, stats_.softwareRasterizer);

    D3D11_TEXTURE2D_DESC phosphorDescription{};
    phosphorDescription.Width = renderWidth_;
    phosphorDescription.Height = renderHeight_;
    phosphorDescription.MipLevels = 1;
    phosphorDescription.ArraySize = 1;
    phosphorDescription.Format = DXGI_FORMAT_R16G16_FLOAT;
    phosphorDescription.SampleDesc.Count = 1;
    phosphorDescription.Usage = D3D11_USAGE_DEFAULT;
    phosphorDescription.BindFlags =
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (std::size_t index = 0; index < phosphorTextures_.size(); ++index) {
        auto result = device_->CreateTexture2D(
            &phosphorDescription,
            nullptr,
            phosphorTextures_[index].ReleaseAndGetAddressOf());
        if (FAILED(result)) {
            error = hresultMessage(L"CreateTexture2D(phosphor)", result);
            return false;
        }
        result = device_->CreateRenderTargetView(
            phosphorTextures_[index].Get(),
            nullptr,
            phosphorTargets_[index].ReleaseAndGetAddressOf());
        if (FAILED(result)) {
            error = hresultMessage(L"CreateRenderTargetView(phosphor)", result);
            return false;
        }
        result = device_->CreateShaderResourceView(
            phosphorTextures_[index].Get(),
            nullptr,
            phosphorViews_[index].ReleaseAndGetAddressOf());
        if (FAILED(result)) {
            error = hresultMessage(L"CreateShaderResourceView(phosphor)", result);
            return false;
        }
    }

    D3D11_TEXTURE2D_DESC outputDescription{};
    outputDescription.Width = outputWidth_;
    outputDescription.Height = outputHeight_;
    outputDescription.MipLevels = 1;
    outputDescription.ArraySize = 1;
    outputDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    outputDescription.SampleDesc.Count = 1;
    outputDescription.Usage = D3D11_USAGE_DEFAULT;
    outputDescription.BindFlags = D3D11_BIND_RENDER_TARGET;
    auto result = device_->CreateTexture2D(
        &outputDescription,
        nullptr,
        outputTexture_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        error = hresultMessage(L"CreateTexture2D(output)", result);
        return false;
    }
    result = device_->CreateRenderTargetView(
        outputTexture_.Get(),
        nullptr,
        outputTarget_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        error = hresultMessage(L"CreateRenderTargetView(output)", result);
        return false;
    }

    auto stagingDescription = outputDescription;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.BindFlags = 0;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    D3D11_QUERY_DESC queryDescription{};
    queryDescription.Query = D3D11_QUERY_EVENT;
    for (auto& slot : readback_) {
        result = device_->CreateTexture2D(
            &stagingDescription,
            nullptr,
            slot.staging.ReleaseAndGetAddressOf());
        if (FAILED(result)) {
            error = hresultMessage(L"CreateTexture2D(staging)", result);
            return false;
        }
        result = device_->CreateQuery(
            &queryDescription,
            slot.completion.ReleaseAndGetAddressOf());
        if (FAILED(result)) {
            error = hresultMessage(L"CreateQuery(readback)", result);
            return false;
        }
    }
    return true;
}

bool CrtOffscreenRenderer::reset(std::wstring& error)
{
    error.clear();
    if (!device_ || !context_ || readbackCount_ != 0) {
        error = readbackCount_ != 0
            ? L"Cannot reset while readbacks are pending"
            : L"Renderer is not initialized";
        return false;
    }
    context_->ClearState();
    for (auto& target : phosphorTargets_) {
        context_->ClearRenderTargetView(target.Get(), kClearPhosphor);
    }
    context_->ClearRenderTargetView(outputTarget_.Get(), kClearOutput);
    context_->Flush();
    for (auto& slot : readback_) {
        slot.pending = false;
        slot.frameIndex = 0;
    }
    phosphorIndex_ = 0;
    clearPhosphor_ = false;
    nextFrameIndex_ = 0;
    readbackHead_ = 0;
    readbackCount_ = 0;
    stats_.submittedFrames = 0;
    stats_.submittedSegments = 0;
    stats_.readbackWaits = 0;
    return true;
}

bool CrtOffscreenRenderer::updateConstants(
    double deltaSeconds,
    std::wstring& error)
{
    const auto constants = makeCrtXyShaderConstants(
        renderWidth_, renderHeight_, outputWidth_, outputHeight_,
        deltaSeconds, orientation_);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto result = context_->Map(
        constantsBuffer_.Get(),
        0,
        D3D11_MAP_WRITE_DISCARD,
        0,
        &mapped);
    if (FAILED(result)) {
        error = hresultMessage(L"Map(constants)", result);
        return false;
    }
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context_->Unmap(constantsBuffer_.Get(), 0);
    return true;
}

bool CrtOffscreenRenderer::decay(std::wstring& error)
{
    const auto next = phosphorIndex_ ^ 1U;
    D3D11_VIEWPORT viewport{
        0.0f,
        0.0f,
        static_cast<float>(renderWidth_),
        static_cast<float>(renderHeight_),
        0.0f,
        1.0f};
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(rasterizer_.Get());
    context_->OMSetRenderTargets(
        1, phosphorTargets_[next].GetAddressOf(), nullptr);
    context_->OMSetBlendState(
        opaqueBlend_.Get(), kBlendFactor, 0xffffffffU);
    if (clearPhosphor_) {
        context_->ClearRenderTargetView(
            phosphorTargets_[next].Get(), kClearPhosphor);
        clearPhosphor_ = false;
    } else {
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(fullscreenVertexShader_.Get(), nullptr, 0);
        context_->VSSetConstantBuffers(
            0, 1, constantsBuffer_.GetAddressOf());
        context_->PSSetShader(decayPixelShader_.Get(), nullptr, 0);
        context_->PSSetConstantBuffers(
            0, 1, constantsBuffer_.GetAddressOf());
        context_->PSSetShaderResources(
            0, 1, phosphorViews_[phosphorIndex_].GetAddressOf());
        context_->PSSetSamplers(0, 1, linearSampler_.GetAddressOf());
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* empty = nullptr;
        context_->PSSetShaderResources(0, 1, &empty);
    }
    phosphorIndex_ = next;
    const auto removed = device_->GetDeviceRemovedReason();
    if (FAILED(removed)) {
        error = hresultMessage(L"D3D11 decay", removed);
        return false;
    }
    return true;
}

bool CrtOffscreenRenderer::beam(
    std::span<const CrtBeamSegment> segments,
    std::wstring& error)
{
    if (segments.empty()) return true;
    if (segments.size() > kMaximumSegments) {
        error = L"Frame contains more than 65536 CRT beam segments";
        return false;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto result = context_->Map(
        segmentBuffer_.Get(),
        0,
        D3D11_MAP_WRITE_DISCARD,
        0,
        &mapped);
    if (FAILED(result)) {
        error = hresultMessage(L"Map(segments)", result);
        return false;
    }
    auto* output = static_cast<CrtBeamSegment*>(mapped.pData);
    for (std::size_t index = 0; index < segments.size(); ++index) {
        output[index] = segments[index];
        output[index].x0 = boundedVisualSample(output[index].x0);
        output[index].y0 = boundedVisualSample(output[index].y0);
        output[index].x1 = boundedVisualSample(output[index].x1);
        output[index].y1 = boundedVisualSample(output[index].y1);
    }
    context_->Unmap(segmentBuffer_.Get(), 0);

    const UINT stride = sizeof(CrtBeamSegment);
    const UINT offset = 0;
    ID3D11Buffer* vertexBuffer = segmentBuffer_.Get();
    context_->IASetInputLayout(beamInputLayout_.Get());
    context_->IASetVertexBuffers(
        0, 1, &vertexBuffer, &stride, &offset);
    context_->IASetPrimitiveTopology(
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->VSSetShader(beamVertexShader_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(
        0, 1, constantsBuffer_.GetAddressOf());
    context_->PSSetShader(beamPixelShader_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(
        0, 1, constantsBuffer_.GetAddressOf());
    context_->OMSetBlendState(
        additiveBlend_.Get(), kBlendFactor, 0xffffffffU);
    context_->DrawInstanced(
        4, static_cast<UINT>(segments.size()), 0, 0);
    return true;
}

bool CrtOffscreenRenderer::composite(std::wstring& error)
{
    D3D11_VIEWPORT viewport{
        0.0f,
        0.0f,
        static_cast<float>(outputWidth_),
        static_cast<float>(outputHeight_),
        0.0f,
        1.0f};
    context_->RSSetViewports(1, &viewport);
    context_->OMSetRenderTargets(1, outputTarget_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(outputTarget_.Get(), kClearOutput);
    context_->OMSetBlendState(
        opaqueBlend_.Get(), kBlendFactor, 0xffffffffU);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(fullscreenVertexShader_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(
        0, 1, constantsBuffer_.GetAddressOf());
    context_->PSSetShader(compositePixelShader_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(
        0, 1, constantsBuffer_.GetAddressOf());
    context_->PSSetShaderResources(
        0, 1, phosphorViews_[phosphorIndex_].GetAddressOf());
    context_->PSSetSamplers(0, 1, linearSampler_.GetAddressOf());
    context_->Draw(3, 0);
    ID3D11ShaderResourceView* empty = nullptr;
    context_->PSSetShaderResources(0, 1, &empty);
    const auto removed = device_->GetDeviceRemovedReason();
    if (FAILED(removed)) {
        error = hresultMessage(L"D3D11 composite", removed);
        return false;
    }
    return true;
}

bool CrtOffscreenRenderer::enqueueReadback(std::wstring& error)
{
    if (readbackCount_ >= readback_.size()) {
        error = L"Readback ring overflow";
        return false;
    }
    const auto slotIndex =
        (readbackHead_ + readbackCount_) % readback_.size();
    auto& slot = readback_[slotIndex];
    slot.frameIndex = nextFrameIndex_++;
    slot.pending = true;
    context_->CopyResource(slot.staging.Get(), outputTexture_.Get());
    context_->End(slot.completion.Get());
    ++readbackCount_;
    return true;
}

bool CrtOffscreenRenderer::consumeOldest(
    CrtReadbackFrame& completed,
    bool wait,
    bool& available,
    std::wstring& error)
{
    available = false;
    if (readbackCount_ == 0) return true;
    auto& slot = readback_[readbackHead_];
    HRESULT result = context_->GetData(
        slot.completion.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (result == S_FALSE && !wait) return true;
    if (result == S_FALSE) {
        ++stats_.readbackWaits;
        context_->Flush();
        do {
            SwitchToThread();
            result = context_->GetData(
                slot.completion.Get(), nullptr, 0,
                D3D11_ASYNC_GETDATA_DONOTFLUSH);
        } while (result == S_FALSE);
    }
    if (FAILED(result)) {
        error = hresultMessage(L"GetData(readback)", result);
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = context_->Map(
        slot.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) {
        error = hresultMessage(L"Map(readback)", result);
        return false;
    }
    const auto rowBytes = static_cast<std::size_t>(outputWidth_) * 4U;
    try {
        completed.index = slot.frameIndex;
        completed.bgra.resize(
            rowBytes * static_cast<std::size_t>(outputHeight_));
        const auto* source = static_cast<const std::uint8_t*>(mapped.pData);
        for (std::uint32_t row = 0; row < outputHeight_; ++row) {
            std::memcpy(
                completed.bgra.data() +
                    static_cast<std::size_t>(row) * rowBytes,
                source + static_cast<std::size_t>(row) * mapped.RowPitch,
                rowBytes);
        }
    } catch (...) {
        context_->Unmap(slot.staging.Get(), 0);
        error = L"Unable to allocate readback frame";
        return false;
    }
    context_->Unmap(slot.staging.Get(), 0);
    slot.pending = false;
    slot.frameIndex = 0;
    readbackHead_ = (readbackHead_ + 1U) % readback_.size();
    --readbackCount_;
    available = true;
    return true;
}

bool CrtOffscreenRenderer::render(
    std::span<const CrtBeamSegment> segments,
    double deltaSeconds,
    CrtReadbackFrame& completed,
    bool& hasCompleted,
    std::wstring& error)
{
    hasCompleted = false;
    error.clear();
    if (!device_ || !context_) {
        error = L"Renderer is not initialized";
        return false;
    }
    if (readbackCount_ == readback_.size() &&
        !consumeOldest(completed, true, hasCompleted, error)) {
        return false;
    }
    if (!updateConstants(deltaSeconds, error) ||
        !decay(error) ||
        !beam(segments, error) ||
        !composite(error) ||
        !enqueueReadback(error)) {
        return false;
    }
    ++stats_.submittedFrames;
    stats_.submittedSegments += segments.size();
    return true;
}

bool CrtOffscreenRenderer::drain(
    CrtReadbackFrame& completed,
    bool& hasCompleted,
    std::wstring& error)
{
    return consumeOldest(completed, true, hasCompleted, error);
}

void CrtOffscreenRenderer::shutdown() noexcept
{
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
    for (auto& slot : readback_) {
        slot.completion.Reset();
        slot.staging.Reset();
        slot = {};
    }
    rasterizer_.Reset();
    opaqueBlend_.Reset();
    additiveBlend_.Reset();
    linearSampler_.Reset();
    constantsBuffer_.Reset();
    segmentBuffer_.Reset();
    beamInputLayout_.Reset();
    compositePixelShader_.Reset();
    beamPixelShader_.Reset();
    decayPixelShader_.Reset();
    beamVertexShader_.Reset();
    fullscreenVertexShader_.Reset();
    outputTarget_.Reset();
    outputTexture_.Reset();
    for (auto& view : phosphorViews_) view.Reset();
    for (auto& target : phosphorTargets_) target.Reset();
    for (auto& texture : phosphorTextures_) texture.Reset();
    context_.Reset();
    device_.Reset();
    outputWidth_ = 0;
    outputHeight_ = 0;
    renderWidth_ = 0;
    renderHeight_ = 0;
    phosphorIndex_ = 0;
    clearPhosphor_ = true;
    nextFrameIndex_ = 0;
    readbackHead_ = 0;
    readbackCount_ = 0;
    stats_ = {};
}

} // namespace nekorox::tools
