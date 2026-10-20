#include "album_mode_view.h"

#include <algorithm>

namespace nekorox::component {
namespace {

D2D1_RECT_F nativeRect(const goden::c11::Rect& value) noexcept
{
    return D2D1::RectF(value.l, value.t, value.r, value.b);
}

} // namespace

AlbumModeLayout computeAlbumModeLayout(float width, float height,
    bool hasLyrics, AlbumPanelKind compactSelection,
    AlbumPanelVisibility visibility) noexcept
{
    (void)hasLyrics;
    width = std::max(0.0f, width);
    height = std::max(0.0f, height);
    AlbumModeLayout out;
    out.viewport = D2D1::RectF(0, 0, width, height);
    out.chooseAlbum = D2D1::RectF(
        std::max(0.0f, width * 0.5f - 240.0f),
        std::max(0.0f, height * 0.5f - 48.0f),
        std::min(width, width * 0.5f + 240.0f),
        std::min(height, height * 0.5f + 48.0f));
    const auto designed = goden::c11::layoutAlbum(width, height, visibility,
        false, compactSelection == AlbumPanelKind::lyrics);
    out.header = nativeRect(designed.title);
    out.cover = nativeRect(designed.art);
    out.tracks = nativeRect(designed.tracks);
    out.lyrics = nativeRect(designed.lyrics);
    out.tracksToggle = nativeRect(designed.tracksToggle);
    out.lyricsToggle = nativeRect(designed.lyricsToggle);
    out.panelsMenu = nativeRect(designed.panelsMenu);
    out.visualMenu = nativeRect(designed.preset);
    out.threeColumns = !designed.tabbed &&
        !designed.tracks.empty() && !designed.lyrics.empty();
    out.stacked = width < 700.0f;
    out.primary = !designed.lyrics.empty()
        ? AlbumPanelKind::lyrics
        : (!designed.tracks.empty() ? AlbumPanelKind::tracks
                                    : AlbumPanelKind::none);
    if (designed.tabbed) {
        // Compact layouts already select Tracks/Lyrics with the tabs below.
        // Visibility remains available in Panels, without a second pair of
        // similarly labelled controls (including invisible hit/accessibility areas).
        out.tracksToggle = {};
        out.lyricsToggle = {};
        if (!designed.tracksToggle.empty()) {
            out.panelsMenu = nativeRect(goden::c11::box(
                designed.lyricsToggle.r - 72.f, designed.panelsMenu.t,
                72.f, designed.panelsMenu.height()));
        }
        auto& panel = out.primary == AlbumPanelKind::lyrics
            ? out.lyrics : out.tracks;
        out.lyricsTab = D2D1::RectF(panel.left, panel.top,
            panel.left + 86.0f, panel.top + 34.0f);
        out.tracksTab = D2D1::RectF(out.lyricsTab.right, panel.top,
            out.lyricsTab.right + 86.0f, panel.top + 34.0f);
        panel.top += 38.0f;
    }
    if (out.lyrics.right > out.lyrics.left) {
        out.follow = D2D1::RectF(out.lyrics.right - 112.0f,
            out.lyrics.top, out.lyrics.right, out.lyrics.top + 34.0f);
    }
    out.followCurrent = D2D1::RectF(out.cover.left,
        out.cover.bottom + 4.0f, out.cover.right,
        out.cover.bottom + 32.0f);
    if(!visibility.artwork){
        out.followCurrent=D2D1::RectF(std::max(out.header.left,width-272.f),
            out.header.top,width-12.f,out.header.bottom);
        out.header.right=std::max(out.header.left,out.followCurrent.left-8.f);
    }
    return out;
}

bool pointInAlbumRect(const D2D1_RECT_F& rectangle, float x, float y) noexcept
{
    return rectangle.right > rectangle.left && rectangle.bottom > rectangle.top &&
        x >= rectangle.left && x < rectangle.right &&
        y >= rectangle.top && y < rectangle.bottom;
}

std::optional<std::size_t> albumTrackAtPoint(
    const AlbumModeLayout& layout, float x, float y,
    std::size_t trackCount, float scroll) noexcept
{
    if (!pointInAlbumRect(layout.tracks, x, y)) return std::nullopt;
    constexpr float header = 38.0f;
    constexpr float row = 28.0f;
    const auto local = y - layout.tracks.top - header + std::max(0.0f, scroll);
    if (local < 0.0f) return std::nullopt;
    const auto index = static_cast<std::size_t>(local / row);
    return index < trackCount ? std::optional(index) : std::nullopt;
}

float albumTrackMaximumScroll(
    const AlbumModeLayout& layout,
    std::size_t trackCount) noexcept
{
    constexpr float header = 38.0f;
    constexpr float row = 28.0f;
    if (layout.tracks.right <= layout.tracks.left ||
        layout.tracks.bottom <= layout.tracks.top + header) {
        return 0.0f;
    }
    const auto viewport = layout.tracks.bottom - layout.tracks.top - header;
    return std::max(0.0f,
        static_cast<float>(trackCount) * row - viewport);
}

float albumPlainLyricsScaleCenter(const D2D1_RECT_F& viewport,
    float blockTop, float blockHeight, float scroll) noexcept
{
    const auto center = viewport.top + blockTop - scroll + blockHeight * .5f;
    if (blockHeight <= viewport.bottom - viewport.top) return center;
    return std::clamp(center,
        viewport.top, std::max(viewport.top, viewport.bottom));
}

} // namespace nekorox::component
