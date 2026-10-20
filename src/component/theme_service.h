#pragma once

#include "art_cache.h"
#include "ui_animation_policy.h"

#include "nekorox/art_palette.h"
#include "nekorox/contracts.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

namespace nekorox::component {

struct ThemeState {
    std::uint64_t generation = 0;
    std::uint64_t artworkGeneration = 0;
    std::uint64_t intentGeneration = 0;
    RgbColor accent{0.0f, 0.847f, 0.816f};
    RgbColor secondary{0.66f, 0.91f, 0.90f};
    RgbColor highlight{0.84f, 0.94f, 0.95f};
    float transitionProgress = 1.0f;
    bool transitionActive = false;
};

class ThemeService final {
public:
    static ThemeService& instance();

    ThemeState snapshot() const;
    // The final palette selected for the current artwork, without sampling
    // the presentation interpolation.  Retained materials use this to build
    // once in their new colour while their outgoing bitmap keeps the old one.
    ThemeState targetSnapshot() const;
    ThemeSnapshotV2 contractSnapshot() const;
    bool transitionActive() const;
    [[nodiscard]] std::uint64_t nextSampleQpc() const;
    bool updateFromArtwork(const std::wstring& identity, const ArtPixels& pixels);
    void resetForTrack(const std::wstring& identity);
    bool resolveWithoutArtwork(const std::wstring& identity);
    bool refreshFallbackPalette();

    // Subscribers receive one posted message when a palette transition
    // starts. Each surface keeps ownership of its refresh-aware frame clock.
    void subscribe(HWND window, UINT message);
    void unsubscribe(HWND window) noexcept;

private:
    ThemeService() = default;
    ThemeState sampleLocked(double nowSeconds) const noexcept;
    void beginTransitionLocked(
        RgbColor accent,
        RgbColor secondary,
        RgbColor highlight,
        double nowSeconds);

    mutable std::mutex mutex_;
    ThemeState state_{};
    ThemeState source_{};
    std::wstring identity_;
    std::uint64_t artworkContentHash_ = 0;
    std::uint64_t artworkIdentityHash_ = 0;
    double transitionStartSeconds_ = 0.0;
    double transitionDurationSeconds_ = 0.0;
    bool resolved_ = false;
    bool artworkDerived_ = false;
    std::uint64_t intentGeneration_ = 0;
    mutable ThemeState sampled_{};
    mutable std::uint64_t sampledBucket_ = 0;
    mutable std::uint64_t sampledRevision_ = 0;
    std::uint64_t sampleRevision_ = 1;
    std::vector<std::pair<HWND, UINT>> subscribers_;
};

} // namespace nekorox::component
