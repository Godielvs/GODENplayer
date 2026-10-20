#pragma once

#include "c11/c11_album_layout.h"

#include <d2d1.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace nekorox::component {

enum class AlbumPanelKind : std::uint8_t {
    none,
    lyrics,
    tracks,
};

struct AlbumModeLayout {
    D2D1_RECT_F viewport{};
    D2D1_RECT_F cover{};
    D2D1_RECT_F lyrics{};
    D2D1_RECT_F tracks{};
    D2D1_RECT_F header{};
    D2D1_RECT_F follow{};
    D2D1_RECT_F lyricsTab{};
    D2D1_RECT_F tracksTab{};
    D2D1_RECT_F chooseAlbum{};
    D2D1_RECT_F followCurrent{};
    D2D1_RECT_F tracksToggle{};
    D2D1_RECT_F lyricsToggle{};
    D2D1_RECT_F panelsMenu{};
    D2D1_RECT_F visualMenu{};
    AlbumPanelKind primary = AlbumPanelKind::tracks;
    bool threeColumns = false;
    bool stacked = false;
};

using AlbumPanelVisibility = goden::c11::Visibility;

[[nodiscard]] AlbumModeLayout computeAlbumModeLayout(
    float widthDip,
    float heightDip,
    bool hasLyrics,
    AlbumPanelKind compactSelection = AlbumPanelKind::lyrics,
    AlbumPanelVisibility visibility = {}) noexcept;

[[nodiscard]] std::optional<std::size_t> albumTrackAtPoint(
    const AlbumModeLayout& layout,
    float xDip,
    float yDip,
    std::size_t trackCount,
    float scrollDip = 0.0f) noexcept;

// Track rows are 28 DIP below a fixed 38-DIP header. Keeping this bound in
// the shared view model makes painting, hit-testing and wheel input agree at
// every responsive breakpoint.
[[nodiscard]] float albumTrackMaximumScroll(
    const AlbumModeLayout& layout,
    std::size_t trackCount) noexcept;

// Keep the scale pivot on screen even for a single multi-page paragraph.
[[nodiscard]] float albumPlainLyricsScaleCenter(
    const D2D1_RECT_F& viewport, float blockTop,
    float blockHeight, float scroll) noexcept;

[[nodiscard]] bool pointInAlbumRect(
    const D2D1_RECT_F& rectangle,
    float x,
    float y) noexcept;

} // namespace nekorox::component
