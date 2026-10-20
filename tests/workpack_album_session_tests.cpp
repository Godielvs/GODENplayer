#include "nekorox/album_session.h"

#include <stdexcept>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

} // namespace

void runWorkpackAlbumSessionTests()
{
    using namespace nekorox;
    std::vector<AlbumSessionCandidate> tracks{
        {L"Artist", L"Album", L"2025", L"", L"C:\\A\\Album\\CD2\\02.flac", "r4", "e1", 2, 2, 0, 4},
        {L"Artist", L"Album", L"2025", L"", L"C:\\A\\Album\\CD1\\02.flac", "r2", "e1", 1, 2, 0, 2},
        {L"Artist", L"Album", L"2025", L"", L"C:\\Other\\Album\\01.flac", "other", "e2", 1, 1, 0, 5},
        {L"Artist", L"Album", L"2025", L"", L"C:\\A\\Album\\CD1\\01.flac", "r1", "e1", 1, 1, 0, 1},
        {L"Artist", L"Album", L"2025", L"", L"C:\\A\\Album\\CD2\\01.flac", "r3", "e1", 2, 1, 0, 3},
    };
    const auto session = resolveAlbumSession(tracks, 0);
    require(session.size() == 4U,
        "multi-disc session did not stay within one edition root");
    require(session[0] == 3U && session[1] == 1U &&
            session[2] == 4U && session[3] == 0U,
        "album session order is not disc/track/subsong deterministic");

    tracks[0].releaseId = L"release-a";
    tracks[2].releaseId = L"release-a";
    const auto release = resolveAlbumSession(tracks, 0);
    require(release.size() == 2U,
        "release ID did not take precedence over folder identity");

    // Figurehead regression: the first remastered track is tagged 2011,
    // the other twenty 2010. Library shows 21; Album used to show 1 or 20
    // depending on which track opened the session.
    std::vector<AlbumSessionCandidate> figurehead;
    for (int number = 15; number <= 35; ++number) {
        AlbumSessionCandidate track;
        track.albumArtist = L"The Queenstons";
        track.album = L"Figurehead";
        track.year = number == 15 ? L"2011" : L"2010";
        track.path = L"C:\\Music\\Figurehead\\" + std::to_wstring(number) + L".flac";
        track.track = number;
        track.sourceIndex = figurehead.size();
        figurehead.push_back(std::move(track));
    }
    const auto wholeAlbum = resolveAlbumSession(figurehead, 0);
    require(wholeAlbum.size() == 21 &&
        resolveAlbumSession(figurehead, 10) == wholeAlbum,
        "Mixed per-track years split the same album by anchor");
    require(albumSessionYear(figurehead, wholeAlbum) == L"2010",
        "Album date did not follow the majority of its tracks");
    require(figurehead[wholeAlbum.front()].track == 15 &&
        figurehead[wholeAlbum.back()].track == 35,
        "Album resolution changed tagged track ordering");
    figurehead[4].year.clear();
    require(resolveAlbumSession(figurehead, 4) == wholeAlbum &&
        albumSessionYear(figurehead, wholeAlbum) == L"2010",
        "A missing date split the album or replaced its representative year");
    auto otherEdition = figurehead.front();
    otherEdition.path = L"C:\\Music\\Figurehead original\\15.flac";
    figurehead.push_back(otherEdition);
    require(resolveAlbumSession(figurehead, 0) == wholeAlbum,
        "Ignoring DATE merged a separate edition folder");
    figurehead.back().path = L"C:\\Music\\Figurehead\\other-release.flac";
    figurehead.back().releaseId = L"different-release";
    require(resolveAlbumSession(figurehead, 0) == wholeAlbum &&
        resolveAlbumSession(figurehead, 21) == std::vector<std::size_t>{21},
        "Ignoring DATE merged an explicit different release");
    for (auto& track : figurehead) track.year.clear();
    require(albumSessionYear(figurehead, wholeAlbum).empty(),
        "An album with no date invented a year");

    PlaybackPrecedenceInput precedence;
    precedence.keepPlaying = true;
    precedence.nativeOrder = true;
    precedence.repeatAlbum = true;
    precedence.manualQueue = true;
    require(resolvePlaybackPrecedence(precedence) ==
            PlaybackPrecedence::manualQueue,
        "manual queue did not win playback precedence");
    precedence.userStop = true;
    require(resolvePlaybackPrecedence(precedence) ==
            PlaybackPrecedence::userStop,
        "explicit stop did not win playback precedence");

    require(!resolveAlbumLocalIndex(0, 4, -1) &&
            resolveAlbumLocalIndex(0, 4, 1) == 1U &&
            resolveAlbumLocalIndex(3, 4, -1) == 2U &&
            !resolveAlbumLocalIndex(3, 4, 1),
        "album-local navigation crossed a session boundary");
}
