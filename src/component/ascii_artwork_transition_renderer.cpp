#include "ascii_artwork_transition_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace nekorox::component {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::array<UINT32, 10> kAsciiRamp{
    L'.', L':', L'-', L'=', L'+', L'*', L'#', L'%', L'&', L'@',
};

float smoothStep(float value) noexcept
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}

std::uint64_t mix64(std::uint64_t value) noexcept
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

float unitHash(std::uint64_t value) noexcept
{
    return static_cast<float>((mix64(value) >> 40U) & 0xFFFFFFU) /
        static_cast<float>(0xFFFFFFU);
}

struct SampledPixel {
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float alpha = 0.0f;
    float luminance = 0.0f;
};

SampledPixel samplePixel(
    const ArtPixels& pixels,
    std::uint32_t column,
    std::uint32_t row,
    std::uint32_t columns,
    std::uint32_t rows) noexcept
{
    if (pixels.width == 0 || pixels.height == 0 || pixels.bgra.empty() ||
        columns == 0 || rows == 0) {
        return {};
    }
    const auto sourceX = std::min(
        pixels.width - 1U,
        static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(column) * 2U + 1U) * pixels.width /
            (static_cast<std::uint64_t>(columns) * 2U)));
    const auto sourceY = std::min(
        pixels.height - 1U,
        static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(row) * 2U + 1U) * pixels.height /
            (static_cast<std::uint64_t>(rows) * 2U)));
    const auto offset =
        (static_cast<std::size_t>(sourceY) * pixels.width + sourceX) * 4U;
    if (offset + 3U >= pixels.bgra.size()) return {};

    SampledPixel result;
    result.blue = static_cast<float>(pixels.bgra[offset]) / 255.0f;
    result.green = static_cast<float>(pixels.bgra[offset + 1U]) / 255.0f;
    result.red = static_cast<float>(pixels.bgra[offset + 2U]) / 255.0f;
    result.alpha = static_cast<float>(pixels.bgra[offset + 3U]) / 255.0f;
    if (result.alpha > 0.001f && result.alpha < 0.999f) {
        result.red = std::min(1.0f, result.red / result.alpha);
        result.green = std::min(1.0f, result.green / result.alpha);
        result.blue = std::min(1.0f, result.blue / result.alpha);
    }
    result.luminance = std::clamp(
        result.red * 0.2126f + result.green * 0.7152f +
            result.blue * 0.0722f,
        0.0f, 1.0f);
    return result;
}

D2D1_RECT_F containedRectangle(
    const ArtPixels& pixels,
    const D2D1_RECT_F& bounds) noexcept
{
    const auto availableWidth = std::max(1.0f, bounds.right - bounds.left);
    const auto availableHeight = std::max(1.0f, bounds.bottom - bounds.top);
    const auto sourceWidth = std::max(1.0f, static_cast<float>(pixels.width));
    const auto sourceHeight = std::max(1.0f, static_cast<float>(pixels.height));
    const auto scale = std::min(
        availableWidth / sourceWidth, availableHeight / sourceHeight);
    const auto width = sourceWidth * scale;
    const auto height = sourceHeight * scale;
    const auto left = bounds.left + (availableWidth - width) * 0.5f;
    const auto top = bounds.top + (availableHeight - height) * 0.5f;
    return D2D1::RectF(left, top, left + width, top + height);
}

} // namespace

bool AsciiArtworkTransitionRenderer::prepare(ID2D1RenderTarget* target)
{
    return target != nullptr && ensureResources(target);
}

bool AsciiArtworkTransitionRenderer::draw(
    ID2D1RenderTarget* target,
    ID2D1Bitmap* currentBitmap,
    const ArtPixels& pixels,
    const D2D1_RECT_F& bounds,
    float progress,
    std::uint64_t seed,
    float opacity)
{
    if (target == nullptr || currentBitmap == nullptr ||
        pixels.width == 0 || pixels.height == 0 ||
        pixels.bgra.empty() || !ensureResources(target)) {
        return false;
    }
    // This bounded draw (at most 480 cells) is part of the chosen cover
    // presentation, not optional background work. A recent click/scroll must
    // not permanently change a prepared reconstruction into a crossfade.

    progress = std::clamp(progress, 0.0f, 1.0f);
    opacity = std::clamp(opacity, 0.0f, 1.0f);
    if (progress <= 0.001f || opacity <= 0.001f) return true;

    const auto art = containedRectangle(pixels, bounds);
    const auto artWidth = std::max(1.0f, art.right - art.left);
    const auto artHeight = std::max(1.0f, art.bottom - art.top);
    auto rows = static_cast<std::uint32_t>(std::clamp(
        std::lround(artHeight / 9.0f), 14L, 24L));
    const auto rowHeight = artHeight / static_cast<float>(rows);
    const auto emSize = rowHeight * 0.92f;
    const auto designUnits = std::max<UINT16>(1U, fontMetrics_.designUnitsPerEm);
    const auto nominalAdvance =
        static_cast<float>(glyphMetrics_.back().advanceWidth) /
        static_cast<float>(designUnits) * emSize;
    auto columns = static_cast<std::uint32_t>(std::clamp(
        std::lround(artWidth / std::max(1.0f, nominalAdvance)), 14L, 42L));
    constexpr std::uint32_t minimumRows = 14;
    constexpr std::uint32_t minimumColumns = 14;
    constexpr std::uint32_t maximumCells = 480;
    const auto requestedCells = rows * columns;
    if (requestedCells > maximumCells) {
        const auto scale = std::sqrt(
            static_cast<float>(maximumCells) /
            static_cast<float>(requestedCells));
        rows = std::max(
            minimumRows,
            static_cast<std::uint32_t>(std::floor(rows * scale)));
        columns = std::max(
            minimumColumns,
            static_cast<std::uint32_t>(std::floor(columns * scale)));
        while (rows * columns > maximumCells) {
            if (columns > minimumColumns &&
                (columns >= rows || rows == minimumRows)) {
                --columns;
            } else if (rows > minimumRows) {
                --rows;
            } else {
                break;
            }
        }
    }
    const auto cellWidth = artWidth / static_cast<float>(columns);
    const auto cellHeight = artHeight / static_cast<float>(rows);
    const auto glyphEm = cellHeight * 0.92f;
    const auto baselineOffset = glyphEm * 0.80f;
    const auto bitmapSize = currentBitmap->GetSize();
    if (!ensurePreparedCells(pixels, columns, rows, seed)) return false;

    const auto previousAntialias = target->GetTextAntialiasMode();
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target->PushAxisAlignedClip(art, D2D1_ANTIALIAS_MODE_ALIASED);
    for (std::uint32_t row = 0; row < rows; ++row) {
        for (std::uint32_t column = 0; column < columns; ++column) {
            const auto index = static_cast<std::size_t>(row) * columns + column;
            const auto& sampled = preparedCells_[index];
            if (sampled.alpha <= 0.01f) continue;

            const auto localProgress = std::clamp(
                (progress - sampled.order * 0.47f) / 0.56f, 0.0f, 1.0f);
            if (localProgress <= 0.001f) continue;

            const auto cell = D2D1::RectF(
                art.left + static_cast<float>(column) * cellWidth - 0.12f,
                art.top + static_cast<float>(row) * cellHeight - 0.12f,
                art.left + static_cast<float>(column + 1U) * cellWidth + 0.12f,
                art.top + static_cast<float>(row + 1U) * cellHeight + 0.12f);
            const auto tileOpacity = smoothStep(
                (localProgress - 0.40f) / 0.52f);
            // Once a cell enters the reconstruction it must own that part of
            // the image. A persistent dark matte prevents the outgoing cover
            // from leaking through the glyph and partially revealed tile;
            // the clean incoming raster later replaces the matte naturally.
            const auto matteOpacity = smoothStep(localProgress / 0.18f);
            if (matteOpacity > 0.002f) {
                brush_->SetColor(D2D1::ColorF(
                    sampled.red * 0.06f,
                    sampled.green * 0.06f,
                    sampled.blue * 0.06f,
                    1.0f));
                brush_->SetOpacity(opacity * matteOpacity);
                target->FillRectangle(cell, brush_.Get());
            }

            const auto scrambleStep = std::min<std::size_t>(
                scrambleStepCount - 1U,
                static_cast<std::size_t>(
                    std::floor(localProgress * scrambleStepCount)));
            const auto glyph = localProgress < 0.58f
                ? sampled.scrambledGlyphs[scrambleStep]
                : sampled.targetGlyph;
            const auto glyphIndex = glyphIndices_[glyph];
            const auto advance =
                static_cast<float>(glyphMetrics_[glyph].advanceWidth) /
                static_cast<float>(designUnits) * glyphEm;
            const DWRITE_GLYPH_OFFSET glyphOffset{};
            const DWRITE_GLYPH_RUN run{
                fontFace_.Get(),
                glyphEm,
                1U,
                &glyphIndex,
                &advance,
                &glyphOffset,
                FALSE,
                0U,
            };

            const auto brightnessLift = sampled.luminance < 0.22f
                ? 0.22f - sampled.luminance : 0.0f;
            brush_->SetColor(D2D1::ColorF(
                std::clamp(sampled.red + brightnessLift, 0.0f, 1.0f),
                std::clamp(sampled.green + brightnessLift, 0.0f, 1.0f),
                std::clamp(sampled.blue + brightnessLift, 0.0f, 1.0f),
                1.0f));
            const auto glyphEnvelope =
                smoothStep(localProgress / 0.16f) *
                (1.0f - smoothStep((localProgress - 0.55f) / 0.38f));
            brush_->SetOpacity(
                opacity * glyphEnvelope * sampled.alpha *
                (0.50f + sampled.luminance * 0.50f));
            const auto originX = art.left +
                (static_cast<float>(column) + 0.5f) * cellWidth -
                advance * 0.5f;
            const auto originY = art.top +
                static_cast<float>(row) * cellHeight + baselineOffset;
            if (glyphEnvelope > 0.001f) {
                target->DrawGlyphRun(
                    D2D1::Point2F(originX, originY),
                    &run,
                    brush_.Get(),
                    DWRITE_MEASURING_MODE_NATURAL);
            }

            if (tileOpacity > 0.001f) {
                const auto source = D2D1::RectF(
                    static_cast<float>(column) * bitmapSize.width /
                        static_cast<float>(columns),
                    static_cast<float>(row) * bitmapSize.height /
                        static_cast<float>(rows),
                    static_cast<float>(column + 1U) * bitmapSize.width /
                        static_cast<float>(columns),
                    static_cast<float>(row + 1U) * bitmapSize.height /
                        static_cast<float>(rows));
                target->DrawBitmap(
                    currentBitmap,
                    cell,
                    opacity * tileOpacity,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                    &source);
            }
        }
    }
    brush_->SetOpacity(1.0f);
    target->PopAxisAlignedClip();
    target->SetTextAntialiasMode(previousAntialias);
    return true;
}

void AsciiArtworkTransitionRenderer::resetDeviceResources() noexcept
{
    brush_.Reset();
    brushOwner_ = nullptr;
}

bool AsciiArtworkTransitionRenderer::ensureResources(ID2D1RenderTarget* target)
{
    if (!fontFace_ && !createFontFace()) return false;
    if (brushOwner_ != target || !brush_) {
        brush_.Reset();
        if (FAILED(target->CreateSolidColorBrush(
                D2D1::ColorF(D2D1::ColorF::White),
                brush_.ReleaseAndGetAddressOf()))) {
            brushOwner_ = nullptr;
            return false;
        }
        brushOwner_ = target;
    }
    return true;
}

bool AsciiArtworkTransitionRenderer::createFontFace()
{
    IDWriteFactory* rawFactory = nullptr;
    if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&rawFactory)))) {
        return false;
    }
    writeFactory_.Attach(rawFactory);

    ComPtr<IDWriteFontCollection> collection;
    if (FAILED(writeFactory_->GetSystemFontCollection(
            collection.ReleaseAndGetAddressOf()))) {
        return false;
    }
    constexpr std::array families{
        L"Cascadia Mono", L"Cascadia Code", L"Consolas", L"Courier New",
    };
    for (const auto* familyName : families) {
        UINT32 familyIndex = 0;
        BOOL exists = FALSE;
        if (FAILED(collection->FindFamilyName(
                familyName, &familyIndex, &exists)) || !exists) {
            continue;
        }
        ComPtr<IDWriteFontFamily> family;
        ComPtr<IDWriteFont> font;
        if (FAILED(collection->GetFontFamily(
                familyIndex, family.ReleaseAndGetAddressOf())) ||
            FAILED(family->GetFirstMatchingFont(
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                font.ReleaseAndGetAddressOf())) ||
            FAILED(font->CreateFontFace(fontFace_.ReleaseAndGetAddressOf()))) {
            continue;
        }
        break;
    }
    if (!fontFace_) return false;
    fontFace_->GetMetrics(&fontMetrics_);
    if (FAILED(fontFace_->GetGlyphIndices(
            kAsciiRamp.data(),
            static_cast<UINT32>(kAsciiRamp.size()),
            glyphIndices_.data())) ||
        FAILED(fontFace_->GetDesignGlyphMetrics(
            glyphIndices_.data(),
            static_cast<UINT32>(glyphIndices_.size()),
            glyphMetrics_.data()))) {
        fontFace_.Reset();
        return false;
    }
    return true;
}

bool AsciiArtworkTransitionRenderer::ensurePreparedCells(
    const ArtPixels& pixels,
    std::uint32_t columns,
    std::uint32_t rows,
    std::uint64_t seed)
{
    if (columns == 0 || rows == 0) return false;
    const auto cellCount = static_cast<std::size_t>(columns) * rows;
    if (preparedContentHash_ == pixels.contentHash &&
        preparedSeed_ == seed &&
        preparedSourceWidth_ == pixels.width &&
        preparedSourceHeight_ == pixels.height &&
        preparedColumns_ == columns &&
        preparedRows_ == rows &&
        preparedCells_.size() == cellCount) {
        return true;
    }

    std::vector<PreparedCell> prepared(cellCount);
    for (std::uint32_t row = 0; row < rows; ++row) {
        for (std::uint32_t column = 0; column < columns; ++column) {
            const auto index = static_cast<std::size_t>(row) * columns + column;
            const auto source = samplePixel(
                pixels, column, row, columns, rows);
            auto& destination = prepared[index];
            destination.red = source.red;
            destination.green = source.green;
            destination.blue = source.blue;
            destination.alpha = source.alpha;
            destination.luminance = source.luminance;

            const auto random = unitHash(seed ^ index);
            const auto scan = columns > 1U
                ? static_cast<float>(column) /
                    static_cast<float>(columns - 1U)
                : 0.0f;
            const auto edgeFirst = 1.0f - source.luminance;
            destination.order = std::clamp(
                random * 0.54f + scan * 0.20f + edgeFirst * 0.12f,
                0.0f,
                0.86f);
            destination.targetGlyph = static_cast<std::uint8_t>(std::clamp(
                std::lround(std::pow(source.luminance, 0.72f) *
                    static_cast<float>(glyphCount - 1U)),
                0L,
                static_cast<long>(glyphCount - 1U)));
            for (std::size_t step = 0; step < scrambleStepCount; ++step) {
                destination.scrambledGlyphs[step] =
                    static_cast<std::uint8_t>(
                        mix64(seed ^ index ^
                            (static_cast<std::uint64_t>(step) *
                                0x9e3779b97f4a7c15ULL)) % glyphCount);
            }
        }
    }

    preparedCells_ = std::move(prepared);
    preparedContentHash_ = pixels.contentHash;
    preparedSeed_ = seed;
    preparedSourceWidth_ = pixels.width;
    preparedSourceHeight_ = pixels.height;
    preparedColumns_ = columns;
    preparedRows_ = rows;
    return true;
}

} // namespace nekorox::component
