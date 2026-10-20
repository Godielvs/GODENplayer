#pragma once

#include "nekorox/art_palette.h"
#include "nekorox/ambient_features.h"
#include "c11/c11_album_layout.h"
#include "motion_policy.h"

#include <cstdint>
#include <windows.h>

namespace nekorox::component {

enum class ListenEffectsMode : std::uint32_t {
    softLight = 0,
    noEffects = 1,
};

enum class ListenEffectsFallback : std::uint32_t {
    none = 0,
    disabled,
    softwareRasterizer,
    unavailable,
};

struct ListenEffectsDiagnostics {
    ListenEffectsMode mode = ListenEffectsMode::softLight;
    ListenEffectsFallback fallback = ListenEffectsFallback::none;
    std::uint64_t submissions = 0;
    std::uint64_t successes = 0;
    std::uint64_t resourceBytes = 0;
    bool gpuMeasured = false;
    double gpuMicroseconds = 0.0;
};

enum class AlbumAmbientFallback : std::uint32_t {
    none = 0,
    disabled,
    rolloutDisabled,
    reducedMotion,
    softwareRasterizer,
    noArtwork,
    unavailable,
    hidden,
    sourceStale,
};

struct AlbumAmbientDiagnostics {
    AlbumAmbientMode mode = AlbumAmbientMode::reactive;
    AlbumAmbientFallback fallback = AlbumAmbientFallback::rolloutDisabled;
    std::uint64_t textureUpdates = 0;
    std::uint64_t featurePublications = 0;
    std::uint64_t resourceBytes = 0;
    bool gpuMeasured = false;
    bool degraded = false;
    double gpuMicroseconds = 0.0;
    double gpuP95Microseconds = 0.0;
};

void initializeMotionPreferences() noexcept;
MotionMode motionMode() noexcept;
MotionPreference motionPreference() noexcept;
void setMotionPreference(MotionPreference preference);
void refreshSystemMotionPreference() noexcept;
void setMotionMode(MotionMode mode);
double scrollResponseScale() noexcept;
ListenEffectsMode listenEffectsMode() noexcept;
void setListenEffectsMode(ListenEffectsMode mode);
ListenEffectsDiagnostics listenEffectsDiagnostics() noexcept;
void publishListenEffectsDiagnostics(
    const ListenEffectsDiagnostics& diagnostics) noexcept;
AlbumAmbientMode albumAmbientMode() noexcept;
void setAlbumAmbientMode(AlbumAmbientMode mode);
void initializeAlbumAmbientPreference();
void initializeAlbumPanelPreferences() noexcept;
bool albumShowTracks() noexcept;
bool albumShowLyrics() noexcept;
bool albumShowArtwork() noexcept;
bool albumTheaterMode() noexcept;
bool setAlbumTheaterMode(bool enabled);
goden::c11::ArtworkAlignment albumArtworkAlignment() noexcept;
bool setAlbumShowTracks(bool visible);
bool setAlbumShowLyrics(bool visible);
bool setAlbumShowArtwork(bool visible);
bool setAlbumArtworkAlignment(goden::c11::ArtworkAlignment alignment);
AlbumAmbientDiagnostics albumAmbientDiagnostics() noexcept;
void publishAlbumAmbientDiagnostics(
    const AlbumAmbientDiagnostics& diagnostics) noexcept;
void publishAlbumAmbientRenderDiagnostics(std::uint64_t resourceBytes,
    bool degraded, bool gpuMeasured, double gpuMicroseconds,
    double gpuP95Microseconds) noexcept;
bool headerWordmarkEnabled() noexcept;
void setHeaderWordmarkEnabled(bool enabled);
void initializeHeaderWordmarkPreference();
RgbColor fallbackAccentColor() noexcept;
void setFallbackAccentColor(RgbColor color);
bool chooseFallbackAccentColor(HWND owner);
void initializeFallbackAccentPreference();

} // namespace nekorox::component
