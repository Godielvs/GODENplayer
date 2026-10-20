#include "artwork_transition_renderer.h"

#include "glyph_flow_service.h"
#include "ui_design_system.h"

#include <algorithm>
#include <cmath>

namespace nekorox::component {
namespace {

std::size_t bitmapBytes(const std::shared_ptr<const ArtPixels>& pixels) noexcept
{
    return pixels ? pixels->bgra.size() : 0U;
}

bool validPixelBuffer(const ArtPixels& pixels) noexcept
{
    const auto expected = artworkBgraBytes(pixels.width, pixels.height);
    return expected != 0 && pixels.bgra.size() == expected &&
        expected <= kArtworkRendererGpuBudgetBytes;
}

} // namespace

bool ArtworkTransitionRenderer::update(
    ID2D1RenderTarget* target,
    float dpi,
    std::wstring_view identity,
    const std::shared_ptr<const ArtPixels>& pixels)
{
    if (target == nullptr || identity.empty() || !pixels ||
        !validPixelBuffer(*pixels)) {
        if (pixels && !pixels->bgra.empty()) {
            ++diagnostics_.gpuBudgetFallbacks;
        }
        return false;
    }
    if (!ensureDeviceResources(target, dpi)) return false;
    const auto sameContent = contentHash_ != 0 &&
        contentHash_ == pixels->contentHash;
    const auto sameDimensions = currentPixels_ &&
        currentPixels_->width == pixels->width &&
        currentPixels_->height == pixels->height;
    if (identity_ == identity && contentHash_ == pixels->contentHash &&
        sameDimensions && currentBitmap_) {
        return false;
    }
    if (sameContent && currentBitmap_ && !sameDimensions) {
        // A DPI/size bucket changed while the same cover stayed visible.
        // Replace its raster without restarting OR settling a cover change
        // already being presented. The broker can publish the master first
        // and its fitted variant on the very next frame.
        if (!validPixelBuffer(*pixels)) {
            ++diagnostics_.gpuBudgetFallbacks;
            return false;
        }
        Microsoft::WRL::ComPtr<ID2D1Bitmap> candidate;
        if (!createBitmap(target, dpi_, *pixels, candidate)) return false;
        currentPixels_ = pixels;
        currentBitmap_ = std::move(candidate);
        identity_.assign(identity);
        if (!artworkBudgetCanAdd(bitmapBytes(currentPixels_),
                bitmapBytes(previousPixels_), kArtworkRendererGpuBudgetBytes)) {
            previousPixels_.reset();
            previousBitmap_.Reset();
            ++diagnostics_.gpuBudgetFallbacks;
        }
        diagnostics_.gpuBytes = bitmapBytes(currentPixels_) + bitmapBytes(previousPixels_);
        diagnostics_.peakGpuBytes = std::max(diagnostics_.peakGpuBytes, diagnostics_.gpuBytes);
        return false;
    }
    if (sameContent && currentBitmap_) {
        // Tracks within one album often resolve to the exact same image.
        // Preserve the clean presentation instead of reconstructing an
        // unchanged bitmap only because playback identity advanced.
        identity_.assign(identity);
        currentPixels_ = pixels;
        return false;
    }

    const auto candidateBytes = pixels->bgra.size();
    if (candidateBytes > kArtworkRendererGpuBudgetBytes) {
        ++diagnostics_.gpuBudgetFallbacks;
        return false;
    }
    auto gpuBudgetFallbacks = diagnostics_.gpuBudgetFallbacks;
    if (!artworkBudgetCanAdd(bitmapBytes(currentPixels_), candidateBytes,
            kArtworkRendererGpuBudgetBytes)) {
        // The incoming image itself is valid, but retaining two full GPU
        // bitmaps would exceed the renderer budget. Settle the old image
        // before allocating the replacement; this chooses a direct reveal
        // over an unbounded transition allocation.
        currentBitmap_.Reset();
        currentPixels_.reset();
        previousBitmap_.Reset();
        previousPixels_.reset();
        ++gpuBudgetFallbacks;
    }

    Microsoft::WRL::ComPtr<ID2D1Bitmap> candidate;
    if (!createBitmap(target, dpi_, *pixels, candidate)) return false;
    const auto reconstructionPrepared = asciiRenderer_.prepare(target);

    GlyphFlowService::instance().observeArtwork(identity, *pixels);
    previousPixels_ = std::move(currentPixels_);
    previousBitmap_ = std::move(currentBitmap_);
    currentPixels_ = pixels;
    currentBitmap_ = std::move(candidate);
    identity_.assign(identity);
    contentHash_ = pixels->contentHash;
    // Start at presentation time, including the first image shown by a newly
    // opened surface. A transient no-track callback must not make the next
    // artwork appear as an unanimated first frame.
    presentationClock_.reset();
    finalFramePending_ = true;
    reconstructionPrepared_ = reconstructionPrepared;
    presentation_ = Presentation::undecided;
    const auto nextGeneration = diagnostics_.generation + 1U;
    diagnostics_ = {};
    diagnostics_.generation = nextGeneration;
    presentationClock_.reset(nextGeneration);
    glyphGeneration_ = GlyphFlowService::instance().generation();
    awaitingPresentation_ = false;
    lastAcceptedSeconds_ = 0;
    acceptedIntervalCount_ = 0;
    diagnostics_.reconstructionPrepared = reconstructionPrepared_;
    diagnostics_.gpuBudgetFallbacks = gpuBudgetFallbacks;
    diagnostics_.gpuBytes = bitmapBytes(currentPixels_) +
        bitmapBytes(previousPixels_);
    diagnostics_.peakGpuBytes = diagnostics_.gpuBytes;
    return true;
}

bool ArtworkTransitionRenderer::draw(
    ID2D1RenderTarget* target,
    const D2D1_RECT_F& rectangle,
    MotionMode mode,
    float opacity)
{
    if (target == nullptr || !currentPixels_) return false;
    if (!ensureDeviceResources(target, dpi_) || !currentBitmap_) return false;

    settlingFrameDrawn_ = false;
    awaitingPresentation_ = true;

    const auto clampedOpacity = std::clamp(opacity, 0.0f, 1.0f);
    const auto durationMilliseconds = static_cast<double>(
        UiDesignSystemV1::durationMs(MotionToken::artwork, mode));
    const auto elapsedMilliseconds = finalFramePending_
        ? presentationClock_.elapsedMilliseconds(qpcSeconds()) : durationMilliseconds;
    const auto transitioning = durationMilliseconds > 0.0 &&
        elapsedMilliseconds < durationMilliseconds;
    if (transitioning) {
        awaitingPresentation_ = true;
        const auto frameStart = qpcSeconds();
        ++diagnostics_.presentedFrames;
        const auto progress = static_cast<float>(std::clamp(
            elapsedMilliseconds / durationMilliseconds, 0.0, 1.0));
        const auto smooth = [](float value) noexcept {
            value = std::clamp(value, 0.0f, 1.0f);
            return value * value * (3.0f - 2.0f * value);
        };
        if (presentation_ == Presentation::undecided) {
            presentation_ = mode == MotionMode::normal &&
                    reconstructionPrepared_
                ? Presentation::reconstruction
                : Presentation::crossfade;
        }
        if (presentation_ == Presentation::reconstruction) {
            const auto alignedCurrent = containedRectangle(
                target, currentBitmap_.Get(), rectangle);
            // Cells in the semantic renderer replace the outgoing image as
            // they wake, so the old cover remains useful behind cells that
            // have not joined the reconstruction yet. Retire the global old
            // layer only once virtually every cell has begun its hand-off;
            // this avoids both late ghosting and a cover -> black -> cover
            // interval.
            const auto previousOpacity = 1.0f -
                smooth((progress - 0.42f) / 0.18f);
            // Crossfade from the semantic reconstruction into the exact
            // raster over a broad interval. This also removes all tiles
            // before the retained frame, including behind transparent art.
            const auto detailResolve = smooth((progress - 0.48f) / 0.44f);
            drawContained(target, previousBitmap_.Get(), rectangle,
                clampedOpacity * previousOpacity);
            const auto reconstructed = asciiRenderer_.draw(
                target,
                currentBitmap_.Get(),
                *currentPixels_,
                alignedCurrent,
                progress,
                stableSeed(identity_, contentHash_),
                clampedOpacity * (1.0f - detailResolve));
            if (reconstructed) {
                ++diagnostics_.reconstructionFrames;
            } else {
                presentation_ = Presentation::crossfade;
                ++diagnostics_.fadeFallbackFrames;
            }
            // Resolve detail progressively instead of replacing the tiled
            // reconstruction with a fully sharp image in the final 5%.
            // The clean raster reaches full opacity before settle, so the
            // last animated frame and the retained frame are identical.
            const auto cleanOpacity = reconstructed
                ? detailResolve
                : smooth(progress);
            drawContained(target, currentBitmap_.Get(), rectangle,
                clampedOpacity * cleanOpacity);
        } else {
            if (mode == MotionMode::normal) {
                ++diagnostics_.fadeFallbackFrames;
            }
            const auto eased = smooth(progress);
            drawContained(
                target,
                previousBitmap_.Get(),
                rectangle,
                clampedOpacity * (1.0f - eased));
            drawContained(target, currentBitmap_.Get(), rectangle,
                clampedOpacity * eased);
        }
        diagnostics_.maximumFrameMilliseconds = std::max(
            diagnostics_.maximumFrameMilliseconds,
            (qpcSeconds() - frameStart) * 1000.0);
    } else {
        previousBitmap_.Reset();
        previousPixels_.reset();
        diagnostics_.gpuBytes = bitmapBytes(currentPixels_);
        drawContained(target, currentBitmap_.Get(), rectangle, clampedOpacity);
        settlingFrameDrawn_ = true;
    }
    return true;
}

void ArtworkTransitionRenderer::presented(std::uint64_t generation, bool accepted) noexcept
{
    if (!awaitingPresentation_ || generation == 0 || generation != diagnostics_.generation) return;
    awaitingPresentation_ = false;
    if (!accepted) { ++diagnostics_.rejectedFrames; return; }
    ++diagnostics_.acceptedFrames;
    const auto now = qpcSeconds();
    if (presentationClock_.accept(generation, true, now))
        GlyphFlowService::instance().presented(glyphGeneration_, true, now);
    if (settlingFrameDrawn_) finalFramePending_ = false;
    if (lastAcceptedSeconds_ > 0) {
        const auto interval = (now - lastAcceptedSeconds_) * 1000;
        acceptedIntervals_[acceptedIntervalCount_ % acceptedIntervals_.size()] = interval;
        ++acceptedIntervalCount_;
        diagnostics_.maximumAcceptedIntervalMilliseconds = std::max(
            diagnostics_.maximumAcceptedIntervalMilliseconds, interval);
    }
    lastAcceptedSeconds_ = now;
}

double ArtworkTransitionRenderer::acceptedIntervalP95Milliseconds() const noexcept
{
    auto intervals = acceptedIntervals_;
    const auto count = std::min<std::size_t>(acceptedIntervalCount_, intervals.size());
    if (!count) return 0;
    std::sort(intervals.begin(), intervals.begin() + count);
    return intervals[static_cast<std::size_t>(std::ceil(count * .95)) - 1];
}

bool ArtworkTransitionRenderer::active(MotionMode mode) const noexcept
{
    if (!currentPixels_) return false;
    if (finalFramePending_) return true;
    if (!presentationClock_.started()) return false;
    const auto durationMilliseconds = static_cast<double>(
        UiDesignSystemV1::durationMs(MotionToken::artwork, mode));
    return finalFramePending_ ||
        (durationMilliseconds > 0.0 &&
            presentationClock_.elapsedMilliseconds(qpcSeconds()) <
                durationMilliseconds);
}

void ArtworkTransitionRenderer::clear() noexcept
{
    discardDeviceResources();
    identity_.clear();
    contentHash_ = 0;
    presentationClock_.reset();
    finalFramePending_ = false;
    reconstructionPrepared_ = false;
    presentation_ = Presentation::undecided;
    diagnostics_ = {};
    awaitingPresentation_ = false;
    lastAcceptedSeconds_ = 0;
    acceptedIntervalCount_ = 0;
    currentPixels_.reset();
    previousPixels_.reset();
    diagnostics_.gpuBytes = bitmapBytes(currentPixels_);
}

void ArtworkTransitionRenderer::settle() noexcept
{
    previousBitmap_.Reset();
    previousPixels_.reset();
    presentationClock_.reset();
    finalFramePending_ = false;
    presentation_ = Presentation::undecided;
}

double ArtworkTransitionRenderer::qpcSeconds() noexcept
{
    static const double inverseFrequency = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return frequency.QuadPart > 0
            ? 1.0 / static_cast<double>(frequency.QuadPart)
            : 0.0;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) * inverseFrequency;
}

void ArtworkTransitionRenderer::discardDeviceResources() noexcept
{
    owner_ = nullptr;
    currentBitmap_.Reset();
    previousBitmap_.Reset();
    asciiRenderer_.resetDeviceResources();
}

bool ArtworkTransitionRenderer::createBitmap(
    ID2D1RenderTarget* target,
    float dpi,
    const ArtPixels& pixels,
    Microsoft::WRL::ComPtr<ID2D1Bitmap>& output)
{
    if (target == nullptr || !validPixelBuffer(pixels)) {
        return false;
    }
    const auto properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM,
            D2D1_ALPHA_MODE_PREMULTIPLIED),
        dpi,
        dpi);
    return SUCCEEDED(target->CreateBitmap(
        D2D1::SizeU(pixels.width, pixels.height),
        pixels.bgra.data(),
        pixels.width * 4U,
        properties,
        output.ReleaseAndGetAddressOf()));
}

void ArtworkTransitionRenderer::drawContained(
    ID2D1RenderTarget* target,
    ID2D1Bitmap* bitmap,
    const D2D1_RECT_F& rectangle,
    float opacity)
{
    if (target == nullptr || bitmap == nullptr || opacity <= 0.0f) return;
    const auto destination = containedRectangle(target, bitmap, rectangle);
    target->DrawBitmap(
        bitmap,
        destination,
        std::clamp(opacity, 0.0f, 1.0f),
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

D2D1_RECT_F ArtworkTransitionRenderer::containedRectangle(
    ID2D1RenderTarget* target,
    ID2D1Bitmap* bitmap,
    const D2D1_RECT_F& rectangle) noexcept
{
    if (target == nullptr || bitmap == nullptr) return rectangle;
    float dpiX = 96.0f;
    float dpiY = 96.0f;
    target->GetDpi(&dpiX, &dpiY);
    const auto physicalScaleX = std::max(1.0f, dpiX) / 96.0f;
    const auto physicalScaleY = std::max(1.0f, dpiY) / 96.0f;
    const auto left = std::lround(rectangle.left * physicalScaleX);
    const auto top = std::lround(rectangle.top * physicalScaleY);
    const auto right = std::lround(rectangle.right * physicalScaleX);
    const auto bottom = std::lround(rectangle.bottom * physicalScaleY);
    const auto availableWidth = std::max<long>(1, right - left);
    const auto availableHeight = std::max<long>(1, bottom - top);
    const auto pixels = bitmap->GetPixelSize();
    const auto fit = std::min(
        static_cast<double>(availableWidth) /
            std::max<UINT32>(1, pixels.width),
        static_cast<double>(availableHeight) /
            std::max<UINT32>(1, pixels.height));
    const auto width = std::max<long>(
        1, std::lround(pixels.width * fit));
    const auto height = std::max<long>(
        1, std::lround(pixels.height * fit));
    const auto destinationLeft = left + (availableWidth - width) / 2;
    const auto destinationTop = top + (availableHeight - height) / 2;
    return D2D1::RectF(
        static_cast<float>(destinationLeft) / physicalScaleX,
        static_cast<float>(destinationTop) / physicalScaleY,
        static_cast<float>(destinationLeft + width) / physicalScaleX,
        static_cast<float>(destinationTop + height) / physicalScaleY);
}

std::uint64_t ArtworkTransitionRenderer::stableSeed(
    std::wstring_view identity,
    std::uint64_t contentHash) noexcept
{
    auto hash = contentHash ^ 1469598103934665603ULL;
    for (const auto character : identity) {
        hash ^= static_cast<std::uint64_t>(character);
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool ArtworkTransitionRenderer::ensureDeviceResources(
    ID2D1RenderTarget* target,
    float dpi)
{
    if (target == nullptr) return false;
    if (owner_ != target) {
        owner_ = target;
        currentBitmap_.Reset();
        previousBitmap_.Reset();
    }
    dpi_ = std::max(1.0f, dpi);
    if (!artworkBudgetCanAdd(bitmapBytes(currentPixels_),
            bitmapBytes(previousPixels_), kArtworkRendererGpuBudgetBytes)) {
        previousPixels_.reset();
        previousBitmap_.Reset();
        ++diagnostics_.gpuBudgetFallbacks;
    }
    if (currentPixels_ && !currentBitmap_ &&
        !createBitmap(target, dpi_, *currentPixels_, currentBitmap_)) {
        return false;
    }
    if (previousPixels_ && !previousBitmap_) {
        createBitmap(target, dpi_, *previousPixels_, previousBitmap_);
    }
    return true;
}

} // namespace nekorox::component
