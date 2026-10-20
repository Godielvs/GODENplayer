#include "file_open_service.h"

#include <nekorox/qa_contracts.h>

#include "keep_playing_service.h"
#include "text_util.h"
#include "workspace_notifications.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace nekorox::component {
namespace {

constexpr char kTemporaryAlbumPlaylistName[] = "GODENplayer Temporary Album";
constexpr char kLegacyGodenTemporaryAlbumPlaylistName[] =
    "Goden Temporary Album";
constexpr char kLegacyTemporaryAlbumPlaylistName[] =
    "NekoRoX Temporary Album";
constexpr char kLegacyOpenPlaylistName[] = "NekoRoX Open";
constexpr char kOpenCommand[] = "/nekorox-open";

std::atomic<std::uint64_t> g_openRequest{0};
std::atomic<std::uint32_t> g_fileOpenQaState{0};
std::atomic<std::uint64_t> g_temporaryAlbumGeneration{1};
GUID g_temporaryAlbumGuid = GUID_NULL;
const GUID kTemporaryOwnershipConfig = {0xa54239ce,0x4c29,0x494e,{0xbb,0x52,0x21,0x10,0x8f,0x35,0xab,0x0c}};
cfg_guid g_ownedTemporaryPlaylist(kTemporaryOwnershipConfig, GUID_NULL);
bool g_temporaryAlbumActive = false;
bool g_temporaryAlbumCheckQueued = false;

void observeFileOpen(std::uint32_t bit) noexcept
{
    g_fileOpenQaState.fetch_or(bit, std::memory_order_release);
}

void publishTemporaryAlbumState(
    bool active,
    std::size_t itemCount,
    bool dismissed = false) noexcept
{
    const auto clampedCount = static_cast<std::uint32_t>(
        std::min<std::size_t>(
            itemCount,
            nekorox::kFileOpenTemporaryAlbumCountMaskV1));
    const auto replacement =
        (active ? nekorox::kFileOpenTemporaryAlbumActiveBitV1 : 0U) |
        (dismissed ? nekorox::kFileOpenTemporaryAlbumDismissedBitV1 : 0U) |
        (clampedCount << nekorox::kFileOpenTemporaryAlbumCountShiftV1);
    constexpr auto mask =
        nekorox::kFileOpenTemporaryAlbumActiveBitV1 |
        nekorox::kFileOpenTemporaryAlbumDismissedBitV1 |
        (nekorox::kFileOpenTemporaryAlbumCountMaskV1 <<
            nekorox::kFileOpenTemporaryAlbumCountShiftV1);
    auto current = g_fileOpenQaState.load(std::memory_order_acquire);
    for (;;) {
        const auto next = (current & ~mask) | replacement;
        if (g_fileOpenQaState.compare_exchange_weak(
                current,
                next,
                std::memory_order_release,
                std::memory_order_acquire)) {
            return;
        }
    }
}

t_size temporaryAlbumPlaylist() noexcept
{
    if (!g_temporaryAlbumActive || InlineIsEqualGUID(
            g_temporaryAlbumGuid, GUID_NULL)) {
        return pfc_infinite;
    }
    try {
        return playlist_manager_v5::get()->find_playlist_by_guid(
            g_temporaryAlbumGuid);
    } catch (...) {
        return pfc_infinite;
    }
}

void notifyTemporaryAlbumChanged() noexcept
{
    g_temporaryAlbumGeneration.fetch_add(1, std::memory_order_acq_rel);
    notifyWorkspaceModelChanged();
}

void dismissTemporaryAlbum(bool dismissed) noexcept
{
    const bool wasActive = g_temporaryAlbumActive;
    try {
        const auto playlist = InlineIsEqualGUID(g_temporaryAlbumGuid,GUID_NULL)
            ? pfc_infinite : playlist_manager_v5::get()->find_playlist_by_guid(g_temporaryAlbumGuid);
        if (playlist != pfc_infinite) {
            if (!playlist_manager::get()->remove_playlist_switch(playlist)) return;
        }
    } catch (...) {
        return; // Retain proof of ownership so a failed removal can be retried.
    }
    g_temporaryAlbumActive = false;
    g_temporaryAlbumGuid = GUID_NULL;
    g_ownedTemporaryPlaylist = GUID_NULL;
    publishTemporaryAlbumState(false, 0, dismissed);
    if (wasActive) notifyTemporaryAlbumChanged();
}

void playIncomingItems(
    metadb_handle_list_cref items,
    std::uint64_t request)
{
    if (items.get_count() == 0 ||
        request != g_openRequest.load(std::memory_order_acquire)) {
        return;
    }
    observeFileOpen(nekorox::kFileOpenItemsResolvedBitV1);

    auto playlists = playlist_manager::get();
    auto playlist = InlineIsEqualGUID(g_temporaryAlbumGuid,GUID_NULL)
        ? pfc_infinite : playlist_manager_v5::get()->find_playlist_by_guid(g_temporaryAlbumGuid);
    if (playlist == pfc_infinite) playlist = playlists->create_playlist(
        kTemporaryAlbumPlaylistName,pfc_infinite,pfc_infinite);
    if (playlist == pfc_infinite) return;
    g_temporaryAlbumGuid = playlist_manager_v5::get()->playlist_get_guid(playlist);
    g_ownedTemporaryPlaylist = g_temporaryAlbumGuid;

    playlists->playlist_undo_backup(playlist);
    playlists->playlist_clear(playlist);
    const auto inserted = playlists->playlist_insert_items(
        playlist, 0, items, bit_array_false());
    if (inserted == pfc_infinite) return;

    playlists->set_active_playlist(playlist);
    playlists->playlist_clear_selection(playlist);
    playlists->playlist_set_selection_single(playlist, 0, true);
    playlists->playlist_set_focus_item(playlist, 0);

    g_temporaryAlbumGuid =
        playlist_manager_v5::get()->playlist_get_guid(playlist);
    g_temporaryAlbumActive = true;
    publishTemporaryAlbumState(true, items.get_count());
    notifyTemporaryAlbumChanged();
    // A temporary playlist owns its order even when its handles also belong
    // to the library. Do not retain a previous album's continuation context.
    KeepPlayingService::instance().clearContext();

    const auto mainWindow = static_cast<HWND>(core_api::get_main_window());
    if (mainWindow != nullptr) {
        if (IsIconic(mainWindow)) ShowWindow(mainWindow, SW_RESTORE);
        ShowWindow(mainWindow, SW_SHOW);
        SetForegroundWindow(mainWindow);
    }
    if (playlists->playlist_execute_default_action(playlist, 0)) {
        observeFileOpen(nekorox::kFileOpenPlaybackStartedBitV1);
    }
}

void openIncomingUrls(
    std::vector<std::string> urls,
    HWND owner)
{
    urls.erase(
        std::remove_if(urls.begin(), urls.end(),
            [](const std::string& value) { return value.empty(); }),
        urls.end());
    if (urls.empty()) return;
    observeFileOpen(nekorox::kFileOpenRequestStartedBitV1);

    pfc::list_t<const char*> input;
    for (const auto& url : urls) input.add_item(url.c_str());
    const auto request =
        g_openRequest.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto notify = process_locations_notify::create(
        [request](metadb_handle_list_cref items) {
            observeFileOpen(nekorox::kFileOpenCallbackBitV1);
            // Superseded opens can resolve large batches. Reject them before
            // copying handles; playIncomingItems rechecks after dispatch.
            if (request != g_openRequest.load(std::memory_order_acquire)) return;
            auto ownedItems = std::make_shared<metadb_handle_list>();
            ownedItems->add_items(items);
            // Escape process_locations' global callback before mutating a
            // playlist or invoking its default playback action.
            fb2k::inMainThread([request, ownedItems] {
                playIncomingItems(*ownedItems, request);
            });
        });
    playlist_incoming_item_filter_v2::get()->process_locations_async(
        input,
        playlist_incoming_item_filter_v2::op_flag_delay_ui |
            playlist_incoming_item_filter_v2::op_flag_no_filter,
        nullptr,
        nullptr,
        owner,
        std::move(notify));
}

class NekoRoxOpenCommandline : public commandline_handler {
public:
    result on_token(const char* token) override
    {
        if (token != nullptr &&
            (_stricmp(token, kOpenCommand) == 0 ||
             _stricmp(token, kOpenCommand + 1) == 0)) {
            return RESULT_PROCESSED_EXPECT_FILES;
        }
        return RESULT_NOT_OURS;
    }

    void on_file(const char* url) override
    {
        if (url != nullptr && *url != '\0') urls_.emplace_back(url);
    }

    void on_files_done() override
    {
        openIncomingUrls(
            std::move(urls_),
            static_cast<HWND>(core_api::get_main_window()));
    }

    bool want_directories() override
    {
        return true;
    }

private:
    std::vector<std::string> urls_;
};

commandline_handler_factory_t<NekoRoxOpenCommandline>
    g_openCommandlineFactory;

} // namespace

bool openDroppedFiles(HWND owner, HDROP drop)
{
    if (drop == nullptr) return false;
    observeFileOpen(nekorox::kFileOpenDropObservedBitV1);
    const auto count = DragQueryFileW(drop, 0xffffffffU, nullptr, 0);
    std::vector<std::string> urls;
    urls.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        const auto length = DragQueryFileW(drop, index, nullptr, 0);
        if (length == 0) continue;
        std::wstring path(static_cast<std::size_t>(length) + 1U, L'\0');
        const auto copied = DragQueryFileW(
            drop,
            index,
            path.data(),
            static_cast<UINT>(path.size()));
        if (copied == 0) continue;
        path.resize(copied);

        const auto utf8 = wideToUtf8(path);
        if (utf8.empty()) continue;
        urls.emplace_back(utf8);
    }
    DragFinish(drop);
    if (urls.empty()) return false;
    observeFileOpen(nekorox::kFileOpenDropExtractedBitV1);
    openIncomingUrls(std::move(urls), owner);
    return true;
}

bool openDroppedFilePath(HWND owner, std::wstring_view path)
{
    observeFileOpen(nekorox::kFileOpenDropObservedBitV1);
    const auto utf8 = wideToUtf8(path);
    if (utf8.empty()) return false;
    observeFileOpen(nekorox::kFileOpenDropExtractedBitV1);
    openIncomingUrls({utf8}, owner);
    return true;
}

std::uint32_t fileOpenQaState() noexcept
{
    return g_fileOpenQaState.load(std::memory_order_acquire);
}

bool temporaryAlbumItems(metadb_handle_list& output) noexcept
{
    output.remove_all();
    try {
        const auto playlist = temporaryAlbumPlaylist();
        if (playlist == pfc_infinite) return false;
        playlist_manager::get()->playlist_get_all_items(playlist, output);
        return output.get_count() > 0;
    } catch (...) {
        output.remove_all();
        return false;
    }
}

std::uint64_t temporaryAlbumGeneration() noexcept
{
    return g_temporaryAlbumGeneration.load(std::memory_order_acquire);
}

bool playTemporaryAlbumItem(std::size_t itemIndex) noexcept
{
    try {
        const auto playlist = temporaryAlbumPlaylist();
        auto playlists = playlist_manager::get();
        if (playlist == pfc_infinite ||
            itemIndex >= playlists->playlist_get_item_count(playlist)) {
            return false;
        }
        playlists->set_active_playlist(playlist);
        playlists->playlist_clear_selection(playlist);
        playlists->playlist_set_selection_single(playlist, itemIndex, true);
        playlists->playlist_set_focus_item(playlist, itemIndex);
        return playlists->playlist_execute_default_action(
            playlist, itemIndex);
    } catch (...) {
        return false;
    }
}

// Main-thread-only occurrence lookup, verified against the owned playlist.
std::optional<std::size_t> temporaryAlbumPlayingItem() noexcept
{
    try {
        const auto temporary = temporaryAlbumPlaylist();
        t_size playlist = pfc_infinite, item = pfc_infinite;
        if (temporary != pfc_infinite &&
                playlist_manager::get()->get_playing_item_location(&playlist, &item) &&
                playlist == temporary && item != pfc_infinite) {
            return static_cast<std::size_t>(item);
        }
    } catch (...) {}
    return std::nullopt;
}

void initializeFileOpenService() noexcept
{
    g_temporaryAlbumActive = false;
    g_temporaryAlbumGuid = g_ownedTemporaryPlaylist.get();
    g_temporaryAlbumCheckQueued = false;
    g_temporaryAlbumGeneration.fetch_add(1, std::memory_order_acq_rel);
    // A name is not ownership. Legacy/same-name user playlists are preserved.
    dismissTemporaryAlbum(false);
    publishTemporaryAlbumState(false, 0);
}

void shutdownFileOpenService() noexcept
{
    ++g_openRequest;
    dismissTemporaryAlbum(false);
}

void notifyTemporaryAlbumPlaybackChanged() noexcept
{
    if (!g_temporaryAlbumActive || g_temporaryAlbumCheckQueued ||
        core_api::is_shutting_down()) {
        return;
    }
    g_temporaryAlbumCheckQueued = true;
    fb2k::inMainThread([] {
        g_temporaryAlbumCheckQueued = false;
        if (!g_temporaryAlbumActive || core_api::is_shutting_down()) return;

        const auto temporary = temporaryAlbumPlaylist();
        if (temporary == pfc_infinite) {
            g_temporaryAlbumActive = false;
            g_temporaryAlbumGuid = GUID_NULL;
            publishTemporaryAlbumState(false, 0, true);
            notifyTemporaryAlbumChanged();
            return;
        }
        t_size playingPlaylist = pfc_infinite;
        t_size playingItem = pfc_infinite;
        if (!playlist_manager::get()->get_playing_item_location(
                &playingPlaylist, &playingItem) ||
            playingPlaylist != temporary) {
            dismissTemporaryAlbum(true);
        }
    });
}

} // namespace nekorox::component
