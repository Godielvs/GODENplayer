#include "glyph_flow_renderer.h"

#include "interaction_latency_guard.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace nekorox::component {
namespace {

using Microsoft::WRL::ComPtr;

bool createPathGeometry(
    ID2D1Factory* factory,
    std::span<const D2D1_POINT_2F> points,
    ComPtr<ID2D1Geometry>& output)
{
    if (factory == nullptr || points.size() < 3) return false;
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(path.GetAddressOf())) ||
        FAILED(path->Open(sink.GetAddressOf()))) {
        return false;
    }
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    sink->BeginFigure(points.front(), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLines(points.data() + 1, static_cast<UINT32>(points.size() - 1));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return false;
    output = path;
    return true;
}

bool createXGeometry(
    ID2D1Factory* factory,
    ComPtr<ID2D1Geometry>& output)
{
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(path.GetAddressOf())) ||
        FAILED(path->Open(sink.GetAddressOf()))) {
        return false;
    }
    const std::array first{
        D2D1::Point2F(-0.50f, -0.38f),
        D2D1::Point2F(-0.38f, -0.50f),
        D2D1::Point2F(0.50f, 0.38f),
        D2D1::Point2F(0.38f, 0.50f),
    };
    const std::array second{
        D2D1::Point2F(-0.50f, 0.38f),
        D2D1::Point2F(-0.38f, 0.50f),
        D2D1::Point2F(0.50f, -0.38f),
        D2D1::Point2F(0.38f, -0.50f),
    };
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    sink->BeginFigure(first.front(), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLines(first.data() + 1, static_cast<UINT32>(first.size() - 1));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->BeginFigure(second.front(), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLines(second.data() + 1, static_cast<UINT32>(second.size() - 1));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return false;
    output = path;
    return true;
}

float ephemeralEnvelope(float progress) noexcept
{
    progress = std::clamp(progress, 0.0f, 1.0f);
    constexpr float pi = 3.14159265358979323846f;
    const auto sine = std::sin(progress * pi);
    return sine * sine;
}

} // namespace

bool GlyphFlowRenderer::draw(
    ID2D1RenderTarget* target,
    const D2D1_RECT_F& bounds,
    MotionMode mode,
    const GlyphFlowDrawStyle& style)
{
    pendingGeneration_ = 0;
    auto& service = GlyphFlowService::instance();
    if (target == nullptr ||
        bounds.right <= bounds.left ||
        bounds.bottom <= bounds.top ||
        mode == MotionMode::off ||
        !service.active(mode) ||
        !ensureResources(target)) {
        return false;
    }
    if (InteractionLatencyGuard::instance().shouldYieldRender()) return false;

    GlyphFlowFrame frame;
    if (!service.sample(mode, cells_, frame) ||
        frame.width == 0 || frame.height == 0) {
        return false;
    }

    pendingGeneration_ = frame.generation;
    const auto envelope = ephemeralEnvelope(frame.progress);
    if (envelope <= 0.0001f) return true;
    const auto cellWidth =
        (bounds.right - bounds.left) / static_cast<float>(frame.width);
    const auto cellHeight =
        (bounds.bottom - bounds.top) / static_cast<float>(frame.height);
    const auto baseGlyphSize = std::clamp(
        std::min(cellWidth, cellHeight) * 0.46f, 1.15f, 4.5f);
    const auto travel = mode == MotionMode::normal
        ? std::clamp(style.maximumTravelDip, 0.0f, 1.0f)
        : 0.0f;
    const auto strength = std::clamp(style.strength, 0.0f, 0.24f);

    brush_->SetColor(style.color);
    D2D1_MATRIX_3X2_F previous{};
    target->GetTransform(&previous);
    target->PushAxisAlignedClip(
        bounds, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (std::size_t index = 0;
         index < static_cast<std::size_t>(frame.width) * frame.height;
         ++index) {
        const auto& cell = cells_[index];
        const auto opacity =
            strength * style.color.a * envelope * cell.opacity *
            (0.28f + 0.72f * cell.sourceLuminance);
        if (opacity < 0.002f) continue;

        const auto centerX =
            bounds.left + (static_cast<float>(cell.column) + 0.5f) *
                cellWidth +
            cell.offsetXNormalized * travel;
        const auto centerY =
            bounds.top + (static_cast<float>(cell.row) + 0.5f) *
                cellHeight +
            cell.offsetYNormalized * travel;
        const auto glyphSize =
            baseGlyphSize * (0.72f + 0.28f * cell.sourceLuminance);
        const auto glyphIndex =
            static_cast<std::size_t>(cell.glyphIndex) % atlas_.size();
        if (!atlas_[glyphIndex]) continue;

        brush_->SetOpacity(opacity);
        target->SetTransform(
            D2D1::Matrix3x2F::Scale(glyphSize, glyphSize) *
            D2D1::Matrix3x2F::Translation(centerX, centerY) *
            previous);
        target->FillGeometry(atlas_[glyphIndex].Get(), brush_.Get());
    }
    target->SetTransform(previous);
    brush_->SetOpacity(1.0f);
    target->PopAxisAlignedClip();
    return true;
}

void GlyphFlowRenderer::presented(bool accepted) noexcept
{
    const auto ticket = std::exchange(pendingGeneration_, 0);
    if (ticket) GlyphFlowService::instance().presented(ticket, accepted);
}

bool GlyphFlowRenderer::drawOverlay(
    ID2D1RenderTarget* target,
    const D2D1_RECT_F& bounds,
    std::span<const GlyphFlowOverlayCell> cells,
    const D2D1_COLOR_F& color)
{
    if (target == nullptr || cells.empty() ||
        bounds.right <= bounds.left || bounds.bottom <= bounds.top ||
        !ensureResources(target)) {
        return false;
    }

    const auto width = bounds.right - bounds.left;
    const auto height = bounds.bottom - bounds.top;
    const auto previousAntialias = target->GetAntialiasMode();
    D2D1_MATRIX_3X2_F previous{};
    target->GetTransform(&previous);
    target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    target->PushAxisAlignedClip(
        bounds, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    brush_->SetColor(color);
    for (const auto& cell : cells) {
        const auto opacity = std::clamp(
            cell.opacity * color.a, 0.0f, 1.0f);
        const auto glyphSize = std::clamp(cell.sizeDip, 0.75f, 5.0f);
        const auto glyphIndex =
            static_cast<std::size_t>(cell.glyphIndex) % atlas_.size();
        if (opacity < 0.002f || !atlas_[glyphIndex]) continue;

        const auto x = bounds.left +
            std::clamp(cell.xNormalized, 0.0f, 1.0f) * width;
        const auto y = bounds.top +
            std::clamp(cell.yNormalized, 0.0f, 1.0f) * height;
        brush_->SetOpacity(opacity);
        target->SetTransform(
            D2D1::Matrix3x2F::Scale(glyphSize, glyphSize) *
            D2D1::Matrix3x2F::Translation(x, y) * previous);
        target->FillGeometry(atlas_[glyphIndex].Get(), brush_.Get());
    }
    target->SetTransform(previous);
    brush_->SetOpacity(1.0f);
    target->PopAxisAlignedClip();
    target->SetAntialiasMode(previousAntialias);
    return true;
}

void GlyphFlowRenderer::reset() noexcept
{
    pendingGeneration_ = 0;
    brush_.Reset();
    for (auto& glyph : atlas_) glyph.Reset();
    brushOwner_ = nullptr;
    atlasOwner_ = nullptr;
}

bool GlyphFlowRenderer::ensureResources(ID2D1RenderTarget* target)
{
    ComPtr<ID2D1Factory> factory;
    target->GetFactory(factory.GetAddressOf());
    if (!factory) return false;
    if (atlasOwner_ != factory.Get()) {
        for (auto& glyph : atlas_) glyph.Reset();
        atlasOwner_ = nullptr;
        if (!createAtlas(factory.Get())) return false;
        atlasOwner_ = factory.Get();
    }
    if (brushOwner_ != target || !brush_) {
        brush_.Reset();
        if (FAILED(target->CreateSolidColorBrush(
                D2D1::ColorF(D2D1::ColorF::White),
                brush_.GetAddressOf()))) {
            brushOwner_ = nullptr;
            return false;
        }
        brushOwner_ = target;
    }
    return true;
}

bool GlyphFlowRenderer::createAtlas(ID2D1Factory* factory)
{
    ComPtr<ID2D1EllipseGeometry> dot;
    if (FAILED(factory->CreateEllipseGeometry(
            D2D1::Ellipse(D2D1::Point2F(), 0.18f, 0.18f),
            dot.GetAddressOf()))) {
        return false;
    }
    atlas_[0] = dot;

    ComPtr<ID2D1RectangleGeometry> horizontal;
    if (FAILED(factory->CreateRectangleGeometry(
            D2D1::RectF(-0.50f, -0.09f, 0.50f, 0.09f),
            horizontal.GetAddressOf()))) {
        return false;
    }
    atlas_[1] = horizontal;

    ComPtr<ID2D1RectangleGeometry> vertical;
    if (FAILED(factory->CreateRectangleGeometry(
            D2D1::RectF(-0.09f, -0.50f, 0.09f, 0.50f),
            vertical.GetAddressOf()))) {
        return false;
    }
    atlas_[2] = vertical;

    const std::array diamond{
        D2D1::Point2F(0.0f, -0.50f),
        D2D1::Point2F(0.50f, 0.0f),
        D2D1::Point2F(0.0f, 0.50f),
        D2D1::Point2F(-0.50f, 0.0f),
    };
    if (!createPathGeometry(factory, diamond, atlas_[3])) return false;

    const std::array plus{
        D2D1::Point2F(-0.13f, -0.50f),
        D2D1::Point2F(0.13f, -0.50f),
        D2D1::Point2F(0.13f, -0.13f),
        D2D1::Point2F(0.50f, -0.13f),
        D2D1::Point2F(0.50f, 0.13f),
        D2D1::Point2F(0.13f, 0.13f),
        D2D1::Point2F(0.13f, 0.50f),
        D2D1::Point2F(-0.13f, 0.50f),
        D2D1::Point2F(-0.13f, 0.13f),
        D2D1::Point2F(-0.50f, 0.13f),
        D2D1::Point2F(-0.50f, -0.13f),
        D2D1::Point2F(-0.13f, -0.13f),
    };
    if (!createPathGeometry(factory, plus, atlas_[4])) return false;
    return createXGeometry(factory, atlas_[5]);
}

} // namespace nekorox::component
