#pragma once
#include <tuple>

namespace nekorox::component {
// Ratings and playback statistics affect rows, not filtering, ordering or
// album headers. Keep the structural view stable for those common updates.
template<class Track>
bool sameLibraryLayout(const Track& a,const Track& b) noexcept {
    const auto fields=[](const Track& t){return std::tie(t.genre,t.artist,t.albumArtist,t.albumArtists,
        t.album,t.title,t.year,t.codec,t.releaseId,t.path,t.searchFolded,t.disc,t.track,
        t.sampleRate,t.bitsPerSample,t.durationSeconds);};
    return fields(a)==fields(b);
}
}
