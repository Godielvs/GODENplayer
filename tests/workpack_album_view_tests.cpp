#include "../src/component/album_mode_view.h"

#include <algorithm>
#include <stdexcept>

namespace {

void requireAlbumView(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

} // namespace

void runWorkpackAlbumViewTests()
{
    using namespace nekorox::component;
    const auto wide = computeAlbumModeLayout(1440.0f, 820.0f, true);
    requireAlbumView(wide.threeColumns && !wide.stacked,
        "WP14 wide layout did not use three columns");
    requireAlbumView(wide.cover.right - wide.cover.left >= 320.0f &&
        wide.cover.right - wide.cover.left <= 560.0f &&
        wide.lyrics.right - wide.lyrics.left >= 320.0f &&
        wide.tracks.right - wide.tracks.left >= 220.0f &&
        wide.tracks.left > wide.cover.right &&
        wide.lyrics.left > wide.tracks.right,
        "WP14 wide clamps violated cover/lyrics/track minimums");
    const auto wideWithoutLyrics = computeAlbumModeLayout(
        1440.0f, 820.0f, false, AlbumPanelKind::tracks,
        AlbumPanelVisibility{true, false});
    requireAlbumView(wideWithoutLyrics.tracks.left ==
            wideWithoutLyrics.cover.right + 24.0f &&
        wideWithoutLyrics.tracks.right > wideWithoutLyrics.tracks.left &&
        wideWithoutLyrics.lyrics.right == wideWithoutLyrics.lyrics.left,
        "C11 wide hidden Lyrics panel still consumed layout space");

    const auto medium = computeAlbumModeLayout(1000.0f, 700.0f, true);
    requireAlbumView(!medium.threeColumns && !medium.stacked &&
        medium.lyrics.right > medium.lyrics.left &&
        medium.tracks.right == medium.tracks.left,
        "WP14 medium layout did not select one right panel");
    requireAlbumView(medium.tracksToggle.right == medium.tracksToggle.left &&
        medium.lyricsToggle.right == medium.lyricsToggle.left &&
        medium.panelsMenu.right > medium.panelsMenu.left &&
        medium.lyricsTab.right > medium.lyricsTab.left &&
        medium.tracksTab.left == medium.lyricsTab.right,
        "Compact Album repeated tabs as visibility controls or lost Panels");
    requireAlbumView(wide.tracksToggle.right > wide.tracksToggle.left &&
        wide.lyricsToggle.right > wide.lyricsToggle.left &&
        wide.lyricsTab.right == wide.lyricsTab.left,
        "Wide Album lost its visibility controls or acquired compact tabs");
    const auto mediumTracks = computeAlbumModeLayout(
        1000.0f, 700.0f, true, AlbumPanelKind::tracks);
    requireAlbumView(mediumTracks.tracks.right > mediumTracks.tracks.left &&
        mediumTracks.lyrics.right == mediumTracks.lyrics.left,
        "WP14 compact Tracks selection was ignored");

    const auto narrow = computeAlbumModeLayout(640.0f, 760.0f, false,
        AlbumPanelKind::lyrics, AlbumPanelVisibility{true, false});
    requireAlbumView(narrow.stacked &&
        narrow.cover.right - narrow.cover.left <= 280.0f &&
        narrow.primary == AlbumPanelKind::tracks &&
        narrow.tracks.right > narrow.tracks.left &&
        narrow.lyrics.right == narrow.lyrics.left,
        "C11 narrow layout did not remove the hidden Lyrics panel");
    const auto narrowTracks = computeAlbumModeLayout(
        640.0f, 760.0f, false, AlbumPanelKind::tracks);
    requireAlbumView(albumTrackAtPoint(narrowTracks,
            narrowTracks.tracks.left + 10.0f,
            narrowTracks.tracks.top + 38.0f + 14.0f,
            3U, 0.0f) == 0U &&
        !albumTrackAtPoint(narrowTracks,
            narrowTracks.cover.left, narrowTracks.cover.top,
            3U, 0.0f),
        "WP14 track hit-test diverged from rendered layout");

    const auto tracksViewport = narrowTracks.tracks.bottom -
        narrowTracks.tracks.top - 38.0f;
    const auto expectedMaximum = std::max(
        0.0f, 20.0f * 28.0f - tracksViewport);
    requireAlbumView(albumTrackMaximumScroll(narrowTracks, 20U) == expectedMaximum &&
        albumTrackMaximumScroll(narrowTracks, 1U) == 0.0f &&
        albumTrackMaximumScroll(medium, 50U) == 0.0f,
        "Album track scrolling is not clamped to the final row");

    const auto artOnly = computeAlbumModeLayout(1000.0f, 700.0f, true,
        AlbumPanelKind::none, AlbumPanelVisibility{false, false});
    const auto anchoredCover = computeAlbumModeLayout(1000.0f, 700.0f, true,
        AlbumPanelKind::tracks, AlbumPanelVisibility{true, false});
    requireAlbumView(artOnly.tracks.right == artOnly.tracks.left &&
        artOnly.lyrics.right == artOnly.lyrics.left &&
        artOnly.cover.right > artOnly.cover.left &&
        artOnly.cover.left == anchoredCover.cover.left &&
        artOnly.cover.top == anchoredCover.cover.top &&
        artOnly.primary == AlbumPanelKind::none,
        "C11 art-only layout moved the cover away from its left anchor");
}
