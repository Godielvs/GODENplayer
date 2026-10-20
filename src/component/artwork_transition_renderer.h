#pragma once

#include "art_cache.h"
#include "ascii_artwork_transition_renderer.h"
#include "ui_preferences.h"
#include "nekorox/presented_animation_clock.h"

#include <d2d1.h>
#include <wrl/client.h>

#include <cstdint>
#include <array>
#include <memory>
#include <string>
#include <string_view>

namespace nekorox::component {

struct ArtworkTransitionDiagnostics {
    std::uint64_t generation = 0;
    std::uint64_t presentedFrames = 0;
    std::uint64_t reconstructionFrames = 0;
    std::uint64_t fadeFallbackFrames = 0;
    double maximumFrameMilliseconds = 0.0;
    bool reconstructionPrepared = false;
    std::size_t gpuBytes = 0;
    std::size_t peakGpuBytes = 0;
    std::uint64_t gpuBudgetFallbacks = 0;
    std::uint64_t acceptedFrames = 0;
    std::uint64_t rejectedFrames = 0;
    double maximumAcceptedIntervalMilliseconds = 0;
};

// Retains the last decoded artwork until its successor and reconstruction
// resources are ready, then presents one locked transition path on a QPC
// clock. Device resources are disposable; decoded pixels remain available
// across target recreation.
class ArtworkTransitionRenderer final {
public:
    bool update(
        ID2D1RenderTarget* target,
        float dpi,
        std::wstring_view identity,
        const std::shared_ptr<const ArtPixels>& pixels);

    bool draw(
        ID2D1RenderTarget* target,
        const D2D1_RECT_F& rectangle,
        MotionMode mode,
        float opacity = 1.0f);

    [[nodiscard]] bool active(MotionMode mode) const noexcept;
    // Called after the owning surface attempts presentation, never at draw.
    void presented(std::uint64_t generation, bool accepted) noexcept;
    [[nodiscard]] std::uint64_t pendingPresentation() const noexcept { return awaitingPresentation_ ? diagnostics_.generation : 0; }
    [[nodiscard]] double acceptedIntervalP95Milliseconds() const noexcept;
    [[nodiscard]] const ArtworkTransitionDiagnostics& diagnostics() const noexcept
    {
        return diagnostics_;
    }

    void clear() noexcept;
    void settle() noexcept;
    void discardDeviceResources() noexcept;

    [[nodiscard]] bool ready() const noexcept
    {
        return currentPixels_ && !currentPixels_->bgra.empty();
    }

    [[nodiscard]] ID2D1Bitmap* currentBitmap() const noexcept
    {
        return currentBitmap_.Get();
    }

    [[nodiscard]] std::uint64_t currentContentHash() const noexcept { return contentHash_; }

private:
    enum class Presentation : std::uint8_t {
        undecided,
        reconstruction,
        crossfade,
    };

    static bool createBitmap(
        ID2D1RenderTarget* target,
        float dpi,
        const ArtPixels& pixels,
        Microsoft::WRL::ComPtr<ID2D1Bitmap>& output);
    static void drawContained(
        ID2D1RenderTarget* target,
        ID2D1Bitmap* bitmap,
        const D2D1_RECT_F& rectangle,
        float opacity);
    static D2D1_RECT_F containedRectangle(
        ID2D1RenderTarget* target,
        ID2D1Bitmap* bitmap,
        const D2D1_RECT_F& rectangle) noexcept;
    static std::uint64_t stableSeed(
        std::wstring_view identity,
        std::uint64_t contentHash) noexcept;
    static double qpcSeconds() noexcept;
    bool ensureDeviceResources(ID2D1RenderTarget* target, float dpi);

    ID2D1RenderTarget* owner_ = nullptr;
    float dpi_ = 96.0f;
    std::wstring identity_;
    std::uint64_t contentHash_ = 0;
    ::nekorox::PresentedAnimationClock presentationClock_;
    std::uint64_t glyphGeneration_ = 0;
    bool settlingFrameDrawn_ = false;
    bool finalFramePending_ = false;
    bool reconstructionPrepared_ = false;
    Presentation presentation_ = Presentation::undecided;
    bool awaitingPresentation_ = false;
    double lastAcceptedSeconds_ = 0;
    std::array<double, 128> acceptedIntervals_{};
    unsigned acceptedIntervalCount_ = 0;
    ArtworkTransitionDiagnostics diagnostics_;
    std::shared_ptr<const ArtPixels> currentPixels_;
    std::shared_ptr<const ArtPixels> previousPixels_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> currentBitmap_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> previousBitmap_;
    AsciiArtworkTransitionRenderer asciiRenderer_;
};

} // namespace nekorox::component
