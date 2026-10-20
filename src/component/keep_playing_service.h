#pragma once

#include "sdk_compat.h"

#include <cstddef>
#include <cstdint>
#include <vector>
#include <unordered_map>

namespace nekorox::component {

enum class KeepPlayingMode : std::uint8_t {
    randomAlbums = 0,
    sequentialAlbums = 1,
};

using PlaybackAlbum = std::vector<metadb_handle_ptr>;

class KeepPlayingService final {
public:
    static KeepPlayingService& instance();

    [[nodiscard]] bool enabled() const noexcept;
    void setEnabled(bool enabled);
    [[nodiscard]] KeepPlayingMode mode() const noexcept;
    void setMode(KeepPlayingMode mode);

    void captureLibraryContext(
        std::vector<PlaybackAlbum> albums,
        std::size_t currentAlbum);
    void clearContext();
    void onPlaybackStarting(play_control::t_track_command command);
    void onNewTrack(const metadb_handle_ptr& track);
    void onPlaybackStop(play_control::t_stop_reason reason);
    void onQueueChanged();
    [[nodiscard]] bool handleNextCommand();
    void stop();

private:
    KeepPlayingService();
    void ensureFullLibraryContext(bool forceExpand = false);
    void indexContext();
    void refillRandomBag();
    bool continuePlayback(std::uint64_t token);
    bool resolveNativeNextBoundary(std::uint64_t token);
    bool playNextAlbum();
    bool playAlbum(std::size_t index);
    bool albumContains(
        const PlaybackAlbum& album,
        const metadb_handle_ptr& track) const;

    std::vector<PlaybackAlbum> albums_;
    std::unordered_map<const metadb_handle*, std::size_t> albumByTrack_;
    std::vector<std::size_t> randomBag_;
    std::size_t currentAlbum_ = static_cast<std::size_t>(-1);
    metadb_handle_ptr expectedTrack_;
    std::uint64_t generation_ = 0;
    bool continuationInFlight_ = false;
    bool fullLibraryContext_ = false;
    bool nativeNextBoundaryPending_ = false;
    metadb_handle_ptr nativeNextExpectedTrack_;
};

} // namespace nekorox::component
