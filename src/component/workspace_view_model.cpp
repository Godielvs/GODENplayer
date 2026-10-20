#include "workspace_view_model.h"

#include "text_util.h"
#include "ui_design_system.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <map>

namespace nekorox::component {
namespace {

const double kFullAlbumHeaderHeight =
    UiDesignSystemV1::metrics().albumHeaderDip;
const double kTrackRowHeight =
    UiDesignSystemV1::metrics().rowDip;

struct TextLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const noexcept
    {
        return compareText(left, right) < 0;
    }
};

bool equalText(const std::wstring& left, const std::wstring& right) noexcept
{
    return compareText(left, right) == 0;
}

std::wstring folded(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

bool contains(const std::wstring& value, const std::wstring& query)
{
    if (query.empty()) return true;
    return folded(value).find(query) != std::wstring::npos;
}

bool matchesQuery(const LibraryTrack& track, const std::wstring& query)
{
    if (query.empty()) return true;
    if (!track.searchFolded.empty()) {
        return track.searchFolded.find(query) != std::wstring::npos;
    }
    return contains(track.genre, query) || contains(track.artist, query) ||
        contains(track.albumArtist, query) || contains(track.album, query) ||
        contains(track.title, query) || contains(track.year, query);
}

bool matches(const FacetSelection& selected, const std::wstring& value) noexcept
{
    if (selected.kind == FacetSelectionKind::all) return true;
    if (selected.kind == FacetSelectionKind::missing) return value.empty();
    return equalText(selected.value, value);
}

bool matchesArtist(
    const FacetSelection& selected,
    const LibraryTrack& track) noexcept
{
    if (selected.kind == FacetSelectionKind::all) return true;
    if (selected.kind == FacetSelectionKind::missing) {
        return track.albumArtists.empty() && track.albumArtist.empty();
    }
    if (track.albumArtists.empty()) {
        return equalText(selected.value, track.albumArtist);
    }
    return std::any_of(
        track.albumArtists.begin(),
        track.albumArtists.end(),
        [&](const std::wstring& artist) {
            return equalText(selected.value, artist);
        });
}

std::wstring durationLabel(double seconds)
{
    const auto total = static_cast<long long>(std::max(0.0, std::round(seconds)));
    wchar_t buffer[32]{};
    if (total >= 3600) {
        std::swprintf(buffer, std::size(buffer), L"%lld:%02lld:%02lld",
            total / 3600, (total / 60) % 60, total % 60);
    } else {
        std::swprintf(buffer, std::size(buffer), L"%lld:%02lld",
            total / 60, total % 60);
    }
    return buffer;
}

std::wstring trackNumberLabel(int number, std::size_t fallback)
{
    wchar_t buffer[20]{};
    std::swprintf(buffer, std::size(buffer), L"%02d",
        number > 0 ? number : static_cast<int>(fallback + 1U));
    return buffer;
}

std::vector<FacetItem> toFacetItems(
    const std::map<std::wstring, std::size_t, TextLess>& counts)
{
    std::vector<FacetItem> result;
    result.reserve(counts.size());
    for (const auto& [name, count] : counts) {
        result.push_back({name, count, std::to_wstring(count)});
    }
    return result;
}

bool hasFacet(
    const std::vector<FacetItem>& items,
    const FacetSelection& selected) noexcept
{
    if (selected.kind == FacetSelectionKind::all) return true;
    return std::any_of(items.begin(), items.end(), [&](const FacetItem& item) {
        if (selected.kind == FacetSelectionKind::missing)
            return item.name.empty();
        return equalText(item.name, selected.value);
    });
}

std::wstring representativeAlbumYear(
    const std::vector<LibraryTrack>& tracks,
    const std::vector<std::size_t>& trackIndices)
{
    std::map<std::wstring, std::size_t, TextLess> counts;
    std::wstring representative;
    std::size_t representativeCount = 0;
    for (const auto trackIndex : trackIndices) {
        if (trackIndex >= tracks.size() || tracks[trackIndex].year.empty()) {
            continue;
        }
        const auto count = ++counts[tracks[trackIndex].year];
        if (count > representativeCount) {
            representative = tracks[trackIndex].year;
            representativeCount = count;
        }
    }
    return representative;
}

} // namespace

void WorkspaceViewModel::rebuild(
    const std::vector<LibraryTrack>& tracks,
    FacetSelection& selectedGenre,
    FacetSelection& selectedArtist,
    FacetSelection& selectedAlbum,
    const std::wstring& query,
    double albumHeaderHeight)
{
    albumHeaderHeight = std::clamp(
        albumHeaderHeight,
        kTrackRowHeight * 2.0,
        kFullAlbumHeaderHeight);
    const auto normalizedQuery = folded(trimText(query));

    // Keep one query result for the cascading facets and the result groups.
    // LibraryTrack::searchFolded is prepared when the library snapshot is built.
    std::vector<std::size_t> queryMatches;
    queryMatches.reserve(tracks.size());
    std::map<std::wstring, std::size_t, TextLess> genreCounts;
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        if (!matchesQuery(tracks[index], normalizedQuery)) continue;
        queryMatches.push_back(index);
        ++genreCounts[tracks[index].genre];
    }
    genres_ = toFacetItems(genreCounts);
    if (!hasFacet(genres_, selectedGenre)) selectedGenre.clear();

    std::map<std::wstring, std::size_t, TextLess> artistCounts;
    for (const auto index : queryMatches) {
        const auto& track = tracks[index];
        if (matches(selectedGenre, track.genre)) {
            if (track.albumArtists.empty()) {
                ++artistCounts[track.albumArtist];
            } else {
                for (const auto& artist : track.albumArtists) {
                    ++artistCounts[artist];
                }
            }
        }
    }
    artists_ = toFacetItems(artistCounts);
    if (!hasFacet(artists_, selectedArtist)) selectedArtist.clear();

    std::map<std::wstring, std::size_t, TextLess> albumCounts;
    for (const auto index : queryMatches) {
        const auto& track = tracks[index];
        if (matches(selectedGenre, track.genre) &&
            matchesArtist(selectedArtist, track)) {
            ++albumCounts[track.album];
        }
    }
    albums_ = toFacetItems(albumCounts);
    if (!hasFacet(albums_, selectedAlbum)) selectedAlbum.clear();

    groups_.clear();
    entries_.clear();
    resultTrackCount_ = 0;
    resultDurationSeconds_ = 0.0;
    resultHeight_ = 0.0;

    for (const auto index : queryMatches) {
        const auto& track = tracks[index];
        if (!matches(selectedGenre, track.genre) ||
            !matchesArtist(selectedArtist, track) ||
            !matches(selectedAlbum, track.album)) {
            continue;
        }

        if (groups_.empty() ||
            !equalText(groups_.back().artist, track.albumArtist) ||
            !equalText(groups_.back().album, track.album)) {
            AlbumGroup group;
            group.artist = track.albumArtist;
            group.album = track.album;
            group.codec = track.codec;
            groups_.push_back(std::move(group));
        }
        auto& group = groups_.back();
        group.trackIndices.push_back(index);
        group.durationSeconds += track.durationSeconds;
        ++resultTrackCount_;
        resultDurationSeconds_ += track.durationSeconds;
    }

    for (auto& group : groups_) {
        // DATE is presentation metadata, not album identity. When tags
        // disagree, prefer the non-empty year carried by most tracks.
        group.year = representativeAlbumYear(tracks, group.trackIndices);
        group.displaySummary = group.codec + L" | " +
            std::to_wstring(group.trackIndices.size()) + L" Tracks | Time: " +
            durationLabel(group.durationSeconds);
    }

    for (std::size_t groupIndex = 0; groupIndex < groups_.size(); ++groupIndex) {
        entries_.push_back({
            ResultEntryKind::albumHeader,
            groupIndex,
            0,
            0,
            {}, {}, {}, {},
            resultHeight_,
            albumHeaderHeight,
        });
        resultHeight_ += albumHeaderHeight;
        for (std::size_t localIndex = 0;
             localIndex < groups_[groupIndex].trackIndices.size(); ++localIndex) {
            const auto trackIndex = groups_[groupIndex].trackIndices[localIndex];
            const auto& track = tracks[trackIndex];
            auto displayTitle = track.title;
            if (!equalText(track.artist, groups_[groupIndex].artist)) {
                displayTitle += L"  \x00b7  " + track.artist;
            }
            entries_.push_back({
                ResultEntryKind::track,
                groupIndex,
                trackIndex,
                localIndex,
                trackNumberLabel(track.track, localIndex),
                std::move(displayTitle),
                std::to_wstring(track.playCount),
                durationLabel(track.durationSeconds),
                resultHeight_,
                kTrackRowHeight,
            });
            resultHeight_ += kTrackRowHeight;
            entries_.back().observedPlayCount=track.playCount;
        }
    }
}

bool WorkspaceViewModel::refreshStatistics(const std::vector<LibraryTrack>& tracks)
{
    bool changed=false;
    for(auto& entry:entries_)if(entry.kind==ResultEntryKind::track&&entry.trackIndex<tracks.size()){
        const auto count=tracks[entry.trackIndex].playCount;
        if(entry.observedPlayCount!=count){entry.displayPlayCount=std::to_wstring(count);entry.observedPlayCount=count;changed=true;}
    }
    return changed;
}

} // namespace nekorox::component
