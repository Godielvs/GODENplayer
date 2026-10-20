#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nekorox {

struct AlbumSessionCandidate {
    std::wstring albumArtist;
    std::wstring album;
    std::wstring year;
    std::wstring releaseId;
    std::wstring path;
    std::string revisionTrackId;
    std::string entityId;
    int disc = 0;
    int track = 0;
    std::uint32_t subsong = 0;
    std::size_t sourceIndex = 0;
};

[[nodiscard]] std::wstring albumEditionRoot(
    const std::wstring& path);
[[nodiscard]] std::vector<std::size_t> resolveAlbumSession(
    const std::vector<AlbumSessionCandidate>& candidates,
    std::size_t anchorIndex);
// DATE is presentation metadata, not an edition boundary. Match the Library's
// non-empty majority rule over the resolved album, independently of its anchor.
[[nodiscard]] std::wstring albumSessionYear(
    const std::vector<AlbumSessionCandidate>& candidates,
    const std::vector<std::size_t>& selected);

enum class PlaybackPrecedence : std::uint8_t {
    userStop = 0,
    manualQueue,
    repeatTrack,
    repeatAlbum,
    nativeOrder,
    keepPlaying,
    none,
};

enum class AlbumRepeatMode : std::uint8_t {
    off,
    track,
    album,
};

struct AlbumSequencePolicy {
    AlbumRepeatMode repeat = AlbumRepeatMode::off;
    bool shuffle = false;
    std::uint64_t seed = 0;
};

[[nodiscard]] std::vector<std::size_t> makeAlbumPermutation(
    std::size_t count,
    std::size_t current,
    AlbumSequencePolicy policy);
[[nodiscard]] std::optional<std::size_t> resolveAlbumPolicyIndex(
    const std::vector<std::size_t>& permutation,
    std::size_t current,
    int direction,
    AlbumRepeatMode repeat) noexcept;

struct PlaybackPrecedenceInput {
    bool userStop = false;
    bool manualQueue = false;
    bool repeatTrack = false;
    bool repeatAlbum = false;
    bool nativeOrder = false;
    bool keepPlaying = false;
};

[[nodiscard]] constexpr PlaybackPrecedence resolvePlaybackPrecedence(
    PlaybackPrecedenceInput input) noexcept
{
    if (input.userStop) return PlaybackPrecedence::userStop;
    if (input.manualQueue) return PlaybackPrecedence::manualQueue;
    if (input.repeatTrack) return PlaybackPrecedence::repeatTrack;
    if (input.repeatAlbum) return PlaybackPrecedence::repeatAlbum;
    if (input.nativeOrder) return PlaybackPrecedence::nativeOrder;
    if (input.keepPlaying) return PlaybackPrecedence::keepPlaying;
    return PlaybackPrecedence::none;
}

[[nodiscard]] constexpr std::optional<std::size_t> resolveAlbumLocalIndex(
    std::size_t current, std::size_t count, int direction) noexcept
{
    if (current >= count || (direction < 0 && current == 0) ||
        (direction > 0 && current + 1U >= count) || direction == 0) {
        return std::nullopt;
    }
    return direction < 0 ? current - 1U : current + 1U;
}

class AlbumPlaylistPreparationLease final {
public:
    [[nodiscard]] bool needsPreparation(
        std::uint64_t sessionId,
        std::uint64_t compositionRevision) const noexcept
    {
        return externallyModified_ || sessionId_ != sessionId ||
            compositionRevision_ != compositionRevision;
    }
    void prepared(
        std::uint64_t sessionId,
        std::uint64_t compositionRevision) noexcept
    {
        sessionId_ = sessionId;
        compositionRevision_ = compositionRevision;
        externallyModified_ = false;
    }
    void externallyModified() noexcept { externallyModified_ = true; }
    void clear() noexcept
    {
        sessionId_ = 0;
        compositionRevision_ = 0;
        externallyModified_ = false;
    }

private:
    std::uint64_t sessionId_ = 0;
    std::uint64_t compositionRevision_ = 0;
    bool externallyModified_ = false;
};

// A deferred EOF action belongs to the transport intent that created it.
// Later user actions must not be overwritten merely because the album stayed.
struct AlbumContinuationToken {
    std::uint64_t intent = 0, request = 0, session = 0, composition = 0;
};
[[nodiscard]] constexpr bool albumContinuationAllowed(
    AlbumContinuationToken captured, AlbumContinuationToken current,
    bool ready, bool lease, bool outside, bool queued) noexcept
{
    return ready && lease && !outside && !queued &&
        captured.intent == current.intent && captured.request == current.request &&
        captured.session == current.session && captured.composition == current.composition;
}

} // namespace nekorox
