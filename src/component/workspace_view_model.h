#pragma once

#include "library_model.h"

#include <cstddef>
#include <string>
#include <vector>

namespace nekorox::component {

enum class FacetSelectionKind {
    all,
    missing,
    value,
};

struct FacetSelection {
    FacetSelectionKind kind = FacetSelectionKind::all;
    std::wstring value;

    static FacetSelection fromValue(std::wstring text)
    {
        return {text.empty() ? FacetSelectionKind::missing
                             : FacetSelectionKind::value,
            std::move(text)};
    }
    void clear() noexcept { kind = FacetSelectionKind::all; value.clear(); }
    [[nodiscard]] bool empty() const noexcept
    {
        return kind == FacetSelectionKind::all;
    }
    [[nodiscard]] bool isAll() const noexcept
    {
        return kind == FacetSelectionKind::all;
    }
    [[nodiscard]] bool operator==(const FacetSelection& other) const noexcept
    {
        return kind == other.kind && value == other.value;
    }
    FacetSelection& operator=(std::wstring text)
    {
        *this = fromValue(std::move(text));
        return *this;
    }
};

struct FacetItem {
    std::wstring name;
    std::size_t trackCount = 0;
    std::wstring countText;
};

struct AlbumGroup {
    std::wstring artist;
    std::wstring album;
    std::wstring year;
    std::wstring codec;
    std::wstring displaySummary;
    std::vector<std::size_t> trackIndices;
    double durationSeconds = 0.0;
};

enum class ResultEntryKind {
    albumHeader,
    track,
};

struct ResultEntry {
    ResultEntryKind kind = ResultEntryKind::track;
    std::size_t groupIndex = 0;
    std::size_t trackIndex = 0;
    std::size_t localTrackIndex = 0;
    std::wstring displayTrackNumber;
    std::wstring displayTitle;
    std::wstring displayPlayCount;
    std::wstring displayDuration;
    double top = 0.0;
    double height = 0.0;
    int observedPlayCount = -1;
};

class WorkspaceViewModel final {
public:
    bool refreshStatistics(const std::vector<LibraryTrack>& tracks);
    void rebuild(
        const std::vector<LibraryTrack>& tracks,
        FacetSelection& selectedGenre,
        FacetSelection& selectedArtist,
        FacetSelection& selectedAlbum,
        const std::wstring& query,
        double albumHeaderHeight = 80.0);

    const std::vector<FacetItem>& genres() const noexcept { return genres_; }
    const std::vector<FacetItem>& artists() const noexcept { return artists_; }
    const std::vector<FacetItem>& albums() const noexcept { return albums_; }
    const std::vector<AlbumGroup>& groups() const noexcept { return groups_; }
    const std::vector<ResultEntry>& entries() const noexcept { return entries_; }
    std::size_t resultTrackCount() const noexcept { return resultTrackCount_; }
    double resultDurationSeconds() const noexcept { return resultDurationSeconds_; }
    double resultHeight() const noexcept { return resultHeight_; }

private:
    std::vector<FacetItem> genres_;
    std::vector<FacetItem> artists_;
    std::vector<FacetItem> albums_;
    std::vector<AlbumGroup> groups_;
    std::vector<ResultEntry> entries_;
    std::size_t resultTrackCount_ = 0;
    double resultDurationSeconds_ = 0.0;
    double resultHeight_ = 0.0;
};

} // namespace nekorox::component
