#include "keep_playing_service.h"

#include "file_open_service.h"
#include "library_model.h"
#include "ui_preferences.h"
#include "workspace_notifications.h"
#include "workspace_view_model.h"

#include <algorithm>
#include <atomic>
#include <numeric>
#include <random>

namespace nekorox::component {
namespace {

const GUID kKeepPlayingEnabledGuid = {0xf9f8dfaa, 0xd453, 0x4d43,
    {0xa8, 0x6d, 0x8a, 0xd2, 0xc0, 0x41, 0x7e, 0xb8}};
const GUID kKeepPlayingModeGuid = {0x8c30e075, 0x8f75, 0x480c,
    {0x85, 0x4d, 0xe0, 0xc2, 0x55, 0xca, 0x10, 0xd8}};
cfg_bool g_keepPlayingEnabled(kKeepPlayingEnabledGuid, true);
cfg_uint g_keepPlayingMode(kKeepPlayingModeGuid,
    static_cast<unsigned>(KeepPlayingMode::randomAlbums));
std::atomic<bool> g_runtimeEnabled{true};
std::atomic<KeepPlayingMode> g_runtimeMode{KeepPlayingMode::randomAlbums};

KeepPlayingMode sanitizeMode(unsigned value) noexcept
{
    return value == static_cast<unsigned>(KeepPlayingMode::sequentialAlbums)
        ? KeepPlayingMode::sequentialAlbums
        : KeepPlayingMode::randomAlbums;
}

std::mt19937& randomEngine()
{
    static std::mt19937 engine([] {
        std::random_device device;
        std::seed_seq seed{device(), device(), device(), device()};
        return std::mt19937(seed);
    }());
    return engine;
}

} // namespace

KeepPlayingService::KeepPlayingService()
{
    g_runtimeEnabled.store(
        static_cast<bool>(g_keepPlayingEnabled),
        std::memory_order_relaxed);
    g_runtimeMode.store(
        sanitizeMode(static_cast<unsigned>(g_keepPlayingMode)),
        std::memory_order_relaxed);
}

KeepPlayingService& KeepPlayingService::instance()
{
    static KeepPlayingService value;
    return value;
}

bool KeepPlayingService::enabled() const noexcept
{
    return g_runtimeEnabled.load(std::memory_order_acquire);
}

void KeepPlayingService::setEnabled(bool enabledValue)
{
    g_keepPlayingEnabled = enabledValue;
    g_runtimeEnabled.store(enabledValue, std::memory_order_release);
    ++generation_;
    notifyNativePanelsPlaybackChanged();
}

KeepPlayingMode KeepPlayingService::mode() const noexcept
{
    return g_runtimeMode.load(std::memory_order_acquire);
}

void KeepPlayingService::setMode(KeepPlayingMode next)
{
    next = sanitizeMode(static_cast<unsigned>(next));
    g_keepPlayingMode = static_cast<unsigned>(next);
    g_runtimeMode.store(next, std::memory_order_release);
    randomBag_.clear();
    ++generation_;
    notifyNativePanelsPlaybackChanged();
}

void KeepPlayingService::captureLibraryContext(
    std::vector<PlaybackAlbum> albums,
    std::size_t currentAlbum)
{
    std::erase_if(albums, [](const auto& album) { return album.empty(); });
    albums_ = std::move(albums);
    indexContext();
    currentAlbum_ = albums_.empty()
        ? static_cast<std::size_t>(-1)
        : std::min(currentAlbum, albums_.size() - 1);
    randomBag_.clear();
    continuationInFlight_ = false;
    fullLibraryContext_ = false;
    expectedTrack_.release();
    nativeNextBoundaryPending_ = false;
    nativeNextExpectedTrack_.release();
    ++generation_;
}

void KeepPlayingService::clearContext()
{
    albums_.clear();
    albumByTrack_.clear();
    randomBag_.clear();
    currentAlbum_ = static_cast<std::size_t>(-1);
    continuationInFlight_ = false;
    fullLibraryContext_ = false;
    expectedTrack_.release();
    nativeNextBoundaryPending_ = false;
    nativeNextExpectedTrack_.release();
    ++generation_;
}

bool KeepPlayingService::albumContains(
    const PlaybackAlbum& album,
    const metadb_handle_ptr& track) const
{
    if (!track.is_valid()) return false;
    return std::find(album.begin(), album.end(), track) != album.end();
}

void KeepPlayingService::onPlaybackStarting(
    play_control::t_track_command command)
{
    // Every explicit/native transport start supersedes previously deferred
    // continuations, even before its new-track callback reaches the observer.
    ++generation_;
    nativeNextBoundaryPending_ = false;
    nativeNextExpectedTrack_.release();
    if (command != play_control::track_command_next ||
        playlist_manager::get()->queue_get_count() != 0 ||
        temporaryAlbumPlayingItem().has_value()) {
        return;
    }
    metadb_handle_ptr current;
    if (!playback_control::get()->get_now_playing(current) ||
        !current.is_valid()) {
        return;
    }
    if (const auto entry = albumByTrack_.find(current.get_ptr());
            entry != albumByTrack_.end() && entry->second < albums_.size()) {
        const auto index = entry->second;
        const auto& album = albums_[index];
        const auto found = std::find(album.begin(), album.end(), current);
        if (found == album.end()) return;
        currentAlbum_ = index;
        nativeNextBoundaryPending_ = std::next(found) == album.end();
        if (nativeNextBoundaryPending_) nativeNextExpectedTrack_ = album.front();
        return;
    }
}

void KeepPlayingService::onNewTrack(const metadb_handle_ptr& track)
{
    const auto token = ++generation_;
    const bool wrapped = nativeNextBoundaryPending_ &&
        nativeNextExpectedTrack_.is_valid() && nativeNextExpectedTrack_ == track;
    nativeNextBoundaryPending_ = false;
    nativeNextExpectedTrack_.release();
    if (wrapped) {
        // A built-in foobar hotkey/menu command cannot be cancelled from its
        // global callback. Let it settle, then replace only the proven
        // last->first album wrap through the same continuation path.
        fb2k::inMainThread([this, token] {
            resolveNativeNextBoundary(token);
        });
    }
    if (continuationInFlight_ && expectedTrack_.is_valid() &&
            expectedTrack_ == track) {
        continuationInFlight_ = false;
        expectedTrack_.release();
        return;
    }
    if (const auto found = albumByTrack_.find(track.get_ptr()); found != albumByTrack_.end()) {
            currentAlbum_ = found->second;
            continuationInFlight_ = false;
            expectedTrack_.release();
            return;
    }
    // Temporary albums and external files intentionally have no captured
    // library context; EOF will then bootstrap the full library.
    albums_.clear();
    albumByTrack_.clear();
    randomBag_.clear();
    currentAlbum_ = static_cast<std::size_t>(-1);
    continuationInFlight_ = false;
    fullLibraryContext_ = false;
    expectedTrack_.release();
    ++generation_;
}

void KeepPlayingService::onPlaybackStop(play_control::t_stop_reason reason)
{
    if (reason == play_control::stop_reason_user ||
            reason == play_control::stop_reason_shutting_down) {
        ++generation_;
        nativeNextBoundaryPending_ = false;
        nativeNextExpectedTrack_.release();
        continuationInFlight_ = false;
        expectedTrack_.release();
        return;
    }
    if (nativeNextBoundaryPending_ &&
        reason == play_control::stop_reason_eof &&
        playlist_manager::get()->queue_get_count() == 0) {
        nativeNextBoundaryPending_ = false;
        nativeNextExpectedTrack_.release();
        const auto token = ++generation_;
        fb2k::inMainThread([this, token] {
            resolveNativeNextBoundary(token);
        });
        return;
    }
    if (reason != play_control::stop_reason_eof || !enabled()) return;
    if (playlist_manager::get()->queue_get_count() != 0) return;
    const auto token = ++generation_;
    fb2k::inMainThread([this, token] {
        continuePlayback(token);
    });
}

void KeepPlayingService::onQueueChanged()
{
    ++generation_;
    continuationInFlight_ = false;
    nativeNextBoundaryPending_ = false;
    nativeNextExpectedTrack_.release();
    expectedTrack_.release();
}

bool KeepPlayingService::handleNextCommand()
{
    // An explicit playback queue always belongs to foobar2000. Let its normal
    // Next command consume that queue before Keep Playing considers albums.
    if (playlist_manager::get()->queue_get_count() != 0) return false;
    // The caller must preserve every occurrence in the temporary playlist,
    // including repeated library handles after an album's last track.
    if (temporaryAlbumPlayingItem().has_value()) return false;

    const auto control = playback_control::get();
    metadb_handle_ptr currentTrack;
    if (!control->get_now_playing(currentTrack) || !currentTrack.is_valid()) {
        return false;
    }

    auto findCurrentAlbum = [&]() -> std::size_t {
        for (std::size_t index = 0; index < albums_.size(); ++index) {
            if (albumContains(albums_[index], currentTrack)) return index;
        }
        return static_cast<std::size_t>(-1);
    };

    auto albumIndex = findCurrentAlbum();
    if (albumIndex == static_cast<std::size_t>(-1)) {
        ensureFullLibraryContext();
        albumIndex = findCurrentAlbum();
    }
    if (albumIndex == static_cast<std::size_t>(-1)) return false;

    currentAlbum_ = albumIndex;
    const auto& album = albums_[albumIndex];
    const auto track = std::find(album.begin(), album.end(), currentTrack);
    if (track == album.end() || std::next(track) != album.end()) {
        // Inside an album, foobar2000's native Next behavior is still exactly
        // what we want (and retains all of its normal playback semantics).
        return false;
    }

    // The selection playlist contains only the current album, so native Next
    // would wrap to item zero here. Consume this boundary explicitly instead.
    if (!enabled() || !playNextAlbum()) control->stop();
    return true;
}

void KeepPlayingService::stop()
{
    clearContext();
}

void KeepPlayingService::indexContext()
{
    albumByTrack_.clear();
    for (std::size_t index = 0; index < albums_.size(); ++index)
        for (const auto& track : albums_[index])
            if (track.is_valid()) albumByTrack_.try_emplace(track.get_ptr(), index);
}

void KeepPlayingService::ensureFullLibraryContext(bool forceExpand)
{
    if ((!albums_.empty() && !forceExpand) || fullLibraryContext_) return;
    metadb_handle_ptr currentAlbumAnchor;
    if (currentAlbum_ < albums_.size() && !albums_[currentAlbum_].empty()) {
        currentAlbumAnchor = albums_[currentAlbum_].front();
    }
    const auto& tracks = LibraryModel::instance().tracks();
    if (tracks.empty()) return;
    WorkspaceViewModel model;
    FacetSelection genre;
    FacetSelection artist;
    FacetSelection album;
    model.rebuild(tracks, genre, artist, album, L"");
    std::vector<PlaybackAlbum> expanded;
    expanded.reserve(model.groups().size());
    for (const auto& group : model.groups()) {
        PlaybackAlbum handles;
        handles.reserve(group.trackIndices.size());
        for (const auto index : group.trackIndices) {
            if (index < tracks.size() && tracks[index].handle.is_valid()) {
                handles.push_back(tracks[index].handle);
            }
        }
        if (!handles.empty()) expanded.push_back(std::move(handles));
    }
    if (expanded.empty()) return;
    albums_ = std::move(expanded);
    indexContext();
    currentAlbum_ = static_cast<std::size_t>(-1);
    if (currentAlbumAnchor.is_valid()) {
        for (std::size_t index = 0; index < albums_.size(); ++index) {
            if (albumContains(albums_[index], currentAlbumAnchor)) {
                currentAlbum_ = index;
                break;
            }
        }
    }
    randomBag_.clear();
    fullLibraryContext_ = true;
}

void KeepPlayingService::refillRandomBag()
{
    randomBag_.resize(albums_.size());
    std::iota(randomBag_.begin(), randomBag_.end(), 0U);
    std::shuffle(randomBag_.begin(), randomBag_.end(), randomEngine());
    if (randomBag_.size() > 1 && currentAlbum_ != static_cast<std::size_t>(-1) &&
            randomBag_.back() == currentAlbum_) {
        std::swap(randomBag_.back(), randomBag_.front());
    }
}

bool KeepPlayingService::continuePlayback(std::uint64_t token)
{
    if (token != generation_ || !enabled()) return false;
    const auto control = playback_control::get();
    if (control->is_playing() || playlist_manager::get()->queue_get_count() != 0) {
        return false;
    }
    return playNextAlbum();
}

bool KeepPlayingService::resolveNativeNextBoundary(std::uint64_t token)
{
    if (token != generation_ ||
        playlist_manager::get()->queue_get_count() != 0 ||
        temporaryAlbumPlayingItem().has_value() ||
        playback_control::get()->is_paused()) {
        return false;
    }
    if (enabled() && playNextAlbum()) return true;
    playback_control::get()->stop();
    return true;
}

bool KeepPlayingService::playNextAlbum()
{
    ensureFullLibraryContext();
    if (albums_.empty()) return false;

    // A click made while the Album facet is selected captures a perfectly
    // valid one-album context. Neither Random nor Sequence may interpret that
    // as permission to self-loop forever: expand to the library once, preserve
    // the current album identity, then select a genuinely different album.
    if (albums_.size() < 2U) {
        ensureFullLibraryContext(true);
    }
    if (albums_.empty()) return false;

    std::size_t next = 0;
    if (mode() == KeepPlayingMode::sequentialAlbums) {
        next = currentAlbum_ == static_cast<std::size_t>(-1)
            ? 0 : (currentAlbum_ + 1) % albums_.size();
    } else {
        if (randomBag_.empty()) refillRandomBag();
        while (!randomBag_.empty()) {
            next = randomBag_.back();
            randomBag_.pop_back();
            if (albums_.size() == 1 || next != currentAlbum_) break;
        }
    }
    return playAlbum(next);
}

bool KeepPlayingService::playAlbum(std::size_t index)
{
    if (index >= albums_.size() || albums_[index].empty()) return false;
    metadb_handle_list handles;
    for (const auto& handle : albums_[index]) {
        if (handle.is_valid()) handles.add_item(handle);
    }
    if (handles.get_count() == 0) return false;

    const auto playlists = playlist_manager::get();
    const auto playlist = playlists->find_or_create_playlist_unlocked(
        "NekoRoX Selection");
    if (playlist == pfc_infinite) return false;
    playlists->playlist_undo_backup(playlist);
    playlists->playlist_clear(playlist);
    playlists->playlist_insert_items(playlist, 0, handles, bit_array_false());
    playlists->set_active_playlist(playlist);
    playlists->playlist_clear_selection(playlist);
    playlists->playlist_set_selection_single(playlist, 0, true);
    playlists->playlist_set_focus_item(playlist, 0);

    currentAlbum_ = index;
    continuationInFlight_ = true;
    expectedTrack_ = handles[0];
    ++generation_;
    return playlists->playlist_execute_default_action(playlist, 0);
}

} // namespace nekorox::component
