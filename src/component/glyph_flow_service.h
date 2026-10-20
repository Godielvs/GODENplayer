#pragma once

#include "art_cache.h"
#include "ui_preferences.h"

#include "nekorox/glyph_flow.h"
#include "nekorox/presented_animation_clock.h"

#include <windows.h>

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nekorox::component {

inline constexpr std::uint32_t kGlyphFlowFieldWidth = 12;
inline constexpr std::uint32_t kGlyphFlowFieldHeight = 6;
inline constexpr std::size_t kGlyphFlowFieldCellCount =
    static_cast<std::size_t>(kGlyphFlowFieldWidth) *
    kGlyphFlowFieldHeight;

struct GlyphFlowFrame {
    std::uint64_t generation = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float progress = 0.0f;
    bool active = false;
};

class GlyphFlowService final {
public:
    static GlyphFlowService& instance();

    // Returns true only when this artwork starts a new shared generation.
    bool observeArtwork(
        std::wstring_view identity,
        const ArtPixels& pixels) noexcept;

    // Only acceptance of a frame containing this generation starts its clock.
    // Duplicate and stale acknowledgements cannot restart a visible flow.
    bool presented(std::uint64_t generation, bool accepted, double seconds = -1.0) noexcept;

    bool sample(
        MotionMode mode,
        std::span<::nekorox::GlyphFlowCellV1> output,
        GlyphFlowFrame& frame) const noexcept;

    [[nodiscard]] GlyphFlowFrame transition(MotionMode mode) const noexcept;

    [[nodiscard]] bool active(MotionMode mode) const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;

    // Subscribers receive one posted message when a generation starts.
    // The service owns no timer and never drives subsequent frames.
    void subscribe(HWND window, UINT message);
    void unsubscribe(HWND window) noexcept;

private:
    GlyphFlowService() = default;

    static std::array<float, kGlyphFlowFieldCellCount> deriveLuminance(
        const ArtPixels& pixels) noexcept;
    static std::uint64_t stableIdentitySeed(
        std::wstring_view identity) noexcept;
    static double qpcSeconds() noexcept;
    static double durationMilliseconds(MotionMode mode) noexcept;

    mutable std::mutex mutex_;
    ::nekorox::GlyphFlowV1 flow_;
    std::wstring identity_;
    std::uint64_t artworkContentHash_ = 0;
    std::wstring presentedIdentity_;
    std::uint64_t presentedContentHash_ = 0;
    std::uint64_t generation_ = 0;
    ::nekorox::PresentedAnimationClock presentationClock_;
    std::atomic<std::uint64_t> publishedGeneration_{0};
    std::atomic<double> publishedStartSeconds_{0.0};
    std::vector<std::pair<HWND, UINT>> subscribers_;
};

} // namespace nekorox::component
