#include "listen_window.h"
#include "album_session_service.h"
#include "mainmenu.h"
#include "database_service.h"
#include "daw_window.h"
#include "keep_playing_service.h"
#include "note_editor.h"
#include "options_window.h"
#include "persona_editor.h"
#include "sdk_compat.h"
#include "ui_preferences.h"
#include "workspace_notifications.h"

#include <array>
#include <limits>
#include <vector>

namespace nekorox::component {
namespace {

const GUID kMenuGroup = {0xe2426d58, 0x137b, 0x438a, {0x85, 0xe0, 0xb4, 0x16, 0xa5, 0x63, 0x7f, 0x30}};
const GUID kListenCommand = {0xc87a1f7b, 0xeb17, 0x4c51, {0xae, 0x5c, 0x37, 0x83, 0x83, 0x46, 0xb0, 0xdc}};
const GUID kNowPlayingCommand = {0xf959ef93, 0x7149, 0x4f42, {0xa2, 0x44, 0x10, 0x35, 0x25, 0x1d, 0x45, 0x55}};
const GUID kFullMenuCommand = {0x4af44b1e, 0x8260, 0x4d9b, {0xbe, 0x23, 0x3d, 0x58, 0x22, 0x6f, 0x48, 0xd4}};
const GUID kQueueCommand = {0xc66f3617, 0xb304, 0x44fe, {0xbd, 0xd5, 0xc2, 0x52, 0xd5, 0x9e, 0xb3, 0x34}};
const GUID kPaletteCommand = {0x43ffbd24, 0x0349, 0x474e, {0x8a, 0xa0, 0x18, 0x30, 0x0a, 0xe8, 0x1a, 0x36}};
const GUID kHistoryCommand = {0x69e347ee, 0xa3b5, 0x47ef, {0x8c, 0xd0, 0x7a, 0x62, 0xb1, 0x7c, 0x63, 0x41}};
const GUID kFavoriteCommand = {0xc5d44e54, 0x831f, 0x4699, {0x9b, 0x7a, 0x19, 0x87, 0xe8, 0xe0, 0x41, 0x22}};
const GUID kNoteCommand = {0xbefee52f, 0x7571, 0x4bc3, {0xbc, 0x55, 0xdb, 0xb0, 0x4e, 0xe6, 0xf8, 0x93}};
const GUID kPersonaCommand = {0x8f334444, 0x765b, 0x4932, {0xa4, 0xda, 0x96, 0xbb, 0x3a, 0x67, 0xfe, 0x21}};
const GUID kMotionGroup = {0x636088be, 0x2c0b, 0x4af0, {0x9e, 0x89, 0xdf, 0x22, 0x3a, 0xa2, 0xbb, 0x0a}};
const GUID kMotionNormal = {0x0be60b76, 0x36fd, 0x432c, {0x88, 0x20, 0xaa, 0x04, 0x4b, 0x92, 0x69, 0x72}};
const GUID kMotionFollowWindows = {0x3e9d16b4, 0x8d7c, 0x4ff1, {0x93, 0xc2, 0xb4, 0x37, 0x45, 0x72, 0xec, 0x6b}};
const GUID kMotionReduced = {0x04382917, 0xb18b, 0x4a4b, {0xb7, 0xd5, 0xfe, 0x32, 0x65, 0xaa, 0xfe, 0x42}};
const GUID kMotionOff = {0xddb0854c, 0x524a, 0x4643, {0xa9, 0x27, 0x54, 0x2a, 0x7a, 0x6b, 0x57, 0x3d}};
const GUID kHeaderWordmarkCommand = {0x27cd3c0d, 0x5111, 0x4383,
    {0x92, 0x2e, 0xee, 0x51, 0x18, 0x0c, 0x66, 0x4b}};
const GUID kFallbackAccentCommand = {0xe6ef6141, 0xe343, 0x41a4,
    {0xa9, 0x70, 0xa1, 0x42, 0x98, 0xe9, 0x79, 0xca}};
const GUID kOptionsCommand = {0xc558f6d5, 0x0e96, 0x477f,
    {0xa2, 0x43, 0x3c, 0x29, 0x69, 0xb4, 0x9c, 0xbc}};
const GUID kDawCommand = {0xb791c393, 0xd1be, 0x48e9,
    {0xb6, 0x7f, 0x9c, 0x6d, 0xe9, 0xed, 0xc5, 0x1e}};
const GUID kNextCommand = {0x4ed41497, 0x654b, 0x4c22,
    {0xb0, 0xb8, 0x50, 0xf2, 0xb4, 0x4a, 0xe7, 0x3c}};
const GUID kAlbumCommand = {0xb1b85a22, 0x424f, 0x4a95,
    {0xb9, 0x6d, 0x09, 0x72, 0x7a, 0x82, 0xd6, 0xa3}};

void showNowPlaying()
{
    if (revealNowPlayingInWorkspace()) return;
    auto playlists = playlist_manager::get();
    t_size playlist = std::numeric_limits<t_size>::max();
    t_size item = std::numeric_limits<t_size>::max();
    if (!playlists->get_playing_item_location(&playlist, &item)) return;
    playlists->set_active_playlist(playlist);
    playlists->playlist_clear_selection(playlist);
    playlists->playlist_set_selection_single(playlist, item, true);
    playlists->playlist_set_focus_item(playlist, item);
    playlists->playlist_ensure_visible(playlist, item);
}

struct MenuBand {
    const wchar_t* label;
    GUID root;
    UINT base;
    service_ptr_t<mainmenu_manager> manager;
};

void showFullMenuAt(HWND owner, POINT point)
{
    auto parent = GetAncestor(owner, GA_ROOT);
    if (parent == nullptr) parent = static_cast<HWND>(core_api::get_main_window());
    std::array<MenuBand, 6> bands{{
        {L"File", mainmenu_groups::file, 1000, {}},
        {L"Edit", mainmenu_groups::edit, 2000, {}},
        {L"View", mainmenu_groups::view, 3000, {}},
        {L"Playback", mainmenu_groups::playback, 4000, {}},
        {L"Library", mainmenu_groups::library, 5000, {}},
        {L"Help", mainmenu_groups::help, 6000, {}},
    }};
    HMENU root = CreatePopupMenu();
    if (root == nullptr) return;
    constexpr UINT kRange = 900;
    constexpr UINT kFlags = mainmenu_manager::flag_show_shortcuts |
        mainmenu_manager::flag_show_shortcuts_global |
        mainmenu_manager::flag_view_full;
    for (auto& band : bands) {
        HMENU submenu = CreatePopupMenu();
        if (submenu == nullptr) continue;
        standard_api_create_t(band.manager);
        band.manager->instantiate(band.root);
        band.manager->generate_menu_win32(
            submenu, band.base, kRange, kFlags);
        AppendMenuW(
            root,
            MF_POPUP | MF_STRING,
            reinterpret_cast<UINT_PTR>(submenu),
            band.label);
    }

    // A popup invoked by the custom header must establish the root as the
    // active menu owner explicitly. This is especially important after a
    // remote-session reconnect or after closing an unowned DAW/DSP window,
    // where the foreground window can still be transitioning.
    SetForegroundWindow(parent);
    SetActiveWindow(parent);
    const UINT selected = TrackPopupMenuEx(
        root,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_RECURSE,
        point.x,
        point.y,
        parent,
        nullptr);
    // Required by the Win32 foreground-menu contract so the next click is not
    // consumed by a popup that has just left its modal loop.
    PostMessageW(parent, WM_NULL, 0, 0);
    for (auto& band : bands) {
        if (selected >= band.base && selected < band.base + kRange &&
            band.manager.is_valid()) {
            band.manager->execute_command(selected - band.base);
            break;
        }
    }
    DestroyMenu(root);
}

void showFullMenu()
{
    POINT point{};
    GetCursorPos(&point);
    showFullMenuAt(static_cast<HWND>(core_api::get_main_window()), point);
}

mainmenu_group_popup_factory g_menuGroup(
    kMenuGroup,
    mainmenu_groups::view,
    mainmenu_commands::sort_priority_dontcare,
    "GODENplayer");

mainmenu_group_popup_factory g_motionGroup(
    kMotionGroup,
    kMenuGroup,
    mainmenu_commands::sort_priority_last,
    "Motion");

class MotionCommands : public mainmenu_commands {
public:
    t_uint32 get_command_count() override { return 4; }
    GUID get_command(t_uint32 index) override
    {
        if (index == 0) return kMotionNormal;
        if (index == 1) return kMotionReduced;
        if (index == 2) return kMotionOff;
        if (index == 3) return kMotionFollowWindows;
        uBugCheck();
    }
    void get_name(t_uint32 index, pfc::string_base& output) override
    {
        if (index == 0) output = "Normal";
        else if (index == 1) output = "Reduced";
        else if (index == 2) output = "Off";
        else if (index == 3) output = "Follow Windows";
        else uBugCheck();
    }
    bool get_description(t_uint32 index, pfc::string_base& output) override
    {
        if (index == 0) output = "Use full short GODENplayer motion and inertial scrolling.";
        else if (index == 1) output = "Use shorter motion with reduced travel.";
        else if (index == 2) output = "Disable nonessential UI motion.";
        else if (index == 3) output = "Reduce nonessential motion when Windows animations are disabled.";
        else return false;
        return true;
    }
    bool get_display(t_uint32 index, pfc::string_base& output,
        t_uint32& flags) override
    {
        get_name(index, output);
        flags = static_cast<unsigned>(motionPreference()) == index
            ? flag_radiochecked : 0;
        return true;
    }
    GUID get_parent() override { return kMotionGroup; }
    void execute(t_uint32 index, service_ptr_t<service_base>) override
    {
        if (index < 4) setMotionPreference(static_cast<MotionPreference>(index));
    }
};

FB2K_SERVICE_FACTORY(MotionCommands);

class Commands : public mainmenu_commands {
public:
    t_uint32 get_command_count() override { return 15; }
    GUID get_command(t_uint32 index) override
    {
        if (index == 0) return kListenCommand;
        if (index == 1) return kNowPlayingCommand;
        if (index == 2) return kFullMenuCommand;
        if (index == 3) return kQueueCommand;
        if (index == 4) return kPaletteCommand;
        if (index == 5) return kHistoryCommand;
        if (index == 6) return kFavoriteCommand;
        if (index == 7) return kNoteCommand;
        if (index == 8) return kPersonaCommand;
        if (index == 9) return kHeaderWordmarkCommand;
        if (index == 10) return kFallbackAccentCommand;
        if (index == 11) return kOptionsCommand;
        if (index == 12) return kDawCommand;
        if (index == 13) return kNextCommand;
        if (index == 14) return kAlbumCommand;
        uBugCheck();
    }
    void get_name(t_uint32 index, pfc::string_base& output) override
    {
        if (index == 0) output = "Listen";
        else if (index == 1) output = "Show now playing";
        else if (index == 2) output = "Menu";
        else if (index == 3) output = "Playback queue";
        else if (index == 4) output = "Command palette";
        else if (index == 5) output = "History";
        else if (index == 6) output = "Toggle favorite";
        else if (index == 7) output = "Edit note";
        else if (index == 8) output = "Edit persona";
        else if (index == 9) output = "Show header identity";
        else if (index == 10) output = "Fallback signature colour...";
        else if (index == 11) output = "Options...";
        else if (index == 12) output = "DAW Mode...";
        else if (index == 13) output = "Next track";
        else if (index == 14) output = "Album mode";
        else uBugCheck();
    }
    bool get_description(t_uint32 index, pfc::string_base& output) override
    {
        if (index == 0) output = "Open the local GODENplayer Listen window.";
        else if (index == 1) output = "Reveal and focus the playing playlist item.";
        else if (index == 2) output = "Open the complete foobar2000 command menu.";
        else if (index == 3) output = "Open or close the native GODENplayer playback queue.";
        else if (index == 4) output = "Search local music and GODENplayer actions.";
        else if (index == 5) output = "Open the current GODENplayer listening session.";
        else if (index == 6) output = "Add or remove the current track from GODENplayer favorites.";
        else if (index == 7) output = "Edit the private local note for the current track.";
        else if (index == 8) output = "Edit the local artist persona, aliases and relationships.";
        else if (index == 9) output = "Show the artwork-colored GODENplayer wordmark beside Menu.";
        else if (index == 10) output = "Choose the RGB signature colour used when artwork has no usable palette.";
        else if (index == 11) output = "Open the central GODENplayer high-level settings window.";
        else if (index == 12) output = "Open the GODENplayer sample and stem arrangement workspace.";
        else if (index == 13) output = "Advance within the album, then apply GODENplayer playback continuity at its boundary.";
        else if (index == 14) output = "Open or close the explicit album listening context.";
        else return false;
        return true;
    }
    bool get_display(t_uint32 index, pfc::string_base& output,
        t_uint32& flags) override
    {
        get_name(index, output);
        flags = index == 9 && headerWordmarkEnabled()
            ? flag_checked : 0;
        return true;
    }
    GUID get_parent() override { return kMenuGroup; }
    void execute(t_uint32 index, service_ptr_t<service_base>) override
    {
        if (index == 0) ListenWindow::instance().show();
        else if (index == 1) showNowPlaying();
        else if (index == 2) showFullMenu();
        else if (index == 3) toggleWorkspaceQueue();
        else if (index == 4) openWorkspaceCommandPalette();
        else if (index == 5) showWorkspaceHistory();
        else if (index == 6) DatabaseService::instance().toggleCurrentFavorite();
        else if (index == 7) NoteEditorWindow::instance().show(
            static_cast<HWND>(core_api::get_main_window()));
        else if (index == 8) PersonaEditorWindow::instance().show(
            static_cast<HWND>(core_api::get_main_window()));
        else if (index == 9) setHeaderWordmarkEnabled(
            !headerWordmarkEnabled());
        else if (index == 10) chooseFallbackAccentColor(
            static_cast<HWND>(core_api::get_main_window()));
        else if (index == 11) OptionsWindow::instance().show(
            static_cast<HWND>(core_api::get_main_window()));
        else if (index == 12) DawWindow::instance().show(
            static_cast<HWND>(core_api::get_main_window()));
        else if (index == 13) {
            if (!KeepPlayingService::instance().handleNextCommand()) {
                playback_control::get()->next();
            }
        }
        else if (index == 14) {
            AlbumSessionService::instance().toggleFromHostContext();
        }
    }
};

FB2K_SERVICE_FACTORY(Commands);

} // namespace

void showMainMenu(HWND owner, POINT screenPoint)
{
    showFullMenuAt(owner, screenPoint);
}

} // namespace nekorox::component
