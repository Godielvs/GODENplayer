#include "qa_hooks.h"
#include "art_cache.h"
#include "artwork_transition_renderer.h"
#include "album_session_service.h"
#include "compact_scope_gpu.h"
#include "daw_window.h"
#include "display_environment.h"
#include "frame_clock.h"
#include "frameless_window.h"
#include "glyph_flow_renderer.h"
#include "glyph_flow_service.h"
#include "interaction_latency_guard.h"
#include "keep_playing_service.h"
#include "listen_window.h"
#include "master_dsp_controller.h"
#include "master_dsp_window.h"
#include "mainmenu.h"
#include "motion_coordinator.h"
#include "options_window.h"
#include "packet_cost_controller.h"
#include "panel_d2d.h"
#include "player_shell_panel.h"
#include "realtime_analysis_hub.h"
#include "runtime.h"
#include "rollout_flags.h"
#include "scrub_coordinator.h"
#include "tag_editor_window.h"
#include "text_reconstruction_transition.h"
#include "text_util.h"
#include "theme_service.h"
#include "ui_invalidation_bus.h"
#include "wave_seek_panel.h"
#include "workspace_notifications.h"

#include "nekorox/realtime_visual_model.h"
#include "nekorox/qa_contracts.h"
#include "nekorox/playback_presentation_gate.h"

#include <columns_ui-sdk/ui_extension.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <commctrl.h>
#include <cwctype>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nekorox::component {
namespace {

constexpr UINT kNativeRefreshMessage = WM_APP + 0x4f0;
constexpr UINT kNativeArtworkReadyMessage = WM_APP + 0x4f2;
constexpr UINT kMonitorFrameMessage = WM_APP + 0x4f3;
constexpr UINT kTransportFrameMessage = WM_APP + 0x4f4;
constexpr UINT kGlyphFlowChangedMessage = WM_APP + 0x4f5;
constexpr UINT kGlyphFlowFrameMessage = WM_APP + 0x4f6;
constexpr UINT kCaptionFrameMessage = WM_APP + 0x4f7;
constexpr UINT kThemeTransitionMessage = WM_APP + 0x4f8;
std::atomic<std::uint64_t> gNextHeaderAnalysisLifetime{1};

UINT coverStateMessage() noexcept
{
    static const UINT message =
        RegisterWindowMessageW(nekorox::kCoverStateMessageV1);
    return message;
}

UINT transportStateMessage() noexcept
{
    static const UINT message =
        RegisterWindowMessageW(nekorox::kTransportStateMessageV1);
    return message;
}

UINT frameClockStateMessage() noexcept
{
    static const UINT message =
        RegisterWindowMessageW(nekorox::kFrameClockStateMessageV1);
    return message;
}

UINT headerQaOpenMessage() noexcept
{
    static const UINT message = RegisterWindowMessageW(
        L"GodenHeaderQaOpenV1");
    return message;
}

UINT headerPaintQaMessage() noexcept
{
    static const auto message = RegisterWindowMessageW(L"Goden.HeaderPaintQa.V1");
    return message;
}

constexpr MotionKey kHeaderHoverMotionBase = 0x100U;
constexpr MotionKey kHeaderPressMotionBase = 0x120U;
constexpr MotionKey kTransportHoverMotionBase = 0x200U;
constexpr MotionKey kTransportPressMotionBase = 0x220U;

UiColor uiColorFromD2D(D2D1_COLOR_F color) noexcept
{
    return {color.r, color.g, color.b, color.a};
}

const GUID kHeaderPanelGuid = {
    0xc5c3917c, 0x8030, 0x46b5, {0xaf, 0xc8, 0x9e, 0xc5, 0xdc, 0xa1, 0x7c, 0x19}};
const GUID kTransportPanelGuid = {
    0x37dfaaa1, 0x5040, 0x4966, {0x81, 0x9f, 0xd0, 0x12, 0xf4, 0x1a, 0x60, 0x78}};
const GUID kCoverPanelGuid = {
    0x5d0cc69e, 0xd3d0, 0x4c82, {0x90, 0xdb, 0x40, 0xc7, 0xcf, 0x9c, 0x3c, 0x9e}};
const GUID kMonitorPanelGuid = {
    0x8e75bd84, 0xc4d0, 0x4f59, {0x8d, 0xa8, 0x9b, 0x12, 0x33, 0xf3, 0xef, 0x71}};
const GUID kCaptionPanelGuid = {
    0x6d9a108b, 0xcc44, 0x44ba, {0x88, 0x54, 0x5f, 0x7b, 0xd0, 0xcd, 0x33, 0x10}};

void registerNativeWindow(HWND window)
{
    UiInvalidationBus::instance().registerWindow(
        UiInvalidationSurface::nativePanels,
        window,
        kNativeRefreshMessage);
    ThemeService::instance().subscribe(window, kThemeTransitionMessage);
}

void unregisterNativeWindow(HWND window)
{
    ThemeService::instance().unsubscribe(window);
    UiInvalidationBus::instance().unregisterWindow(
        UiInvalidationSurface::nativePanels,
        window);
}

void applyFixedBandLimits(
    MINMAXINFO* limits,
    HWND window,
    UiFixedBand band) noexcept
{
    if (limits == nullptr) return;
    const auto height = static_cast<LONG>(fixedBandHeightPixels(
        band,
        window != nullptr ? GetDpiForWindow(window) : 96U));
    limits->ptMinTrackSize.y = height;
    limits->ptMaxTrackSize.y = height;
}

void notifyFixedBandDpiChanged(
    const uie::window_host_ptr& host,
    HWND window) noexcept
{
    if (host.is_valid() && window != nullptr) {
        host->on_size_limit_change(
            window,
            uie::size_limit_minimum_height |
                uie::size_limit_maximum_height);
    }
}

UiDirty consumeNativeInvalidation(HWND window) noexcept
{
    const auto dirty = UiInvalidationBus::instance().consume(
        UiInvalidationSurface::nativePanels,
        window);
    return any(dirty) ? dirty : UiDirty::semantic;
}

bool canPresent(HWND window) noexcept
{
    return DisplayEnvironmentService::instance().presentationVisible(window);
}

void presentBoundedThemeFrame(HWND window) noexcept
{
    if (window == nullptr) return;
    InvalidateRect(window, nullptr, FALSE);
    // Frame notices already run within WM_PAINT. A synchronous update here
    // nests another Present before the original handler begins painting.
}

double presentationElapsedSeconds(
    LARGE_INTEGER& previous,
    double fallbackSeconds) noexcept
{
    LARGE_INTEGER now{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    if (previous.QuadPart == 0 || frequency.QuadPart <= 0) {
        previous = now;
        return fallbackSeconds;
    }
    const auto elapsed = static_cast<double>(
        now.QuadPart - previous.QuadPart) /
        static_cast<double>(frequency.QuadPart);
    previous = now;
    return std::clamp(elapsed, 0.0005, 0.050);
}

double presentationQpcSeconds() noexcept
{
    LARGE_INTEGER now{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart > 0
        ? static_cast<double>(now.QuadPart) /
            static_cast<double>(frequency.QuadPart)
        : 0.0;
}

std::wstring playbackTime(double seconds)
{
    const auto total = static_cast<long long>(std::max(0.0, std::floor(seconds)));
    wchar_t buffer[32]{};
    if (total >= 3600) {
        std::swprintf(buffer, std::size(buffer), L"%lld:%02lld:%02lld",
            total / 3600, (total / 60) % 60, total % 60);
    } else {
        std::swprintf(buffer, std::size(buffer), L"%lld:%02lld", total / 60, total % 60);
    }
    return buffer;
}

std::wstring currentTrackCaption()
{
    const auto snapshot = Runtime::instance().playbackSnapshot();
    if (snapshot.state == PlaybackState::stopped ||
        snapshot.trackIdentityHash == 0) return {};
    const auto artist = snapshot.artist[0] != '\0'
        ? utf8ToWide(snapshot.artist.data()) : L"Unknown artist";
    auto title = snapshot.title[0] != '\0'
        ? utf8ToWide(snapshot.title.data()) : std::wstring{};
    if (title.empty() && snapshot.path[0] != '\0') {
        title = utf8ToWide(snapshot.path.data());
        const auto slash = title.find_last_of(L"/\\");
        if (slash != std::wstring::npos) title.erase(0, slash + 1U);
    }
    if (title.empty()) title = L"Unknown title";
    const auto album = snapshot.album[0] != '\0'
        ? utf8ToWide(snapshot.album.data()) : L"Unknown album";
    return trimText(artist + L" - " + title + L" - " + album);
}

bool pointIn(const D2D1_RECT_F& rectangle, float x, float y) noexcept
{
    return x >= rectangle.left && x < rectangle.right &&
        y >= rectangle.top && y < rectangle.bottom;
}

D2D1_SIZE_F clientSizeDip(HWND window) noexcept
{
    RECT rectangle{};
    GetClientRect(window, &rectangle);
    const auto scale = 96.0f /
        static_cast<float>(std::max<UINT>(1U, GetDpiForWindow(window)));
    return D2D1::SizeF(
        static_cast<float>(rectangle.right - rectangle.left) * scale,
        static_cast<float>(rectangle.bottom - rectangle.top) * scale);
}

std::pair<float, float> clientPointDip(HWND window, LPARAM lParam) noexcept
{
    const auto scale = 96.0f /
        static_cast<float>(std::max<UINT>(1U, GetDpiForWindow(window)));
    return {
        static_cast<float>(GET_X_LPARAM(lParam)) * scale,
        static_cast<float>(GET_Y_LPARAM(lParam)) * scale,
    };
}

D2D1_RECT_F shiftedControlRect(
    D2D1_RECT_F rectangle,
    double hover,
    double press) noexcept
{
    if (motionMode() != MotionMode::normal) return rectangle;
    const auto offset = std::clamp(
        -hover * (1.0 - press) + press * 0.5,
        -1.0,
        1.0);
    rectangle.top += static_cast<float>(offset);
    rectangle.bottom += static_cast<float>(offset);
    return rectangle;
}

float interactionOpacity(double hover, double press) noexcept
{
    return static_cast<float>(std::clamp(
        hover * 0.30 + press * 0.38,
        0.0,
        0.68));
}

PanelColor interactionColor(
    PanelColor base,
    double hover,
    double press) noexcept
{
    if (press > 0.35) return PanelColor::bright;
    if (hover > 0.35 && base == PanelColor::muted) return PanelColor::text;
    return base;
}

class NativeTooltipHost final {
public:
    void create(HWND owner)
    {
        destroy();
        owner_ = owner;
        tooltip_ = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            TOOLTIPS_CLASSW,
            nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            owner_,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        if (tooltip_ == nullptr) return;
        SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 320);
        SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_INITIAL, 450);
        SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_RESHOW, 90);
    }

    void destroy() noexcept
    {
        if (tooltip_ != nullptr) DestroyWindow(tooltip_);
        tooltip_ = nullptr;
        owner_ = nullptr;
    }

    void add(UINT_PTR id, const wchar_t* text) const
    {
        if (tooltip_ == nullptr || owner_ == nullptr || text == nullptr) return;
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.uFlags = TTF_SUBCLASS | TTF_TRANSPARENT;
        tool.hwnd = owner_;
        tool.uId = id;
        tool.rect = {0, 0, 1, 1};
        tool.lpszText = const_cast<wchar_t*>(text);
        SendMessageW(
            tooltip_,
            TTM_ADDTOOLW,
            0,
            reinterpret_cast<LPARAM>(&tool));
    }

    void update(UINT_PTR id, const D2D1_RECT_F& rectangle) const
    {
        if (tooltip_ == nullptr || owner_ == nullptr) return;
        const auto scale = static_cast<float>(
            std::max<UINT>(1U, GetDpiForWindow(owner_))) / 96.0f;
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.hwnd = owner_;
        tool.uId = id;
        tool.rect = {
            static_cast<LONG>(std::floor(rectangle.left * scale)),
            static_cast<LONG>(std::floor(rectangle.top * scale)),
            static_cast<LONG>(std::ceil(rectangle.right * scale)),
            static_cast<LONG>(std::ceil(rectangle.bottom * scale)),
        };
        SendMessageW(
            tooltip_,
            TTM_NEWTOOLRECTW,
            0,
            reinterpret_cast<LPARAM>(&tool));
    }

private:
    HWND owner_ = nullptr;
    HWND tooltip_ = nullptr;
};

std::wstring lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

struct PlaybackOrderState {
    t_size active = 0;
    bool repeat = false;
    bool shuffle = false;
    bool repeatOne = false;
};

PlaybackOrderState capturePlaybackOrder()
{
    auto playlists = playlist_manager::get();
    PlaybackOrderState result;
    result.active = playlists->playback_order_get_active();
    if (result.active >= playlists->playback_order_get_count()) return result;
    const auto name = lower(utf8ToWide(
        playlists->playback_order_get_name(result.active)));
    result.repeat = name.find(L"repeat") != std::wstring::npos;
    result.shuffle = name.find(L"shuffle") != std::wstring::npos ||
        name.find(L"random") != std::wstring::npos;
    result.repeatOne = result.repeat && (name.find(L"track") != std::wstring::npos ||
        name.find(L"one") != std::wstring::npos);
    return result;
}

std::uint32_t queryTransportState()
{
    const auto control = playback_control::get();
    std::uint32_t state = 0;
    if (Runtime::instance().playbackSnapshot().state !=
            PlaybackState::stopped) {
        state |= nekorox::kTransportPlayingBitV1;
    }
    if (control->is_muted()) state |= nekorox::kTransportMutedBitV1;
    const auto playbackOrder = capturePlaybackOrder();
    if (playbackOrder.repeat) {
        state |= nekorox::kTransportRepeatBitV1;
    }
    if (playbackOrder.shuffle) {
        state |= nekorox::kTransportShuffleBitV1;
    }
    if (PacketCostController::instance().readyResult()) {
        state |= nekorox::kTransportCostMapReadyBitV1;
    }
    const auto normalized = std::clamp(
        std::pow(10.0, control->get_volume() / 20.0), 0.0, 1.0);
    const auto volume = static_cast<std::uint32_t>(std::lround(
        normalized * nekorox::kTransportVolumeMaskV1));
    const auto order = static_cast<std::uint32_t>(std::min<t_size>(
        playbackOrder.active,
        nekorox::kTransportOrderMaskV1));
    state |= volume << nekorox::kTransportVolumeShiftV1;
    state |= order << nekorox::kTransportOrderShiftV1;
    return state;
}

void togglePlaybackOrder(const wchar_t* token, const wchar_t* preferred)
{
    auto playlists = playlist_manager::get();
    const auto count = playlists->playback_order_get_count();
    const auto active = playlists->playback_order_get_active();
    const bool disabling = active < count &&
        lower(utf8ToWide(playlists->playback_order_get_name(active))).find(token) != std::wstring::npos;

    t_size fallback = pfc::infinite_size;
    t_size target = pfc::infinite_size;
    for (t_size index = 0; index < count; ++index) {
        const auto name = lower(utf8ToWide(playlists->playback_order_get_name(index)));
        if (name == L"default") fallback = index;
        if (!disabling && name.find(preferred) != std::wstring::npos) target = index;
        if (!disabling && target == pfc::infinite_size && name.find(token) != std::wstring::npos) target = index;
    }
    const auto desired = disabling ? fallback : target;
    if (desired != pfc::infinite_size) playlists->playback_order_set_active(desired);
    notifyNativePanelsPlaybackChanged();
}

struct HeaderLayout {
    float menuHeight = 38.0f;
    D2D1_RECT_F menu{};
    D2D1_RECT_F tag{};
    D2D1_RECT_F dsp{};
    D2D1_RECT_F daw{};
    D2D1_RECT_F options{};
    D2D1_RECT_F monitor{};
    D2D1_RECT_F listen{};
    D2D1_RECT_F album{};
    D2D1_RECT_F queue{};
    D2D1_RECT_F nowPlaying{};
    D2D1_RECT_F minimize{};
    D2D1_RECT_F maximize{};
    D2D1_RECT_F close{};
    D2D1_RECT_F stop{};
    D2D1_RECT_F previous{};
    D2D1_RECT_F play{};
    D2D1_RECT_F next{};
};

enum class HeaderControl : int {
    none = -1,
    menu = 0,
    tag,
    dsp,
    daw,
    options,
    listen,
    album,
    queue,
    nowPlaying,
    minimize,
    maximize,
    close,
    stop,
    previous,
    play,
    next,
    count,
};

constexpr auto kHeaderControlCount =
    static_cast<int>(HeaderControl::count);

float headerMenuHeight(float totalHeight) noexcept
{
    // Profiles moved between DPI environments can give the header only about
    // 52 DIP. A fixed 38-DIP caption then crushes the playback row into a few
    // pixels. Preserve a readable lower control row and compact the caption
    // first; its labels and window buttons remain usable at 28 DIP.
    return std::clamp(totalHeight - 24.0f, 28.0f, 38.0f);
}

HeaderLayout headerLayout(float width, float height)
{
    const auto menuHeight = headerMenuHeight(height);
    constexpr float monitorAspect = 4.7f;
    const auto monitorHeight = std::max(20.0f, menuHeight - 6.0f);
    constexpr float button = 25.0f;
    constexpr float gap = 2.0f;
    float right = width - 6.0f;
    HeaderLayout result;
    result.menuHeight = menuHeight;
    result.menu = D2D1::RectF(7.0f, 1.0f, 55.0f, menuHeight - 1.0f);
    result.close = D2D1::RectF(width - 30.0f, 1.0f, width - 2.0f, menuHeight - 1.0f);
    result.maximize = D2D1::RectF(width - 58.0f, 1.0f, width - 30.0f, menuHeight - 1.0f);
    result.minimize = D2D1::RectF(width - 86.0f, 1.0f, width - 58.0f, menuHeight - 1.0f);
    result.nowPlaying = D2D1::RectF(width - 126.0f, 1.0f, width - 90.0f, menuHeight - 1.0f);
    result.queue = D2D1::RectF(width - 158.0f, 1.0f, width - 130.0f, menuHeight - 1.0f);
    result.album = {};
    result.listen = D2D1::RectF(width - 214.0f, 1.0f,
        width - 162.0f, menuHeight - 1.0f);
    const auto monitorRight = result.listen.left - 8.0f;
    const auto monitorMinimumLeft = result.menu.right + 110.0f;
    const auto monitorAvailable = monitorRight - monitorMinimumLeft;
    const auto monitorWidth = monitorHeight * monitorAspect;
    constexpr float dspWidth = 40.0f;
    constexpr float dawWidth = 42.0f;
    constexpr float optionsWidth = 62.0f;
    constexpr float utilityGap = 5.0f;
    constexpr float tagWidth = 34.0f;
    constexpr float mandatoryWidth = tagWidth + dspWidth +
        dawWidth + utilityGap * 3.0f;
    constexpr float utilityWidth = mandatoryWidth + optionsWidth + utilityGap;
    if (monitorAvailable >= monitorWidth + utilityWidth) {
        result.monitor = D2D1::RectF(
            monitorRight - monitorWidth,
            (menuHeight - monitorHeight) * 0.5f,
            monitorRight,
            (menuHeight + monitorHeight) * 0.5f);
        result.options = D2D1::RectF(
            result.monitor.left - utilityGap - optionsWidth,
            1.0f,
            result.monitor.left - utilityGap,
            menuHeight - 1.0f);
        result.daw = D2D1::RectF(
            result.options.left - utilityGap - dawWidth,
            1.0f,
            result.options.left - utilityGap,
            menuHeight - 1.0f);
        result.dsp = D2D1::RectF(
            result.daw.left - utilityGap - dspWidth,
            1.0f,
            result.daw.left - utilityGap,
            menuHeight - 1.0f);
        result.tag = D2D1::RectF(
            result.dsp.left - utilityGap - tagWidth,
            1.0f,
            result.dsp.left - utilityGap,
            menuHeight - 1.0f);
    } else if (monitorAvailable >= utilityWidth) {
        result.options = D2D1::RectF(
            monitorRight - optionsWidth,
            1.0f,
            monitorRight,
            menuHeight - 1.0f);
        result.daw = D2D1::RectF(
            result.options.left - utilityGap - dawWidth,
            1.0f,
            result.options.left - utilityGap,
            menuHeight - 1.0f);
        result.dsp = D2D1::RectF(
            result.daw.left - utilityGap - dspWidth,
            1.0f,
            result.daw.left - utilityGap,
            menuHeight - 1.0f);
        result.tag = D2D1::RectF(
            result.dsp.left - utilityGap - tagWidth,
            1.0f,
            result.dsp.left - utilityGap,
            menuHeight - 1.0f);
    } else if (monitorAvailable >= mandatoryWidth) {
        result.daw = D2D1::RectF(
            monitorRight - dawWidth,
            1.0f,
            monitorRight,
            menuHeight - 1.0f);
        result.dsp = D2D1::RectF(
            result.daw.left - utilityGap - dspWidth,
            1.0f,
            result.daw.left - utilityGap,
            menuHeight - 1.0f);
        result.tag = D2D1::RectF(
            result.dsp.left - utilityGap - tagWidth,
            1.0f,
            result.dsp.left - utilityGap,
            menuHeight - 1.0f);
    }
    auto take = [&](float itemWidth) {
        const auto rectangle = D2D1::RectF(right - itemWidth, menuHeight + 2.0f, right, height - 3.0f);
        right -= itemWidth + gap;
        return rectangle;
    };
    result.next = take(button);
    result.play = take(button);
    result.previous = take(button);
    result.stop = take(button);
    return result;
}

D2D1_RECT_F headerControlRect(
    const HeaderLayout& layout,
    HeaderControl control) noexcept
{
    switch (control) {
    case HeaderControl::menu: return layout.menu;
    case HeaderControl::tag: return layout.tag;
    case HeaderControl::dsp: return layout.dsp;
    case HeaderControl::daw: return layout.daw;
    case HeaderControl::options: return layout.options;
    case HeaderControl::listen: return layout.listen;
    case HeaderControl::album: return layout.album;
    case HeaderControl::queue: return layout.queue;
    case HeaderControl::nowPlaying: return layout.nowPlaying;
    case HeaderControl::minimize: return layout.minimize;
    case HeaderControl::maximize: return layout.maximize;
    case HeaderControl::close: return layout.close;
    case HeaderControl::stop: return layout.stop;
    case HeaderControl::previous: return layout.previous;
    case HeaderControl::play: return layout.play;
    case HeaderControl::next: return layout.next;
    case HeaderControl::none:
    case HeaderControl::count:
        break;
    }
    return {};
}

HeaderControl hitHeaderControl(
    const HeaderLayout& layout,
    float x,
    float y) noexcept
{
    constexpr std::array order{
        HeaderControl::close,
        HeaderControl::maximize,
        HeaderControl::minimize,
        HeaderControl::nowPlaying,
        HeaderControl::queue,
        HeaderControl::listen,
        HeaderControl::options,
        HeaderControl::daw,
        HeaderControl::dsp,
        HeaderControl::tag,
        HeaderControl::menu,
        HeaderControl::next,
        HeaderControl::play,
        HeaderControl::previous,
        HeaderControl::stop,
    };
    for (const auto control : order) {
        if (pointIn(headerControlRect(layout, control), x, y)) return control;
    }
    return HeaderControl::none;
}

const wchar_t* headerTooltipText(HeaderControl control) noexcept
{
    switch (control) {
    case HeaderControl::menu: return L"Main menu";
    case HeaderControl::tag: return L"Open safe batch TAG editor";
    case HeaderControl::dsp: return L"Open GODENplayer Master DSP";
    case HeaderControl::daw: return L"Open GODENplayer DAW Mode";
    case HeaderControl::options: return L"Open GODENplayer Options";
    case HeaderControl::listen: return L"Open GODENplayer Listen";
    case HeaderControl::album: return L"Open or close album mode (Ctrl+Shift+I)";
    case HeaderControl::queue: return L"Open playback queue";
    case HeaderControl::nowPlaying: return L"Show now playing";
    case HeaderControl::minimize: return L"Minimize";
    case HeaderControl::maximize: return L"Maximize or restore";
    case HeaderControl::close: return L"Close";
    case HeaderControl::stop: return L"Stop";
    case HeaderControl::previous: return L"Previous track";
    case HeaderControl::play: return L"Play or pause";
    case HeaderControl::next: return L"Next track";
    case HeaderControl::none:
    case HeaderControl::count:
        break;
    }
    return nullptr;
}

void drawCompactScope(
    PanelD2D& render,
    const D2D1_RECT_F& bounds,
    const CompactScopePresenter& scope,
    CompactScopeGpu& gpu)
{
    const auto width = bounds.right - bounds.left;
    const auto height = bounds.bottom - bounds.top;
    if (width < 20.0f || height < 6.0f || !scope.ready()) return;

    render.fill(bounds, PanelColor::panel);
    render.line(bounds.left, bounds.top, bounds.right, bounds.top,
        PanelColor::border);
    render.line(bounds.left, bounds.bottom, bounds.right, bounds.bottom,
        PanelColor::border);
    render.line(bounds.left, bounds.top, bounds.left, bounds.bottom,
        PanelColor::border);
    render.line(bounds.right, bounds.top, bounds.right, bounds.bottom,
        PanelColor::border);

    const auto left = bounds.left + 2.0f;
    const auto right = bounds.right - 2.0f;
    const auto center = (bounds.top + bounds.bottom) * 0.5f;
    const auto amplitude = std::max(1.0f, (height - 4.0f) * 0.47f);
    auto* target = render.target();
    auto* baseline = render.brush(PanelColor::selected);
    auto* accent = render.brush(PanelColor::secondary);
    auto* bright = render.brush(PanelColor::highlight);
    if (target == nullptr || baseline == nullptr || accent == nullptr ||
        bright == nullptr) {
        return;
    }

    const auto baselineOpacity = baseline->GetOpacity();
    baseline->SetOpacity(0.34f);
    target->DrawLine(D2D1::Point2F(left, center),
        D2D1::Point2F(right, center), baseline, 1.0f);
    baseline->SetOpacity(baselineOpacity);
    const auto& samples = scope.samples();
    const auto& minimum = scope.minimumSamples();
    const auto& maximum = scope.maximumSamples();
    const auto columns = std::clamp<std::size_t>(
        static_cast<std::size_t>(std::lround(right - left)),
        1U,
        kScopeSamples);
    static_assert(kScopeSamples == 512);
    std::array<CompactScopeColumn, kScopeSamples> projected{};
    for (std::size_t column = 0; column < columns; ++column) {
        const auto first = column * kScopeSamples / columns;
        const auto last = std::max(
            first + 1U,
            (column + 1U) * kScopeSamples / columns);
        auto low = minimum[first];
        auto high = maximum[first];
        float midpoint = 0.0f;
        for (auto index = first; index < last; ++index) {
            low = std::min(low, minimum[index]);
            high = std::max(high, maximum[index]);
            midpoint += samples[index];
        }
        midpoint /= static_cast<float>(last - first);

        const auto x = left + (static_cast<float>(column) + 0.5f) *
            (right - left) / static_cast<float>(columns);
        const auto top = center - high * amplitude;
        const auto bottom = center - low * amplitude;
        projected[column] = {x, top, bottom, center - midpoint * amplitude};
    }
    if (gpu.tryDraw(target, render.d3dDevice(), render.backend(),
            {projected.data(), columns}, accent->GetColor(), bright->GetColor())) return;

    // Unsupported states or device/effect failures retain the original drawing
    // and source-over order. Projection is shared with the GPU path.
    const auto accentOpacity = accent->GetOpacity();
    const auto brightOpacity = bright->GetOpacity();
    accent->SetOpacity(0.16f);
    bright->SetOpacity(0.92f);
    for (std::size_t column = 0; column < columns; ++column) {
        const auto& point = projected[column];
        target->DrawLine(
            D2D1::Point2F(point.x, point.top),
            D2D1::Point2F(point.x, point.bottom),
            accent,
            2.0f);
        target->DrawLine(
            D2D1::Point2F(point.x, point.top),
            D2D1::Point2F(point.x, point.bottom),
            bright,
            1.0f);
    }
    for (std::size_t index = 1; index < columns; ++index) {
        target->DrawLine(
            D2D1::Point2F(projected[index - 1U].x, projected[index - 1U].midpoint),
            D2D1::Point2F(projected[index].x, projected[index].midpoint),
            bright,
            1.0f);
    }
    accent->SetOpacity(accentOpacity);
    bright->SetOpacity(brightOpacity);
}

class HeaderPanel : public uie::container_uie_window_v3 {
public:
    const GUID& get_extension_guid() const override { return kHeaderPanelGuid; }
    void get_name(pfc::string_base& output) const override { output = "GODENplayer Header"; }
    void get_category(pfc::string_base& output) const override { output = "Panels"; }
    unsigned get_type() const override { return uie::type_panel; }

private:
    uie::container_window_v3_config get_window_config() override
    {
        // The existing WM_LBUTTONDBLCLK handler only receives messages when
        // the registered class opts into double-click synthesis.
        return {L"NekoRoXHeaderV3", false, CS_DBLCLKS};
    }

    LRESULT on_message(HWND window, UINT message, WPARAM wParam, LPARAM lParam) override
    {
        if (message == headerPaintQaMessage()) {
            wchar_t enabled[8]{};
            if (!GetEnvironmentVariableW(L"NEKOROX_QA_HOOKS", enabled, 8) || enabled[0] != L'1')
                return E_ACCESSDENIED;
            if (wParam == 0) return static_cast<LRESULT>(fullPaints_);
            if (wParam == 1) return static_cast<LRESULT>(partialPaints_);
            if (wParam == 2) return static_cast<LRESULT>(monitorTicks_);
            if (wParam == 3) return static_cast<LRESULT>(cachedOverlayPaints_);
            const auto& scopeStats = scopeGpu_.statistics();
            if (wParam == 4) return static_cast<LRESULT>(scopeStats.submitted);
            if (wParam == 5) return static_cast<LRESULT>(scopeStats.fallback);
            if (wParam == 6) return static_cast<LRESULT>(scopeStats.creations);
            if (wParam == 7) return static_cast<LRESULT>(scopeStats.failures);
            if (wParam == 8) return static_cast<LRESULT>(scopeStats.completedDraws);
            if (wParam == 9) return static_cast<LRESULT>(scopeGpu_.lastFailure());
            if (wParam == 20 || wParam == 21 || wParam == 22) {
                if (!qa::hooksAllowed()) return E_ACCESSDENIED;
                if (wParam == 20) scopeGpu_.setScalarForQa(lParam != 0);
                if (wParam == 21) {
                    if (lParam < 1 || lParam > 3) return E_INVALIDARG;
                    scopeGpu_.queueFaultForQa(static_cast<CompactScopeGpu::FaultForQa>(lParam));
                }
                if (wParam == 22) {
                    scopeGpu_.releaseTargetResources();
                    render_.reset();
                }
                retainedHeaderReady_ = false;
                InvalidateRect(window_, nullptr, FALSE);
                return TRUE;
            }
            return 0;
        }
        if (message == frameClockStateMessage()) {
            return static_cast<LRESULT>(
                WindowFrameClock::diagnosticActiveCount());
        }
        if (message == headerQaOpenMessage()) {
            if (!qa::hooksAllowed()) return E_ACCESSDENIED;
            if (wParam == 1U) DawWindow::instance().show(window_);
            else if (wParam == 2U) OptionsWindow::instance().show(window_);
            else if (wParam == 3U) ListenWindow::instance().show();
            else if (wParam == 4U) TagEditorWindow::instance().show(window_);
            else return FALSE;
            return TRUE;
        }
        switch (message) {
        case WM_CREATE:
            window_ = window;
            analysisLifetime_ = gNextHeaderAnalysisLifetime.fetch_add(
                1, std::memory_order_relaxed);
            monitorClock_.attach(window_, kMonitorFrameMessage);
            monitorClock_.setTelemetrySurface(TelemetrySurface::headerMonitor);
            monitorClock_.setDamageProvider(
                [](HWND, void* context, RECT& region) noexcept {
                    auto& owner = *static_cast<HeaderPanel*>(context);
                    if (!owner.retainedHeaderReady_ || owner.motion_.needsFrame() ||
                        owner.motionWasActive_ || owner.themeWasActive_ || owner.glyphWasActive_ ||
                        ThemeService::instance().transitionActive() ||
                        GlyphFlowService::instance().active(motionMode())) return false;
                    region = owner.monitorPixelRect();
                    return true;
                }, this);
            GlyphFlowService::instance().subscribe(
                window_, kGlyphFlowChangedMessage);
            for (int index = 0; index < kHeaderControlCount; ++index) {
                motion_.begin(
                    kHeaderHoverMotionBase +
                        static_cast<MotionKey>(index),
                    MotionToken::hover,
                    MotionMode::off,
                    0.0,
                    0.0);
                motion_.begin(
                    kHeaderPressMotionBase +
                        static_cast<MotionKey>(index),
                    MotionToken::press,
                    MotionMode::off,
                    0.0,
                    0.0);
            }
            playbackGlyph_ = currentPlaybackGlyph();
            createTooltips();
            registerNativeWindow(window_);
            syncMonitorPlayback();
            return 0;
        case WM_DESTROY:
            cancelPress();
            tooltips_.destroy();
            stopMonitorRealtime();
            motion_.clear();
            GlyphFlowService::instance().unsubscribe(window_);
            monitorClock_.detach();
            unregisterNativeWindow(window_);
            window_ = nullptr;
            glyphFlowRenderer_.reset();
            scopeGpu_.reset();
            render_.reset();
            return 0;
        case WM_ERASEBKGND:
            return TRUE;
        case WM_SHOWWINDOW:
            syncMonitorPlayback();
            syncPresentationClock();
            break;
        case WM_SIZE:
            retainedHeaderReady_ = false;
            render_.resize();
            syncTooltips();
            syncMonitorPlayback();
            syncPresentationClock();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO:
            applyFixedBandLimits(
                reinterpret_cast<MINMAXINFO*>(lParam),
                window,
                UiFixedBand::header);
            return 0;
        case WM_DPICHANGED:
            notifyFixedBandDpiChanged(get_host(), window);
            retainedHeaderReady_ = false;
            glyphFlowRenderer_.reset();
            scopeGpu_.releaseTargetResources();
            render_.reset();
            syncTooltips();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_PAINT:
            paint(window);
            return 0;
        case kMonitorFrameMessage:
            if (monitorClock_.consume(wParam)) {
                ++monitorTicks_;
                tickMonitor();
            }
            return 0;
        case kGlyphFlowChangedMessage:
            syncPresentationClock();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case kThemeTransitionMessage:
            syncPresentationClock();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            pointerDown(lParam, true);
            return 0;
        case WM_LBUTTONDBLCLK:
            // The first click in a double-click sequence has already executed
            // the control action on button-up. Re-arming here made the second
            // button-up execute it again (notably Next and Pause).
            if (hitTest(lParam) == HeaderControl::none) {
                toggleMaximize(lParam);
            }
            return 0;
        case WM_LBUTTONUP:
            pointerUp(lParam);
            return 0;
        case WM_RBUTTONUP:
            showContextMenu(lParam);
            return 0;
        case WM_MOUSEMOVE:
            updateHover(lParam);
            return 0;
        case WM_MOUSELEAVE:
            trackingMouse_ = false;
            setHoverControl(HeaderControl::none);
            setPressInside(false);
            return 0;
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
        case WM_KILLFOCUS:
            trackingMouse_ = false;
            setHoverControl(HeaderControl::none);
            cancelPress();
            return 0;
        case kNativeRefreshMessage:
            (void)consumeNativeInvalidation(window);
            syncPlaybackGlyphMotion();
            syncMonitorPlayback();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void paint(HWND window)
    {
        PAINTSTRUCT paintStruct{};
        BeginPaint(window, &paintStruct);
        const auto themeSampleActive = ThemeService::instance().transitionActive();
        const auto glyphSampleActive = GlyphFlowService::instance().active(motionMode());
        const auto monitorPixels = monitorPixelRect();
        bool monitorOnly = retainedHeaderReady_ &&
            paintStruct.rcPaint.left >= monitorPixels.left &&
            paintStruct.rcPaint.top >= monitorPixels.top &&
            paintStruct.rcPaint.right <= monitorPixels.right &&
            paintStruct.rcPaint.bottom <= monitorPixels.bottom &&
            render_.retainedContentsReady(window);
        const auto cachedOverlay = monitorOnly &&
            render_.beginCachedOverlay(window, paintStruct.hdc, monitorPixels);
        auto began = cachedOverlay;
        if (monitorOnly && !began &&
                render_.backend() == SurfaceBackend::gdiFallback) {
            // Preserve the bounded partial repaint during software recovery.
            began = render_.beginPartial(window, paintStruct.hdc, monitorPixels);
        }
        if (!began) {
            // A resize, topology migration or device recovery may invalidate
            // the backing between the header's semantic state and WM_PAINT.
            // Promote that monitor tick to a complete frame immediately.
            monitorOnly = false;
            retainedHeaderReady_ = false;
            began = render_.begin(window, paintStruct.hdc);
        }
        if (!began) {
            retainedHeaderReady_ = false;
            EndPaint(window, &paintStruct);
            return;
        }
        const auto size = render_.sizeDip();
        const auto layout = headerLayout(size.width, size.height);
        if (monitorOnly) {
            ++partialPaints_;
            if (cachedOverlay) ++cachedOverlayPaints_;
            // The padded damage region reaches the caption separator. Restore
            // its clipped portion as in a complete semantic frame.
            render_.line(0.0f, layout.menuHeight, size.width,
                layout.menuHeight, PanelColor::border);
            drawCompactScope(render_, layout.monitor, monitorPresentation_, scopeGpu_);
            const auto result = render_.end();
            retainedHeaderReady_ = SUCCEEDED(result);
            if (scopeGpu_.finishFrame(render_.lastFrameOutcome().drawHr)) {
                retainedHeaderReady_ = false;
                InvalidateRect(window_, nullptr, FALSE);
            }
            EndPaint(window, &paintStruct);
            return;
        }
        const auto menuHeight = layout.menuHeight;
        ++fullPaints_;
        render_.line(0.0f, menuHeight, size.width, menuHeight, PanelColor::border);
        render_.line(0.0f, size.height - 1.0f, size.width, size.height - 1.0f, PanelColor::border);

        for (int index = 0; index < kHeaderControlCount; ++index) {
            drawControlSurface(
                static_cast<HeaderControl>(index),
                headerControlRect(layout, static_cast<HeaderControl>(index)));
        }

        render_.text(
            L"Menu",
            visualRect(layout, HeaderControl::menu),
            PanelText::normal,
            controlColor(HeaderControl::menu, PanelColor::text));
        if (rolloutFlagEnabled(RolloutFlag::tagEditorV1)) {
            render_.text(L"TAG", visualRect(layout, HeaderControl::tag),
                PanelText::center,
                controlColor(HeaderControl::tag, PanelColor::secondary));
        }
        const auto dspState = MasterDspController::instance().snapshot();
        auto dspColor = PanelColor::muted;
        switch (dspState.status) {
        case goden_audio::MasterDspStatus::dry:
        case goden_audio::MasterDspStatus::bypassed:
            dspColor = PanelColor::secondary;
            break;
        case goden_audio::MasterDspStatus::active:
            dspColor = PanelColor::accent;
            break;
        case goden_audio::MasterDspStatus::preparing:
            dspColor = PanelColor::amber;
            break;
        case goden_audio::MasterDspStatus::partialFault:
            dspColor = PanelColor::highlight;
            break;
        case goden_audio::MasterDspStatus::off:
            break;
        }
        render_.text(
            L"DSP",
            visualRect(layout, HeaderControl::dsp),
            PanelText::center,
            controlColor(HeaderControl::dsp, dspColor));
        if (dspState.enabled) {
            const auto dspRect = visualRect(layout, HeaderControl::dsp);
            const auto count = MasterDspController::instance().activeModuleCountRt();
            const auto active = count > 0 && !dspState.bypassed;
            render_.line(
                dspRect.left + (active ? 5.0f : 15.0f),
                dspRect.bottom - 3.0f,
                dspRect.right - (active ? 5.0f : 15.0f),
                dspRect.bottom - 3.0f,
                active ? PanelColor::accent : PanelColor::secondary,
                active ? 2.0f : 1.0f);
            if (active) {
                render_.text(
                    std::to_wstring(std::min<std::size_t>(count, 32U)),
                    D2D1::RectF(dspRect.right - 12.0f, dspRect.top,
                        dspRect.right, dspRect.top + 12.0f),
                    PanelText::caption,
                    PanelColor::accent);
            }
        }
        render_.text(
            L"DAW",
            visualRect(layout, HeaderControl::daw),
            PanelText::center,
            controlColor(HeaderControl::daw, PanelColor::muted));
        render_.text(
            L"OPTIONS",
            visualRect(layout, HeaderControl::options),
            PanelText::center,
            controlColor(HeaderControl::options, PanelColor::secondary));
        drawCompactScope(render_, layout.monitor, monitorPresentation_, scopeGpu_);
        render_.text(
            L"LISTEN",
            visualRect(layout, HeaderControl::listen),
            PanelText::center,
            controlColor(HeaderControl::listen, PanelColor::secondary));
        const auto queueCount = playlist_manager::get()->queue_get_count();
        const auto queueBase =
            queueCount > 0 ? PanelColor::accent : PanelColor::muted;
        render_.glyph(
            PanelGlyph::queue,
            visualRect(layout, HeaderControl::queue),
            controlColor(HeaderControl::queue, queueBase));
        if (queueCount > 0) {
            const auto queueRect = visualRect(layout, HeaderControl::queue);
            render_.text(
                std::to_wstring(std::min<t_size>(queueCount, 99U)),
                D2D1::RectF(queueRect.right - 11.0f, queueRect.top,
                    queueRect.right, queueRect.top + 11.0f),
                PanelText::caption,
                PanelColor::accent);
        }
        render_.glyph(
            PanelGlyph::nowPlaying,
            visualRect(layout, HeaderControl::nowPlaying),
            controlColor(HeaderControl::nowPlaying, PanelColor::accent));
        const auto root = GetAncestor(window, GA_ROOT);
        render_.glyph(
            PanelGlyph::windowMinimize,
            visualRect(layout, HeaderControl::minimize),
            controlColor(HeaderControl::minimize, PanelColor::muted));
        render_.glyph(root != nullptr && IsZoomed(root)
                ? PanelGlyph::windowRestore : PanelGlyph::windowMaximize,
            visualRect(layout, HeaderControl::maximize),
            controlColor(HeaderControl::maximize, PanelColor::muted));
        render_.glyph(
            PanelGlyph::windowClose,
            visualRect(layout, HeaderControl::close),
            controlColor(HeaderControl::close, PanelColor::muted));
        if (headerWordmarkEnabled()) {
            const auto availableRight = layout.tag.right > layout.tag.left
                ? layout.tag.left - 10.0f
                : layout.daw.right > layout.daw.left
                ? layout.daw.left - 10.0f
                : layout.monitor.right > layout.monitor.left
                ? layout.monitor.left - 10.0f
                : layout.listen.left - 10.0f;
            const auto wordmarkLeft = layout.menu.right + 10.0f;
            if (availableRight - wordmarkLeft >= 122.0f) {
                const auto wordmarkTop = (menuHeight - 20.0f) * 0.5f;
                render_.godenWordmark(D2D1::RectF(
                    wordmarkLeft,
                    wordmarkTop,
                    std::min(availableRight, wordmarkLeft + 142.0f),
                    wordmarkTop + 20.0f));
            }
        }
        const auto lowerCenter = (menuHeight + size.height) * 0.5f;
        render_.nekoLogo(D2D1::RectF(
            8.0f, lowerCenter - 7.5f, 23.0f, lowerCenter + 7.5f));
        const auto theme = ThemeService::instance().snapshot();
        GlyphFlowDrawStyle glyphStyle;
        glyphStyle.color = D2D1::ColorF(
            theme.accent.red,
            theme.accent.green,
            theme.accent.blue,
            1.0f);
        glyphStyle.strength = 0.095f;
        glyphFlowRenderer_.draw(
            render_.target(),
            D2D1::RectF(
                28.0f,
                menuHeight + 2.0f,
                std::max(28.0f, layout.stop.left - 8.0f),
                size.height - 2.0f),
            motionMode(),
            glyphStyle);
        auto title = utf8ToWide(Runtime::instance().nowPlayingText());
        if (title == L"Stopped") title = L"foobar2000";
        render_.text(title,
            D2D1::RectF(30.0f, menuHeight, layout.stop.left - 8.0f, size.height),
            PanelText::strong, PanelColor::bright);

        render_.glyph(
            PanelGlyph::stop,
            visualRect(layout, HeaderControl::stop),
            controlColor(HeaderControl::stop, PanelColor::muted));
        render_.glyph(
            PanelGlyph::previous,
            visualRect(layout, HeaderControl::previous),
            controlColor(HeaderControl::previous, PanelColor::text));
        drawPlaybackGlyph(
            visualRect(layout, HeaderControl::play),
            controlColor(
                HeaderControl::play,
                Runtime::instance().playbackSnapshot().state !=
                        PlaybackState::stopped
                    ? PanelColor::accent : PanelColor::text));
        render_.glyph(
            PanelGlyph::next,
            visualRect(layout, HeaderControl::next),
            controlColor(HeaderControl::next, PanelColor::text));
        retainedHeaderReady_ = SUCCEEDED(render_.end());
        glyphFlowRenderer_.presented(render_.lastFrameResult() == FrameResult::accepted);
        if (scopeGpu_.finishFrame(render_.lastFrameOutcome().drawHr)) {
            retainedHeaderReady_ = false;
            InvalidateRect(window_, nullptr, FALSE);
        }
        if (retainedHeaderReady_) {
            // Keep full damage pending through the last transition frame;
            // scope-only repainting must not retain an intermediate palette.
            themeWasActive_ = themeSampleActive;
            glyphWasActive_ = glyphSampleActive;
        }
        EndPaint(window, &paintStruct);
    }

    RECT monitorPixelRect() const noexcept
    {
        const auto size = clientSizeDip(window_);
        const auto monitor = headerLayout(size.width, size.height).monitor;
        const auto scale = static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        constexpr float padding = 3.0f;
        return {
            static_cast<LONG>(std::floor((monitor.left - padding) * scale)),
            static_cast<LONG>(std::floor((monitor.top - padding) * scale)),
            static_cast<LONG>(std::ceil((monitor.right + padding) * scale)),
            static_cast<LONG>(std::ceil((monitor.bottom + padding) * scale)),
        };
    }

    void syncMonitorPlayback()
    {
        if (window_ == nullptr) return;
        const auto playback = Runtime::instance().playbackSnapshot();
        const auto streamGeneration = Runtime::instance().streamGeneration();
        if (streamGeneration != monitorStreamGeneration_) {
            monitorStreamGeneration_ = streamGeneration;
            // Pause is a discontinuity for future audio, not a request to
            // erase the compact FL-style monitor. Preserve its last complete
            // trace while paused; a playing generation replaces it as soon as
            // real samples arrive.
            if (playback.state == PlaybackState::playing ||
                    Runtime::instance().dawPreviewActive()) {
                monitorPresentation_.reset();
                monitorAnalysisGeneration_ = 0;
                monitorPresentationQpc_.QuadPart = 0;
            }
        }

        const bool shouldRun =
            (playback.state == PlaybackState::playing ||
                Runtime::instance().dawPreviewActive()) &&
            canPresent(window_);
        if (shouldRun && !monitorRealtimeActive_) {
            Runtime::instance().setRealtimeConsumerActive(
                RealtimeConsumer::compactMonitor, true);
            (void)RealtimeAnalysisHub::instance().setDemand(
                AnalysisOwner::header, analysisLifetime_,
                kAnalysisFeatureCompactScope,
                playback.durationSeconds,
                Runtime::instance().latestRealtimeSequence());
            monitorRealtimeActive_ = true;
            syncPresentationClock();
        } else if (!shouldRun && monitorRealtimeActive_) {
            if (canPresent(window_) &&
                    (playback.state == PlaybackState::paused ||
                     playback.state == PlaybackState::stopped)) {
                (void)monitorPresentation_.beginRelease();
            }
            stopMonitorRealtime();
        }
    }

    PanelGlyph currentPlaybackGlyph() const
    {
        return Runtime::instance().playbackSnapshot().state ==
                PlaybackState::playing
            ? PanelGlyph::pause
            : PanelGlyph::play;
    }

    void syncPlaybackGlyphMotion()
    {
        const auto next = currentPlaybackGlyph();
        if (next == playbackGlyph_) return;
        playbackGlyph_ = next;
    }

    void drawPlaybackGlyph(
        const D2D1_RECT_F& rectangle,
        PanelColor color)
    {
        render_.glyph(playbackGlyph_, rectangle, color);
    }

    void stopMonitorRealtime()
    {
        if (monitorRealtimeActive_) {
            RealtimeAnalysisHub::instance().removeDemand(
                AnalysisOwner::header, analysisLifetime_);
            Runtime::instance().setRealtimeConsumerActive(
                RealtimeConsumer::compactMonitor, false);
            monitorRealtimeActive_ = false;
        }
        syncPresentationClock();
    }

    void syncPresentationClock()
    {
        if (!canPresent(window_)) {
            monitorClock_.stop();
            return;
        }
        const auto motionActive = motion_.needsFrame();
        if (motionActive) motionWasActive_ = true;
        if (ThemeService::instance().transitionActive()) themeWasActive_ = true;
        if (GlyphFlowService::instance().active(motionMode())) glyphWasActive_ = true;
        if (monitorRealtimeActive_ || monitorPresentation_.active() || motionActive ||
            themeWasActive_ || glyphWasActive_ ||
            ThemeService::instance().transitionActive() ||
            GlyphFlowService::instance().active(motionMode())) {
            monitorClock_.start();
        } else {
            monitorClock_.stop();
        }
    }

    void tickMonitor()
    {
        if (!canPresent(window_)) {
            stopMonitorRealtime();
            return;
        }
        bool changed = false;
        if (monitorRealtimeActive_) {
            Runtime::instance().pumpVisualisation();
            const auto analysis = RealtimeAnalysisHub::instance().snapshot();
            if (analysis && analysis->generation != monitorAnalysisGeneration_ &&
                    (analysis->featureMask & kAnalysisFeatureCompactScope) != 0) {
                monitorAnalysisGeneration_ = analysis->generation;
                changed = monitorPresentation_.syncTarget(
                    analysis->scope) || changed;
            }
        }
        changed = monitorPresentation_.advance(
            presentationElapsedSeconds(
                monitorPresentationQpc_,
                monitorClock_.intervalSeconds())) || changed;
        const auto motionActive = motion_.needsFrame();
        const auto themeActive =
            ThemeService::instance().transitionActive();
        const auto glyphActive =
            GlyphFlowService::instance().active(motionMode());
        if (changed || motionActive || themeActive || glyphActive ||
            motionWasActive_ || themeWasActive_ || glyphWasActive_) {
            if (changed && !motionActive && !themeActive && !glyphActive &&
                !motionWasActive_ && !themeWasActive_ && !glyphWasActive_ && retainedHeaderReady_) {
                const auto monitor = monitorPixelRect();
                InvalidateRect(window_, &monitor, FALSE);
                // The enclosing WM_PAINT draws this retained region once.
            } else if (themeActive) {
                presentBoundedThemeFrame(window_);
            } else {
                InvalidateRect(window_, nullptr, FALSE);
            }
        }
        motionWasActive_ = motionActive;
        if (!monitorRealtimeActive_ && !monitorPresentation_.active() && !motionActive &&
            !themeActive && !glyphActive && !themeWasActive_ && !glyphWasActive_) {
            monitorClock_.stop();
        }
    }

    double hoverValue(HeaderControl control) const noexcept
    {
        return motion_.sample(
            kHeaderHoverMotionBase +
            static_cast<MotionKey>(control)).value;
    }

    double pressValue(HeaderControl control) const noexcept
    {
        return motion_.sample(
            kHeaderPressMotionBase +
            static_cast<MotionKey>(control)).value;
    }

    D2D1_RECT_F visualRect(
        const HeaderLayout& layout,
        HeaderControl control) const noexcept
    {
        return shiftedControlRect(
            headerControlRect(layout, control),
            hoverValue(control),
            pressValue(control));
    }

    PanelColor controlColor(
        HeaderControl control,
        PanelColor base) const noexcept
    {
        return interactionColor(
            base,
            hoverValue(control),
            pressValue(control));
    }

    void drawControlSurface(
        HeaderControl control,
        const D2D1_RECT_F& rectangle)
    {
        const auto opacity = interactionOpacity(
            hoverValue(control),
            pressValue(control));
        if (opacity > 0.001f) {
            render_.fill(rectangle, PanelColor::selected, opacity);
        }
    }

    HeaderControl hitTest(LPARAM lParam) const noexcept
    {
        const auto [x, y] = clientPointDip(window_, lParam);
        const auto size = clientSizeDip(window_);
        return hitHeaderControl(
            headerLayout(size.width, size.height),
            x,
            y);
    }

    void createTooltips()
    {
        tooltips_.create(window_);
        for (int index = 0; index < kHeaderControlCount; ++index) {
            const auto control = static_cast<HeaderControl>(index);
            tooltips_.add(
                static_cast<UINT_PTR>(index + 1),
                headerTooltipText(control));
        }
        syncTooltips();
    }

    void syncTooltips() const
    {
        if (window_ == nullptr) return;
        const auto size = clientSizeDip(window_);
        const auto layout = headerLayout(size.width, size.height);
        for (int index = 0; index < kHeaderControlCount; ++index) {
            tooltips_.update(
                static_cast<UINT_PTR>(index + 1),
                headerControlRect(
                    layout,
                    static_cast<HeaderControl>(index)));
        }
    }

    void pointerDown(LPARAM lParam, bool allowMove)
    {
        const auto control = hitTest(lParam);
        if (control == HeaderControl::none) {
            if (allowMove) beginMove(lParam);
            return;
        }

        pressedControl_ = control;
        pressedInside_ = true;
        setHoverControl(control);
        retargetPress(control);
        SetCapture(window_);
    }

    void pointerUp(LPARAM lParam)
    {
        if (pressedControl_ == HeaderControl::none) return;
        const auto pressed = pressedControl_;
        const bool activate = hitTest(lParam) == pressed;
        pressedControl_ = HeaderControl::none;
        pressedInside_ = false;
        retargetPress(HeaderControl::none);
        if (GetCapture() == window_) ReleaseCapture();
        if (activate) executeControl(pressed);
    }

    void cancelPress()
    {
        if (pressedControl_ == HeaderControl::none) return;
        pressedControl_ = HeaderControl::none;
        pressedInside_ = false;
        retargetPress(HeaderControl::none);
        if (GetCapture() == window_) ReleaseCapture();
    }

    void executeControl(HeaderControl selected)
    {
        auto control = playback_control::get();
        switch (selected) {
        case HeaderControl::menu: {
            const auto size = clientSizeDip(window_);
            const auto layout = headerLayout(size.width, size.height);
            const auto dpi = static_cast<float>(GetDpiForWindow(window_));
            POINT point{
                static_cast<LONG>(
                    std::lround(layout.menu.left * dpi / 96.0f)),
                static_cast<LONG>(
                    std::lround(layout.menu.bottom * dpi / 96.0f)),
            };
            ClientToScreen(window_, &point);
            showMainMenu(window_, point);
            break;
        }
        case HeaderControl::tag:
            if (rolloutFlagEnabled(RolloutFlag::tagEditorV1)) {
                TagEditorWindow::instance().show(window_);
            }
            break;
        case HeaderControl::listen:
            ListenWindow::instance().show();
            break;
        case HeaderControl::album:
            AlbumSessionService::instance().toggleFromHostContext();
            break;
        case HeaderControl::dsp:
            MasterDspWindow::instance().show(window_);
            break;
        case HeaderControl::daw:
            DawWindow::instance().show(window_);
            break;
        case HeaderControl::options:
            OptionsWindow::instance().show(window_);
            break;
        case HeaderControl::queue:
            toggleWorkspaceQueue();
            break;
        case HeaderControl::nowPlaying:
            if (!revealNowPlayingInWorkspace()) {
                playlist_manager::get()->highlight_playing_item();
            }
            break;
        case HeaderControl::minimize:
            ShowWindow(GetAncestor(window_, GA_ROOT), SW_MINIMIZE);
            break;
        case HeaderControl::maximize:
            toggleFramelessMaximize(GetAncestor(window_, GA_ROOT));
            break;
        case HeaderControl::close:
            PostMessageW(GetAncestor(window_, GA_ROOT), WM_CLOSE, 0, 0);
            break;
        case HeaderControl::stop:
            AlbumSessionService::instance().cancelContinuation();
            control->stop();
            break;
        case HeaderControl::previous:
            control->previous();
            break;
        case HeaderControl::play:
            requestPlaybackPauseToggle();
            break;
        case HeaderControl::next:
            if (!KeepPlayingService::instance().handleNextCommand()) {
                control->next();
            }
            break;
        case HeaderControl::none:
        case HeaderControl::count:
            break;
        }
    }

    bool isTopControl(const HeaderLayout& layout, float x, float y) const noexcept
    {
        return pointIn(layout.menu, x, y) || pointIn(layout.tag, x, y) ||
            pointIn(layout.dsp, x, y) ||
            pointIn(layout.daw, x, y) ||
            pointIn(layout.options, x, y) || pointIn(layout.listen, x, y) ||
            pointIn(layout.queue, x, y) || pointIn(layout.nowPlaying, x, y) ||
            pointIn(layout.minimize, x, y) || pointIn(layout.maximize, x, y) ||
            pointIn(layout.close, x, y);
    }

    void beginMove(LPARAM lParam)
    {
        const auto [x, y] = clientPointDip(window_, lParam);
        const auto size = clientSizeDip(window_);
        const auto layout = headerLayout(size.width, size.height);
        if (y < 38.0f && !isTopControl(layout, x, y)) {
            beginFramelessMove(GetAncestor(window_, GA_ROOT));
        }
    }

    void toggleMaximize(LPARAM lParam)
    {
        const auto [x, y] = clientPointDip(window_, lParam);
        const auto size = clientSizeDip(window_);
        const auto layout = headerLayout(size.width, size.height);
        if (y < 38.0f && !isTopControl(layout, x, y)) {
            toggleFramelessMaximize(GetAncestor(window_, GA_ROOT));
        }
    }

    void showSystemMenu(LPARAM lParam)
    {
        const auto [x, y] = clientPointDip(window_, lParam);
        const auto size = clientSizeDip(window_);
        const auto layout = headerLayout(size.width, size.height);
        if (y >= 38.0f || isTopControl(layout, x, y)) return;
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(window_, &point);
        showFramelessSystemMenu(GetAncestor(window_, GA_ROOT), point);
    }

    void showContextMenu(LPARAM lParam)
    {
        if (hitTest(lParam) != HeaderControl::dsp) {
            showSystemMenu(lParam);
            return;
        }
        const auto state = MasterDspController::instance().snapshot();
        const auto menu = CreatePopupMenu();
        if (menu == nullptr) return;
        AppendMenuW(menu, MF_STRING, 1U, L"Open Master DSP");
        AppendMenuW(menu, MF_SEPARATOR, 0U, nullptr);
        AppendMenuW(menu, MF_STRING | (state.enabled ? MF_CHECKED : 0U),
            2U, L"Enable DSP");
        AppendMenuW(menu, MF_STRING | (state.bypassed ? MF_CHECKED : 0U) |
            (state.enabled ? 0U : MF_GRAYED), 3U, L"Bypass rack");
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(window_, &point);
        const auto command = TrackPopupMenuEx(menu,
            TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
            point.x, point.y, window_, nullptr);
        DestroyMenu(menu);
        if (command == 1U) {
            MasterDspWindow::instance().show(window_);
        } else if (command == 2U) {
            MasterDspController::instance().toggleEnabled();
        } else if (command == 3U) {
            MasterDspController::instance().toggleBypass();
        }
    }

    void updateHover(LPARAM lParam)
    {
        if (!trackingMouse_) {
            TRACKMOUSEEVENT tracking{
                sizeof(tracking), TME_LEAVE, window_, HOVER_DEFAULT};
            TrackMouseEvent(&tracking);
            trackingMouse_ = true;
        }
        const auto next = hitTest(lParam);
        setHoverControl(next);
        setPressInside(
            pressedControl_ != HeaderControl::none &&
            next == pressedControl_);
    }

    void setHoverControl(HeaderControl next)
    {
        if (next == hoverControl_) return;
        hoverControl_ = next;
        const auto mode = motionMode();
        for (int index = 0; index < kHeaderControlCount; ++index) {
            motion_.retarget(
                kHeaderHoverMotionBase +
                    static_cast<MotionKey>(index),
                MotionToken::hover,
                mode,
                static_cast<HeaderControl>(index) == hoverControl_
                    ? 1.0 : 0.0);
        }
        syncPresentationClock();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void setPressInside(bool inside)
    {
        if (pressedControl_ == HeaderControl::none ||
            pressedInside_ == inside) {
            return;
        }
        pressedInside_ = inside;
        retargetPress(inside
                ? pressedControl_
                : HeaderControl::none);
    }

    void retargetPress(HeaderControl active)
    {
        const auto mode = motionMode();
        for (int index = 0; index < kHeaderControlCount; ++index) {
            motion_.retarget(
                kHeaderPressMotionBase +
                    static_cast<MotionKey>(index),
                MotionToken::press,
                mode,
                static_cast<HeaderControl>(index) == active
                    ? 1.0 : 0.0);
        }
        syncPresentationClock();
        InvalidateRect(window_, nullptr, FALSE);
    }

    HWND window_ = nullptr;
    PanelD2D render_;
    CompactScopeGpu scopeGpu_;
    WindowFrameClock monitorClock_;
    std::uint64_t fullPaints_ = 0, partialPaints_ = 0, monitorTicks_ = 0;
    MotionCoordinator motion_;
    GlyphFlowRenderer glyphFlowRenderer_;
    NativeTooltipHost tooltips_;
    CompactScopePresenter monitorPresentation_;
    LARGE_INTEGER monitorPresentationQpc_{};
    std::uint64_t analysisLifetime_ = 0;
    std::uint64_t monitorAnalysisGeneration_ = 0;
    std::uint64_t monitorStreamGeneration_ = 0;
    HeaderControl hoverControl_ = HeaderControl::none;
    HeaderControl pressedControl_ = HeaderControl::none;
    bool trackingMouse_ = false;
    bool pressedInside_ = false;
    bool monitorRealtimeActive_ = false;
    bool motionWasActive_ = false;
    bool themeWasActive_ = false;
    bool glyphWasActive_ = false;
    bool retainedHeaderReady_ = false;
    std::uint64_t cachedOverlayPaints_ = 0;
    PanelGlyph playbackGlyph_ = PanelGlyph::play;
};

class MonitorPanel : public uie::container_uie_window_v3 {
public:
    const GUID& get_extension_guid() const override { return kMonitorPanelGuid; }
    void get_name(pfc::string_base& output) const override { output = "GODENplayer Monitor"; }
    void get_category(pfc::string_base& output) const override { output = "Panels"; }
    unsigned get_type() const override { return uie::type_panel; }

private:
    uie::container_window_v3_config get_window_config() override
    {
        return {L"NekoRoXMonitorV3", false};
    }

    LRESULT on_message(HWND window, UINT message, WPARAM wParam, LPARAM lParam) override
    {
        switch (message) {
        case WM_CREATE:
            window_ = window;
            frameClock_.attach(window_, kMonitorFrameMessage);
            frameClock_.setTelemetrySurface(TelemetrySurface::headerMonitor);
            registerNativeWindow(window_);
            syncPlayback();
            return 0;
        case WM_DESTROY:
            stopRealtime();
            frameClock_.detach();
            unregisterNativeWindow(window_);
            window_ = nullptr;
            scopeGpu_.reset();
            render_.reset();
            return 0;
        case WM_ERASEBKGND:
            return TRUE;
        case WM_SHOWWINDOW:
            syncPlayback();
            break;
        case WM_SIZE:
            render_.resize();
            syncPlayback();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED:
            scopeGpu_.releaseTargetResources();
            render_.reset();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_PAINT:
            paint(window);
            return 0;
        case kMonitorFrameMessage:
            if (frameClock_.consume(wParam)) {
                tick();
            }
            return 0;
        case kNativeRefreshMessage:
            (void)consumeNativeInvalidation(window);
            syncPlayback();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case kThemeTransitionMessage:
            syncClock();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void syncPlayback()
    {
        if (window_ == nullptr) return;
        const auto playback = Runtime::instance().playbackSnapshot();
        const auto streamGeneration = Runtime::instance().streamGeneration();
        if (streamGeneration != streamGeneration_) {
            streamGeneration_ = streamGeneration;
            frameSequence_ = 0;
            scope_.reset();
            presentation_.reset();
            presentationQpc_.QuadPart = 0;
        }

        // The standalone Monitor panel is used by older/migrated layouts.
        // DAW preview pauses the normal foobar transport, but it still
        // publishes post-rack PCM through the shared realtime bridge.  Keep
        // this consumer alive for that source just like the integrated
        // Header monitor and Listen Live do.
        const bool shouldRun =
            (playback.state == PlaybackState::playing ||
                Runtime::instance().dawPreviewActive()) &&
            canPresent(window_);
        if (shouldRun && !realtimeActive_) {
            Runtime::instance().setRealtimeConsumerActive(
                RealtimeConsumer::compactMonitor, true);
            realtimeActive_ = true;
        } else if (!shouldRun && realtimeActive_) {
            Runtime::instance().setRealtimeConsumerActive(
                RealtimeConsumer::compactMonitor, false);
            realtimeActive_ = false;
        }
        syncClock();
    }

    void stopRealtime()
    {
        frameClock_.stop();
        if (!realtimeActive_) return;
        Runtime::instance().setRealtimeConsumerActive(
            RealtimeConsumer::compactMonitor, false);
        realtimeActive_ = false;
    }

    void tick()
    {
        if (!canPresent(window_)) {
            stopRealtime();
            return;
        }
        bool changed = false;
        if (realtimeActive_) {
            Runtime::instance().pumpVisualisation();
            for (int pending = 0; pending < 8; ++pending) {
                if (!Runtime::instance().consumeNextFrame(
                        RealtimeConsumer::compactMonitor,
                        frame_, frameSequence_, nullptr)) {
                    break;
                }
                changed = scope_.ingest(frame_) || changed;
            }
            changed = presentation_.syncTarget(scope_) || changed;
            changed = presentation_.advance(
                presentationElapsedSeconds(
                    presentationQpc_,
                    frameClock_.intervalSeconds())) || changed;
        }
        const auto themeActive =
            ThemeService::instance().transitionActive();
        if (changed || themeActive) {
            InvalidateRect(window_, nullptr, FALSE);
        }
        syncClock();
    }

    void syncClock()
    {
        const auto active = canPresent(window_) &&
            (realtimeActive_ || ThemeService::instance().transitionActive());
        if (active) frameClock_.start();
        else frameClock_.stop();
    }

    void paint(HWND window)
    {
        PAINTSTRUCT paintStruct{};
        BeginPaint(window, &paintStruct);
        if (!render_.begin(window, paintStruct.hdc)) {
            EndPaint(window, &paintStruct);
            return;
        }

        const auto size = render_.sizeDip();
        drawCompactScope(render_,
            D2D1::RectF(4.0f, 2.0f, size.width - 4.0f, size.height - 2.0f),
            presentation_, scopeGpu_);

        render_.end();
        if (scopeGpu_.finishFrame(render_.lastFrameOutcome().drawHr))
            InvalidateRect(window_, nullptr, FALSE);
        EndPaint(window, &paintStruct);
    }

    HWND window_ = nullptr;
    PanelD2D render_;
    CompactScopeGpu scopeGpu_;
    WindowFrameClock frameClock_;
    CompactScopeModel scope_;
    CompactScopePresenter presentation_;
    RealtimeFrameV1 frame_{};
    LARGE_INTEGER presentationQpc_{};
    std::uint64_t frameSequence_ = 0;
    std::uint64_t streamGeneration_ = 0;
    bool realtimeActive_ = false;
};

struct TransportLayout {
    D2D1_RECT_F elapsed{};
    D2D1_RECT_F remaining{};
    D2D1_RECT_F keepPlaying{};
    D2D1_RECT_F repeat{};
    D2D1_RECT_F shuffle{};
    D2D1_RECT_F volume{};
    D2D1_RECT_F mute{};
};

enum class TransportControl : int {
    none = -1,
    keepPlaying = 0,
    repeat,
    shuffle,
    volume,
    mute,
    count,
};

constexpr auto kTransportControlCount =
    static_cast<int>(TransportControl::count);

TransportLayout transportLayout(float width, float height)
{
    const auto middle = height * 0.5f;
    TransportLayout result;
    result.elapsed = D2D1::RectF(7.0f, 0.0f, 64.0f, height);
    result.mute = D2D1::RectF(width - 30.0f, 2.0f, width - 4.0f, height - 2.0f);
    result.volume = D2D1::RectF(width - 126.0f, middle - 6.0f, width - 34.0f, middle + 6.0f);
    result.shuffle = D2D1::RectF(width - 158.0f, 2.0f, width - 132.0f, height - 2.0f);
    result.repeat = D2D1::RectF(width - 190.0f, 2.0f, width - 164.0f, height - 2.0f);
    result.keepPlaying = D2D1::RectF(width - 222.0f, 2.0f, width - 196.0f, height - 2.0f);
    result.remaining = D2D1::RectF(width - 292.0f, 0.0f, width - 228.0f, height);
    return result;
}

D2D1_RECT_F transportControlRect(
    const TransportLayout& layout,
    TransportControl control) noexcept
{
    switch (control) {
    case TransportControl::keepPlaying: return layout.keepPlaying;
    case TransportControl::repeat: return layout.repeat;
    case TransportControl::shuffle: return layout.shuffle;
    case TransportControl::volume: return layout.volume;
    case TransportControl::mute: return layout.mute;
    case TransportControl::none:
    case TransportControl::count:
        break;
    }
    return {};
}

TransportControl hitTransportControl(
    const TransportLayout& layout,
    float x,
    float y) noexcept
{
    constexpr std::array order{
        TransportControl::mute,
        TransportControl::shuffle,
        TransportControl::repeat,
        TransportControl::keepPlaying,
        TransportControl::volume,
    };
    for (const auto control : order) {
        if (pointIn(transportControlRect(layout, control), x, y)) return control;
    }
    return TransportControl::none;
}

const wchar_t* transportTooltipText(TransportControl control) noexcept
{
    switch (control) {
    case TransportControl::keepPlaying:
        return L"Keep Playing; left-click toggles, right-click selects Random Albums or Sequence";
    case TransportControl::repeat:
        return L"Repeat playlist; right-click for playback order";
    case TransportControl::shuffle:
        return L"Shuffle tracks; right-click for playback order";
    case TransportControl::volume: return L"Volume";
    case TransportControl::mute: return L"Mute or unmute";
    case TransportControl::none:
    case TransportControl::count:
        break;
    }
    return nullptr;
}

class TransportPanel : public uie::container_uie_window_v3 {
public:
    const GUID& get_extension_guid() const override { return kTransportPanelGuid; }
    void get_name(pfc::string_base& output) const override { output = "GODENplayer Transport"; }
    void get_category(pfc::string_base& output) const override { output = "Panels"; }
    unsigned get_type() const override { return uie::type_panel; }

private:
    enum class DragMode {
        none,
        volume,
    };

    uie::container_window_v3_config get_window_config() override
    {
        return {L"NekoRoXTransportV3", false};
    }

    LRESULT on_message(HWND window, UINT message, WPARAM wParam, LPARAM lParam) override
    {
        if (message == transportStateMessage()) {
            switch (wParam) {
            case 0U:
                return static_cast<LRESULT>(queryTransportState());
            case 1U:
                return static_cast<LRESULT>(playbackPaints_);
            case 2U:
                return static_cast<LRESULT>(frameClock_.deliveredPulses());
            case 3U:
                return static_cast<LRESULT>(frameClock_.skippedDeadlines());
            case 4U:
                return static_cast<LRESULT>(
                    frameClock_.targetRefreshMilliHz());
            case 5U:
                return static_cast<LRESULT>(
                    Runtime::instance().playbackSnapshot().state);
            case 6U:
                return static_cast<LRESULT>(
                    Runtime::instance().playbackSnapshot().generation);
            case 7U:
                return static_cast<LRESULT>(
                    Runtime::instance().playbackSnapshot().trackIdentityHash);
            case 8U:
                return static_cast<LRESULT>(std::llround(
                    Runtime::instance().playbackSnapshot().positionSeconds *
                    1000.0));
            case 9U:
                return static_cast<LRESULT>(std::llround(
                    Runtime::instance().playbackSnapshot().durationSeconds *
                    1000.0));
            case 10U: {
                const auto control = playback_control::get();
                return control->is_playing()
                    ? (control->is_paused() ? 2 : 1) : 0;
            }
            default:
                return E_INVALIDARG;
            }
        }
        switch (message) {
        case WM_CREATE:
            window_ = window;
            frameClock_.attach(window_, kTransportFrameMessage);
            frameClock_.setTelemetrySurface(TelemetrySurface::transport);
            frameClock_.setLatencyCritical(true);
            refreshPlaybackContext(true);
            for (int index = 0; index < kTransportControlCount; ++index) {
                motion_.begin(
                    kTransportHoverMotionBase +
                        static_cast<MotionKey>(index),
                    MotionToken::hover,
                    MotionMode::off,
                    0.0,
                    0.0);
                motion_.begin(
                    kTransportPressMotionBase +
                        static_cast<MotionKey>(index),
                    MotionToken::press,
                    MotionMode::off,
                    0.0,
                    0.0);
            }
            createTooltips();
            registerNativeWindow(window_);
            syncClock();
            return 0;
        case WM_DESTROY:
            cancelPointer();
            tooltips_.destroy();
            motion_.clear();
            frameClock_.detach();
            unregisterNativeWindow(window_);
            window_ = nullptr;
            render_.reset();
            return 0;
        case WM_ERASEBKGND:
            return TRUE;
        case WM_SHOWWINDOW:
            syncClock();
            break;
        case WM_SIZE:
            render_.resize();
            seekPresentation_.reset();
            syncTooltips();
            syncClock();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO:
            applyFixedBandLimits(
                reinterpret_cast<MINMAXINFO*>(lParam),
                window,
                UiFixedBand::transport);
            return 0;
        case WM_DPICHANGED:
            notifyFixedBandDpiChanged(get_host(), window);
            seekPresentation_.reset();
            render_.reset();
            syncTooltips();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_PAINT:
            paint(window);
            return 0;
        case WM_LBUTTONDOWN:
            pointerDown(lParam);
            return 0;
        case WM_MOUSEMOVE:
            updateHover(lParam);
            if (drag_ != DragMode::none && (wParam & MK_LBUTTON) != 0) updateDrag(lParam);
            return 0;
        case WM_LBUTTONUP:
            pointerUp(lParam);
            return 0;
        case WM_MOUSELEAVE:
            trackingMouse_ = false;
            setHoverControl(TransportControl::none);
            setPressInside(false);
            return 0;
        case WM_RBUTTONUP:
            showPlaybackOrderMenu(lParam);
            return 0;
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
        case WM_KILLFOCUS:
            trackingMouse_ = false;
            setHoverControl(TransportControl::none);
            cancelPointer();
            return 0;
        case kTransportFrameMessage:
            if (frameClock_.consume(wParam)) {
                const auto skipped = frameClock_.skippedDeadlines();
                const bool recoveringDeadline =
                    skipped != observedSkippedDeadlines_;
                observedSkippedDeadlines_ = skipped;
                if (recoveringDeadline || shouldPresentPlaybackFrame()) {
                    // WM_PAINT is lower priority than posted frame messages.
                    // Present the frame selected by the gate immediately so a
                    // busy queue cannot leave the playhead frozen until input.
                    // Active scrolling keeps the asynchronous path to protect
                    // input latency and catches up on the next quiet pulse.
                    if (ThemeService::instance().transitionActive()) {
                        presentBoundedThemeFrame(window);
                    } else {
                        InvalidateRect(window, nullptr, FALSE);
                    }
                }
                syncClock();
                return 0;
            }
            break;
        case kNativeRefreshMessage:
            if (const auto dirty = consumeNativeInvalidation(window);
                any(dirty & (UiDirty::semantic | UiDirty::artwork))) {
                refreshPlaybackContext(false);
            }
            syncClock();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case kThemeTransitionMessage:
            syncClock();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void paint(HWND window)
    {
        PAINTSTRUCT paintStruct{};
        BeginPaint(window, &paintStruct);
        if (!render_.begin(window, paintStruct.hdc)) {
            EndPaint(window, &paintStruct);
            return;
        }
        const auto size = render_.sizeDip();
        const auto layout = transportLayout(size.width, size.height);
        const auto control = playback_control::get();
        const auto playback = Runtime::instance().playbackSnapshot();
        const bool playing = playback.state != PlaybackState::stopped;
        const auto position = playing
            ? Runtime::instance().presentationPlaybackPosition() : 0.0;
        const auto duration = playing ? playback.durationSeconds : 0.0;
        const auto presentationStep = elapsedPresentationStep(position);

        render_.line(0.0f, 0.0f, size.width, 0.0f, PanelColor::border);
        render_.line(0.0f, size.height - 1.0f, size.width, size.height - 1.0f, PanelColor::border);
        render_.text(playbackTime(position), layout.elapsed, PanelText::strong, PanelColor::bright);
        render_.text(duration > 0.0
                ? L"-" + playbackTime(std::max(0.0, duration - position))
                : (playing ? L"Live stream" : L"\u2014"),
            layout.remaining, PanelText::right, PanelColor::bright);

        drawPlaybackContext(layout, size.height);

        for (int index = 0; index < kTransportControlCount; ++index) {
            const auto selected = static_cast<TransportControl>(index);
            drawControlSurface(
                selected,
                transportControlRect(layout, selected));
        }

        const auto playbackOrder = capturePlaybackOrder();
        const auto& keepPlaying = KeepPlayingService::instance();
        render_.glyph(
            PanelGlyph::keepPlaying,
            visualRect(layout, TransportControl::keepPlaying),
            controlColor(
                TransportControl::keepPlaying,
                keepPlaying.enabled() ? PanelColor::accent : PanelColor::muted));
        render_.repeatGlyph(
            playbackOrder.repeatOne,
            visualRect(layout, TransportControl::repeat),
            controlColor(
                TransportControl::repeat,
                playbackOrder.repeat ? PanelColor::accent : PanelColor::muted));
        render_.glyph(
            PanelGlyph::shuffle,
            visualRect(layout, TransportControl::shuffle),
            controlColor(
                TransportControl::shuffle,
                playbackOrder.shuffle ? PanelColor::accent : PanelColor::muted));

        const auto volume = control->get_volume();
        const auto normalizedVolume = control->is_muted()
            ? 0.0
            : std::clamp(std::pow(10.0, volume / 20.0), 0.0, 1.0);
        drawBar(
            layout.volume,
            normalizedVolume,
            true,
            TransportControl::volume);
        auto volumeGlyph = PanelGlyph::volumeHigh;
        if (control->is_muted()) volumeGlyph = PanelGlyph::volumeMuted;
        else if (normalizedVolume < 0.34) volumeGlyph = PanelGlyph::volumeLow;
        else if (normalizedVolume < 0.67) volumeGlyph = PanelGlyph::volumeMedium;
        render_.glyph(
            volumeGlyph,
            visualRect(layout, TransportControl::mute),
            controlColor(
                TransportControl::mute,
                control->is_muted() ? PanelColor::muted : PanelColor::text));

        const auto result = render_.end();
        if (SUCCEEDED(result)) {
            seekPresentation_.markPresented(
                presentationStep,
                presentationQpcSeconds(),
                frameClock_.targetRefreshMilliHz());
            if (playback.state == PlaybackState::playing) ++playbackPaints_;
        }
        EndPaint(window, &paintStruct);
    }

    void drawBar(
        const D2D1_RECT_F& hitbox,
        double normalized,
        bool enabled,
        TransportControl control)
    {
        const auto hover = hoverValue(control);
        const auto press = pressValue(control);
        const auto center = (hitbox.top + hitbox.bottom) * 0.5f;
        const auto left = hitbox.left;
        const auto right = hitbox.right;
        const auto halfThickness = static_cast<float>(
            2.0 + hover * 0.45 + press * 0.55);
        render_.fill(
            D2D1::RectF(
                left,
                center - halfThickness,
                right,
                center + halfThickness),
            PanelColor::selected);
        const auto progress = left + static_cast<float>(std::clamp(normalized, 0.0, 1.0)) * (right - left);
        render_.fill(D2D1::RectF(
                left,
                center - halfThickness,
                progress,
                center + halfThickness),
            enabled ? PanelColor::text : PanelColor::muted);
    }

    void refreshPlaybackContext(bool force)
    {
        const auto snapshot = Runtime::instance().playbackSnapshot();
        if (!force && playbackContextGeneration_ == snapshot.generation) {
            return;
        }
        playbackContextGeneration_ = snapshot.generation;
        playbackContext_.clear();
        playbackCodec_.clear();
        playbackFallbackBitrate_.clear();
        playbackFormatTail_.clear();
        playbackCostPath_.clear();
        playbackCostDuration_ = snapshot.durationSeconds;
        if (snapshot.path[0] != '\0') {
            try {
                const auto native = filesystem::g_get_native_path(snapshot.path.data());
                playbackCostPath_ = std::filesystem::path(utf8ToWide(native.c_str()));
            } catch (...) { /* Nonlocal playback keeps the decoder's rate. */ }
        }
        packetCostUpdated_ = {};
        if (snapshot.state == PlaybackState::stopped) {
            playbackContext_.clear();
            return;
        }
        if (!playbackContextScript_.is_valid()) {
            titleformat_compiler::get()->compile_safe(
                playbackContextScript_,
                "$upper($if2(%codec%,AUDIO))$tab()"
                "$if2(%bitrate%,?)$tab()"
                "$if2(%samplerate%,?) Hz  $char(183)  "
                "$caps($if2(%channels%,?))");
        }
        pfc::string8_fast output;
        if (playbackContextScript_.is_valid() &&
            playback_control::get()->playback_format_title(
                nullptr,
                output,
                playbackContextScript_,
                nullptr,
                playback_control::display_level_all)) {
            const auto formatted = utf8ToWide(output.c_str());
            const auto first = formatted.find(L'\t');
            const auto second = first == std::wstring::npos
                ? std::wstring::npos : formatted.find(L'\t', first + 1U);
            if (first != std::wstring::npos && second != std::wstring::npos) {
                playbackCodec_ = trimText(formatted.substr(0, first));
                playbackFallbackBitrate_ = trimText(
                    formatted.substr(first + 1U, second - first - 1U));
                playbackFormatTail_ = trimText(formatted.substr(second + 1U));
            }
        }
        updatePacketCostText(true);
    }

    void drawPlaybackContext(
        const TransportLayout& layout,
        float height)
    {
        refreshPlaybackContext(false);
        updatePacketCostText(false);
        const auto left = layout.elapsed.right + 14.0f;
        const auto right = layout.remaining.left - 14.0f;
        const auto available = right - left;
        if (available < 116.0f) return;

        const auto center = (left + right) * 0.5f;
        const auto labelWidth = std::min(
            420.0f,
            std::max(96.0f, available * 0.68f));
        const auto labelLeft = center - labelWidth * 0.5f;
        const auto labelRight = center + labelWidth * 0.5f;
        const auto lineY = height * 0.5f;
        if (labelLeft - left >= 8.0f) {
            render_.line(
                left,
                lineY,
                labelLeft - 7.0f,
                lineY,
                PanelColor::border);
        }
        if (right - labelRight >= 8.0f) {
            render_.line(
                labelRight + 7.0f,
                lineY,
                right,
                lineY,
                PanelColor::border);
        }
        render_.text(
            playbackContext_,
            D2D1::RectF(labelLeft, 0.0f, labelRight, height),
            PanelText::center,
            PanelColor::secondary,
            0.82f);
    }

    void updatePacketCostText(bool force)
    {
        const auto now = std::chrono::steady_clock::now();
        if (!force && packetCostUpdated_.time_since_epoch().count() != 0 &&
                now - packetCostUpdated_ < std::chrono::milliseconds(200))
            return;
        packetCostUpdated_ = now;
        const auto scrub = ScrubCoordinator::instance().snapshot();
        const auto previewing = scrub.active && scrub.auditioning;
        const auto presentationPosition = previewing
            ? scrub.visualSeconds
            : Runtime::instance().presentationPlaybackPosition();
        const auto cost = PacketCostController::instance().readyResultForTrack(
            playbackCostPath_);
        std::wstring rate = playbackFallbackBitrate_.empty()
            ? L"? kbps" : playbackFallbackBitrate_ + L" kbps";
        if (cost && packetCostDurationMatches(cost->durationSeconds, playbackCostDuration_) &&
                !cost->levels.empty()) {
            const auto& result = *cost;
            const auto start = std::max(0.0, presentationPosition - 0.25);
            const auto end = std::min(result.durationSeconds,
                presentationPosition + 0.25);
            double weighted = 0.0;
            double coverage = 0.0;
            if (end > start) {
                const auto& bins = result.levels.front().bins;
                auto bin = std::lower_bound(bins.begin(), bins.end(), start,
                    [](const PacketCostBin& candidate, double seconds) {
                        return candidate.startSeconds +
                            candidate.durationSeconds <= seconds;
                    });
                for (; bin != bins.end() && bin->startSeconds < end; ++bin) {
                    const auto overlap = std::max(0.0,
                        std::min(end, bin->startSeconds + bin->durationSeconds) -
                        std::max(start, bin->startSeconds));
                    weighted += overlap * bin->kilobitsPerSecond;
                    coverage += overlap;
                }
            }
            const auto requested = std::max(0.0, end - start);
            wchar_t buffer[96]{};
            if (requested > 0.0 && coverage >= requested * 0.9) {
                std::swprintf(buffer, std::size(buffer),
                    previewing
                        ? L"%.0f preview local / %.0f avg kbps"
                        : L"%.0f local / %.0f avg kbps",
                    weighted / coverage, result.stats.averageKbps);
            } else {
                std::swprintf(buffer, std::size(buffer),
                    L"%.0f avg kbps", result.stats.averageKbps);
            }
            rate = buffer;
        }
        playbackContext_ = playbackCodec_.empty() ? L"Local playback"
            : playbackCodec_ + L"  \u00b7  " + rate +
                (playbackFormatTail_.empty() ? L"" :
                    L"  \u00b7  " + playbackFormatTail_);
    }

    double hoverValue(TransportControl control) const noexcept
    {
        return motion_.sample(
            kTransportHoverMotionBase +
            static_cast<MotionKey>(control)).value;
    }

    double pressValue(TransportControl control) const noexcept
    {
        return motion_.sample(
            kTransportPressMotionBase +
            static_cast<MotionKey>(control)).value;
    }

    D2D1_RECT_F visualRect(
        const TransportLayout& layout,
        TransportControl control) const noexcept
    {
        return shiftedControlRect(
            transportControlRect(layout, control),
            hoverValue(control),
            pressValue(control));
    }

    PanelColor controlColor(
        TransportControl control,
        PanelColor base) const noexcept
    {
        return interactionColor(
            base,
            hoverValue(control),
            pressValue(control));
    }

    void drawControlSurface(
        TransportControl control,
        const D2D1_RECT_F& rectangle)
    {
        auto opacity = interactionOpacity(
            hoverValue(control),
            pressValue(control));
        if (control == TransportControl::volume) {
            opacity *= 0.35f;
        }
        if (opacity > 0.001f) {
            render_.fill(rectangle, PanelColor::selected, opacity);
        }
    }

    TransportControl hitTest(LPARAM lParam) const noexcept
    {
        const auto [x, y] = point(lParam);
        const auto size = clientSizeDip(window_);
        return hitTransportControl(
            transportLayout(size.width, size.height),
            x,
            y);
    }

    void createTooltips()
    {
        tooltips_.create(window_);
        for (int index = 0; index < kTransportControlCount; ++index) {
            const auto control = static_cast<TransportControl>(index);
            tooltips_.add(
                static_cast<UINT_PTR>(index + 1),
                transportTooltipText(control));
        }
        syncTooltips();
    }

    void syncTooltips() const
    {
        if (window_ == nullptr) return;
        const auto size = clientSizeDip(window_);
        const auto layout = transportLayout(size.width, size.height);
        for (int index = 0; index < kTransportControlCount; ++index) {
            tooltips_.update(
                static_cast<UINT_PTR>(index + 1),
                transportControlRect(
                    layout,
                    static_cast<TransportControl>(index)));
        }
    }

    void pointerDown(LPARAM lParam)
    {
        const auto selected = hitTest(lParam);
        if (selected == TransportControl::none) return;
        pressedControl_ = selected;
        pressedInside_ = true;
        setHoverControl(selected);
        retargetPress(selected);
        if (selected == TransportControl::volume) {
            drag_ = DragMode::volume;
        }
        SetCapture(window_);
        if (drag_ != DragMode::none) updateDrag(lParam);
    }

    void updateDrag(LPARAM lParam)
    {
        const auto [x, ignored] = point(lParam);
        const auto size = clientSizeDip(window_);
        const auto layout = transportLayout(size.width, size.height);
        if (drag_ == DragMode::volume) {
            const auto normalized = normalizedAt(layout.volume, x);
            const auto decibels = normalized <= 0.001
                ? static_cast<double>(playback_control::volume_mute)
                : std::max(-60.0, 20.0 * std::log10(normalized));
            playback_control::get()->set_volume(static_cast<float>(decibels));
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    void pointerUp(LPARAM lParam)
    {
        if (pressedControl_ == TransportControl::none) return;
        const auto pressed = pressedControl_;
        if (drag_ != DragMode::none) {
            updateDrag(lParam);
        }
        const bool activate =
            drag_ == DragMode::none && hitTest(lParam) == pressed;
        drag_ = DragMode::none;
        pressedControl_ = TransportControl::none;
        pressedInside_ = false;
        retargetPress(TransportControl::none);
        if (GetCapture() == window_) ReleaseCapture();
        if (activate) executeControl(pressed);
    }

    void cancelPointer()
    {
        if (drag_ == DragMode::none &&
            pressedControl_ == TransportControl::none) {
            return;
        }
        drag_ = DragMode::none;
        pressedControl_ = TransportControl::none;
        pressedInside_ = false;
        retargetPress(TransportControl::none);
        if (GetCapture() == window_) ReleaseCapture();
    }

    void executeControl(TransportControl selected)
    {
        switch (selected) {
        case TransportControl::keepPlaying:
            KeepPlayingService::instance().setEnabled(
                !KeepPlayingService::instance().enabled());
            break;
        case TransportControl::repeat:
            togglePlaybackOrder(L"repeat", L"repeat (playlist)");
            break;
        case TransportControl::shuffle:
            togglePlaybackOrder(L"shuffle", L"shuffle (tracks)");
            break;
        case TransportControl::mute:
            playback_control::get()->volume_mute_toggle();
            break;
        case TransportControl::volume:
        case TransportControl::none:
        case TransportControl::count:
            break;
        }
    }

    void updateHover(LPARAM lParam)
    {
        if (!trackingMouse_) {
            TRACKMOUSEEVENT tracking{
                sizeof(tracking), TME_LEAVE, window_, HOVER_DEFAULT};
            TrackMouseEvent(&tracking);
            trackingMouse_ = true;
        }
        const auto next = hitTest(lParam);
        setHoverControl(next);
        setPressInside(
            pressedControl_ != TransportControl::none &&
            next == pressedControl_);
    }

    void setHoverControl(TransportControl next)
    {
        if (next == hoverControl_) return;
        hoverControl_ = next;
        const auto mode = motionMode();
        for (int index = 0; index < kTransportControlCount; ++index) {
            motion_.retarget(
                kTransportHoverMotionBase +
                    static_cast<MotionKey>(index),
                MotionToken::hover,
                mode,
                static_cast<TransportControl>(index) == hoverControl_
                    ? 1.0 : 0.0);
        }
        syncClock();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void setPressInside(bool inside)
    {
        if (pressedControl_ == TransportControl::none ||
            pressedInside_ == inside) {
            return;
        }
        pressedInside_ = inside;
        retargetPress(inside
                ? pressedControl_
                : TransportControl::none);
    }

    void retargetPress(TransportControl active)
    {
        const auto mode = motionMode();
        for (int index = 0; index < kTransportControlCount; ++index) {
            motion_.retarget(
                kTransportPressMotionBase +
                    static_cast<MotionKey>(index),
                MotionToken::press,
                mode,
                static_cast<TransportControl>(index) == active
                    ? 1.0 : 0.0);
        }
        syncClock();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void showPlaybackOrderMenu(LPARAM lParam)
    {
        const auto [x, y] = point(lParam);
        const auto size = clientSizeDip(window_);
        const auto layout = transportLayout(size.width, size.height);
        if (pointIn(layout.keepPlaying, x, y)) {
            const auto menu = CreatePopupMenu();
            if (menu == nullptr) return;
            const auto current = KeepPlayingService::instance().mode();
            AppendMenuW(menu,
                MF_STRING | (current == KeepPlayingMode::randomAlbums
                    ? MF_CHECKED : MF_UNCHECKED),
                1U,
                L"Random Albums");
            AppendMenuW(menu,
                MF_STRING | (current == KeepPlayingMode::sequentialAlbums
                    ? MF_CHECKED : MF_UNCHECKED),
                2U,
                L"Sequence");
            POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(window_, &screenPoint);
            const auto command = TrackPopupMenuEx(
                menu,
                TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                screenPoint.x,
                screenPoint.y,
                window_,
                nullptr);
            DestroyMenu(menu);
            if (command == 1U) {
                KeepPlayingService::instance().setMode(
                    KeepPlayingMode::randomAlbums);
            } else if (command == 2U) {
                KeepPlayingService::instance().setMode(
                    KeepPlayingMode::sequentialAlbums);
            }
            return;
        }
        if (!pointIn(layout.repeat, x, y) &&
            !pointIn(layout.shuffle, x, y)) {
            return;
        }

        auto playlists = playlist_manager::get();
        const auto count = playlists->playback_order_get_count();
        const auto active = playlists->playback_order_get_active();
        const auto menu = CreatePopupMenu();
        if (menu == nullptr) return;
        for (t_size index = 0; index < count; ++index) {
            const auto name = utf8ToWide(
                playlists->playback_order_get_name(index));
            const auto flags = MF_STRING |
                (index == active ? MF_CHECKED : MF_UNCHECKED);
            AppendMenuW(
                menu,
                flags,
                static_cast<UINT_PTR>(index + 1U),
                name.c_str());
        }

        POINT screenPoint{
            GET_X_LPARAM(lParam),
            GET_Y_LPARAM(lParam),
        };
        ClientToScreen(window_, &screenPoint);
        const auto command = TrackPopupMenuEx(
            menu,
            TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
            screenPoint.x,
            screenPoint.y,
            window_,
            nullptr);
        DestroyMenu(menu);
        if (command > 0 &&
            static_cast<t_size>(command - 1U) < count) {
            playlists->playback_order_set_active(
                static_cast<t_size>(command - 1U));
            notifyNativePanelsPlaybackChanged();
        }
    }

    std::pair<float, float> point(LPARAM lParam) const
    {
        return clientPointDip(window_, lParam);
    }

    static double normalizedAt(const D2D1_RECT_F& rectangle, float x)
    {
        return std::clamp(
            static_cast<double>((x - rectangle.left) / (rectangle.right - rectangle.left)),
            0.0,
            1.0);
    }

    void syncClock()
    {
        if (!canPresent(window_)) {
            frameClock_.stop();
            return;
        }
        const auto themeActive = ThemeService::instance().transitionActive();
        const auto semanticMotion = motion_.needsFrame() || themeActive;
        frameClock_.setMaximumRefreshHz(motion_.needsFrame() ? 0.0 : themeActive ? kThemeSampleHz : 10.0);
        const bool shouldRun =
            Runtime::instance().playbackSnapshot().state ==
                PlaybackState::playing || semanticMotion;
        if (shouldRun) frameClock_.start();
        else frameClock_.stop();
    }

    static constexpr std::int64_t kUnpresentedStep =
        (std::numeric_limits<std::int64_t>::min)();

    std::int64_t elapsedPresentationStep(double position) const noexcept
    {
        if (position < 0.0 || window_ == nullptr) return kUnpresentedStep;
        // The unified WaveSeek surface owns display-rate cursor animation.
        // Transport only needs to update its textual clock at 10 Hz.
        return static_cast<std::int64_t>(std::floor(position * 10.0));
    }

    bool shouldPresentPlaybackFrame()
    {
        if (motion_.needsFrame() || ThemeService::instance().transitionActive()) {
            return true;
        }
        const auto playback = Runtime::instance().playbackSnapshot();
        if (playback.state != PlaybackState::playing) return false;
        const auto position = Runtime::instance().presentationPlaybackPosition();
        const auto step = elapsedPresentationStep(position);
        return seekPresentation_.shouldPresent(
            step,
            presentationQpcSeconds(),
            frameClock_.targetRefreshMilliHz());
    }

    HWND window_ = nullptr;
    PanelD2D render_;
    WindowFrameClock frameClock_;
    MotionCoordinator motion_;
    NativeTooltipHost tooltips_;
    titleformat_object::ptr playbackContextScript_;
    std::wstring playbackContext_;
    std::wstring playbackCodec_;
    std::wstring playbackFallbackBitrate_;
    std::wstring playbackFormatTail_;
    std::filesystem::path playbackCostPath_;
    double playbackCostDuration_ = 0.0;
    std::chrono::steady_clock::time_point packetCostUpdated_{};
    std::uint64_t playbackContextGeneration_ =
        (std::numeric_limits<std::uint64_t>::max)();
    DragMode drag_ = DragMode::none;
    TransportControl hoverControl_ = TransportControl::none;
    TransportControl pressedControl_ = TransportControl::none;
    bool trackingMouse_ = false;
    bool pressedInside_ = false;
    nekorox::PlaybackPresentationGate seekPresentation_;
    std::uint64_t playbackPaints_ = 0;
    std::uint64_t observedSkippedDeadlines_ = 0;
};

class TrackCaptionPanel : public uie::container_uie_window_v3 {
public:
    const GUID& get_extension_guid() const override { return kCaptionPanelGuid; }
    void get_name(pfc::string_base& output) const override
    {
        output = "GODENplayer Track Caption";
    }
    void get_category(pfc::string_base& output) const override
    {
        output = "Panels";
    }
    unsigned get_type() const override { return uie::type_panel; }

private:
    uie::container_window_v3_config get_window_config() override
    {
        return {L"NekoRoXTrackCaptionV3", false};
    }

    LRESULT on_message(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam) override
    {
        switch (message) {
        case WM_CREATE:
            window_ = window;
            frameClock_.attach(window_, kCaptionFrameMessage);
            frameClock_.setTelemetrySurface(TelemetrySurface::artwork);
            registerNativeWindow(window_);
            refresh(false);
            return 0;
        case WM_DESTROY:
            unregisterNativeWindow(window_);
            frameClock_.detach();
            window_ = nullptr;
            render_.reset();
            return 0;
        case WM_ERASEBKGND:
            return TRUE;
        case WM_SHOWWINDOW:
            syncClock();
            break;
        case WM_SIZE:
            render_.resize();
            syncClock();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO:
            applyFixedBandLimits(
                reinterpret_cast<MINMAXINFO*>(lParam),
                window,
                UiFixedBand::trackCaption);
            return 0;
        case WM_DPICHANGED:
            notifyFixedBandDpiChanged(get_host(), window);
            render_.reset();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_PAINT:
            paint(window);
            return 0;
        case kNativeRefreshMessage:
            if (const auto dirty = consumeNativeInvalidation(window);
                any(dirty & (UiDirty::semantic | UiDirty::artwork))) {
                refresh(true);
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case kThemeTransitionMessage:
            syncClock();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case kCaptionFrameMessage:
            if (frameClock_.consume(wParam)) {
                InvalidateRect(window, nullptr, FALSE);
                syncClock();
            }
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void refresh(bool animate)
    {
        transition_.setText(currentTrackCaption(), animate);
        syncClock();
    }

    void paint(HWND window)
    {
        PAINTSTRUCT paintStruct{};
        BeginPaint(window, &paintStruct);
        if (!render_.begin(window, paintStruct.hdc)) {
            EndPaint(window, &paintStruct);
            return;
        }

        const auto size = render_.sizeDip();
        const auto bounds = D2D1::RectF(
            8.0f,
            0.0f,
            std::max(8.0f, size.width - 8.0f),
            size.height);
        const auto mode = motionMode();
        const auto reconstructing = transition_.active(mode);
        if (!reconstructing) {
            render_.text(transition_.frame(mode), bounds,
                PanelText::center, PanelColor::text);
        } else {
            const auto progress = static_cast<float>(
                transition_.animationProgress(mode));
            if (mode == MotionMode::reduced) {
                render_.text(transition_.sourceText(), bounds,
                    PanelText::center, PanelColor::secondary,
                    1.0f - progress);
                render_.text(transition_.text(), bounds,
                    PanelText::center, PanelColor::text, progress);
            } else {
                const auto secondary = uiColorFromD2D(
                    render_.brush(PanelColor::secondary)->GetColor());
                const auto accent = uiColorFromD2D(
                    render_.brush(PanelColor::accent)->GetColor());
                const auto text = uiColorFromD2D(
                    render_.brush(PanelColor::text)->GetColor());
                const auto transitionColor = progress < 0.45f
                    ? interpolateLinearColor(secondary, accent,
                        progress / 0.45f)
                    : interpolateLinearColor(accent, text,
                        (progress - 0.45f) / 0.55f);
                render_.text(transition_.sourceText(), bounds,
                    PanelText::center, PanelColor::secondary,
                    std::clamp(1.0f - progress * 2.8f, 0.0f, 1.0f));
                render_.text(transition_.frame(mode), bounds,
                    PanelText::center, transitionColor,
                    std::clamp(progress * 2.5f, 0.0f, 1.0f));
            }
        }
        render_.end();
        EndPaint(window, &paintStruct);
        syncClock();
    }

    void syncClock()
    {
        if (canPresent(window_) &&
            (transition_.active(motionMode()) ||
                ThemeService::instance().transitionActive())) {
            frameClock_.start();
        } else {
            frameClock_.stop();
        }
    }

    HWND window_ = nullptr;
    PanelD2D render_;
    WindowFrameClock frameClock_;
    TextReconstructionTransition transition_;
};

class CoverPanel : public uie::container_uie_window_v3 {
public:
    CoverPanel() : artCache_(kNativeArtworkReadyMessage) {}

    const GUID& get_extension_guid() const override { return kCoverPanelGuid; }
    void get_name(pfc::string_base& output) const override { output = "GODENplayer Cover"; }
    void get_category(pfc::string_base& output) const override { output = "Panels"; }
    unsigned get_type() const override { return uie::type_panel; }

private:
    uie::container_window_v3_config get_window_config() override
    {
        return {L"NekoRoXCoverV3", false};
    }

    LRESULT on_message(HWND window, UINT message, WPARAM wParam, LPARAM lParam) override
    {
        if (message == playerShellModeMessage()) {
            const auto visible = wParam ==
                static_cast<WPARAM>(PlayerShellMode::normal);
            if (get_host().is_valid()) {
                (void)get_host()->set_window_visibility(window, visible);
            } else {
                ShowWindow(window, visible ? SW_SHOWNA : SW_HIDE);
            }
            return TRUE;
        }
        if (message == coverStateMessage()) {
            const auto& transition = artworkTransition_.diagnostics();
            const auto broker = artworkBrokerDiagnostics();
            switch (wParam) {
            case 1:
                return static_cast<LRESULT>(transition.presentedFrames);
            case 2:
                return static_cast<LRESULT>(transition.reconstructionFrames);
            case 3:
                return static_cast<LRESULT>(transition.fadeFallbackFrames);
            case 4:
                return static_cast<LRESULT>(std::llround(
                    transition.maximumFrameMilliseconds * 1000.0));
            case 5:
                return static_cast<LRESULT>(transition.generation);
            case 6:
                return static_cast<LRESULT>(pixelWidth_);
            case 7:
                return static_cast<LRESULT>(pixelHeight_);
            case 8:
                return static_cast<LRESULT>(targetPhysicalEdge_);
            case 20:
                return static_cast<LRESULT>(broker.loadRequests);
            case 21:
                return static_cast<LRESULT>(broker.localPaths);
            case 22:
                return static_cast<LRESULT>(broker.sidecarCandidates);
            case 23:
                return static_cast<LRESULT>(broker.fileReads);
            case 24:
                return static_cast<LRESULT>(broker.decodeAttempts);
            case 25:
                return static_cast<LRESULT>(broker.decodeSuccesses);
            case 26:
                return static_cast<LRESULT>(broker.readyBytes);
            case 27:
                return static_cast<LRESULT>(broker.peakTemporaryBytes);
            case 28:
                return static_cast<LRESULT>(broker.busyRetryRequeues);
            case 29:
                return static_cast<LRESULT>(broker.failedNotices);
            case 30:
                return static_cast<LRESULT>(broker.resampleAttempts);
            case 31:
                return static_cast<LRESULT>(broker.resampleMemoHits);
            default:
                break;
            }
            std::uint32_t state = 0;
            if (track_.is_valid()) state |= nekorox::kCoverTrackValidBitV1;
            if (pixelsReady_) state |= nekorox::kCoverPixelsReadyBitV1;
            if (artworkTransition_.ready()) state |= nekorox::kCoverBitmapReadyBitV1;
            if (FAILED(bitmapStatus_)) state |= nekorox::kCoverBitmapFailedBitV1;
            if (!key_.empty()) state |= nekorox::kCoverKeyValidBitV1;
            if (paintObserved_) state |= nekorox::kCoverPaintObservedBitV1;
            if (renderReady_) state |= nekorox::kCoverRenderReadyBitV1;
            if (artworkTransition_.active(motionMode())) {
                state |= nekorox::kCoverTransitionActiveBitV1;
            }
            if (transition.reconstructionFrames != 0) {
                state |= nekorox::kCoverReconstructionUsedBitV1;
            }
            if (transition.fadeFallbackFrames != 0) {
                state |= nekorox::kCoverFadeFallbackUsedBitV1;
            }
            if (transition.reconstructionPrepared) {
                state |= nekorox::kCoverReconstructionPreparedBitV1;
            }
            const auto cacheStatus = artCache_.status(
                key_, targetPhysicalEdge_ != 0 ? targetPhysicalEdge_ : 512U);
            if (cacheStatus == ArtCacheStatus::loading) {
                state |= nekorox::kCoverCacheLoadingBitV1;
            } else if (cacheStatus == ArtCacheStatus::missing) {
                state |= nekorox::kCoverCacheMissingBitV1;
            } else if (cacheStatus == ArtCacheStatus::ready) {
                state |= nekorox::kCoverCacheReadyBitV1;
            }
            return static_cast<LRESULT>(state);
        }
        switch (message) {
        case WM_CREATE:
            window_ = window;
            frameClock_.attach(window_, kGlyphFlowFrameMessage);
            frameClock_.setTelemetrySurface(TelemetrySurface::artwork);
            GlyphFlowService::instance().subscribe(
                window_, kGlyphFlowChangedMessage);
            artCache_.attach(window_);
            registerNativeWindow(window_);
            refreshTrack();
            return 0;
        case WM_DESTROY:
            unregisterNativeWindow(window_);
            GlyphFlowService::instance().unsubscribe(window_);
            frameClock_.detach();
            artCache_.detach();
            window_ = nullptr;
            artworkTransition_.clear();
            glyphFlowRenderer_.reset();
            render_.reset();
            return 0;
        case WM_ERASEBKGND:
            return TRUE;
        case WM_SHOWWINDOW:
            syncGlyphFlowClock();
            break;
        case WM_SIZE:
            render_.resize();
            syncGlyphFlowClock();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED:
            artworkTransition_.discardDeviceResources();
            glyphFlowRenderer_.reset();
            render_.reset();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            const auto [x, y] = clientPointDip(window, lParam);
            coverPressed_ = pointIn(artworkRect_, x, y);
            if (coverPressed_) SetCapture(window);
            return 0;
        }
        case WM_LBUTTONUP: {
            const auto [x, y] = clientPointDip(window, lParam);
            const auto activate = coverPressed_ &&
                pointIn(artworkRect_, x, y);
            coverPressed_ = false;
            if (GetCapture() == window) ReleaseCapture();
            if (activate) (void)revealNowPlayingInWorkspace();
            return 0;
        }
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
            coverPressed_ = false;
            return 0;
        case WM_PAINT:
            paint(window);
            return 0;
        case kNativeRefreshMessage:
            if (const auto dirty = consumeNativeInvalidation(window);
                any(dirty & (UiDirty::semantic | UiDirty::artwork))) {
                refreshTrack(any(dirty & UiDirty::artwork));
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case kNativeArtworkReadyMessage:
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case kGlyphFlowChangedMessage:
            syncGlyphFlowClock();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case kThemeTransitionMessage:
            syncGlyphFlowClock();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case kGlyphFlowFrameMessage:
            if (frameClock_.consume(wParam)) {
                InvalidateRect(window, nullptr, FALSE);
                syncGlyphFlowClock();
            }
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void refreshTrack(bool artworkChanged = false)
    {
        const auto next = Runtime::instance().currentTrackHandle();
        if (next == track_ && !artworkChanged) return;
        track_ = next;
        key_ = artworkTrackIdentity(track_);
        pixelsReady_ = false;
        bitmapStatus_ = S_FALSE;
        if (track_.is_valid()) {
            artCache_.getOrRequest(key_, track_, 512,
                ArtRequestPriority::visibleMain);
        }
    }

    void paint(HWND window)
    {
        paintObserved_ = true;
        PAINTSTRUCT paintStruct{};
        BeginPaint(window, &paintStruct);
        if (!render_.begin(window, paintStruct.hdc)) {
            renderReady_ = false;
            EndPaint(window, &paintStruct);
            return;
        }
        renderReady_ = true;
        const auto size = render_.sizeDip();
        constexpr float kArtworkPaddingDip = 5.0f;
        // The Cover panel no longer owns a caption. Reserving the old 22-DIP
        // lane made the square visibly smaller and vertically asymmetric,
        // especially in the minimum-size layout.
        const auto artworkHeight = std::max(1.0f, size.height);
        const auto edge = std::max(
            1.0f,
            std::min(size.width - kArtworkPaddingDip * 2.0f,
                artworkHeight - kArtworkPaddingDip * 2.0f));
        const auto rectangle = D2D1::RectF(
            (size.width - edge) * 0.5f,
            (artworkHeight - edge) * 0.5f,
            (size.width + edge) * 0.5f,
            (artworkHeight + edge) * 0.5f);
        artworkRect_ = rectangle;
        const auto dpi = static_cast<float>(GetDpiForWindow(window));
        const auto targetPhysicalEdge = static_cast<std::uint32_t>(std::max(
            1.0,
            std::ceil(static_cast<double>(edge) * dpi / 96.0)));
        targetPhysicalEdge_ = targetPhysicalEdge;
        const auto pixels = track_.is_valid()
            ? artCache_.getOrRequestVariant(
                key_, track_, targetPhysicalEdge, true,
                ArtRequestPriority::visibleMain)
            : std::shared_ptr<const ArtPixels>{};
        pixelsReady_ = pixels && !pixels->bgra.empty();
        pixelWidth_ = pixels ? pixels->width : 0;
        pixelHeight_ = pixels ? pixels->height : 0;
        if (!pixels && track_.is_valid() &&
            artCache_.status(key_, targetPhysicalEdge) ==
                ArtCacheStatus::missing) {
            artworkTransition_.clear();
        }
        auto* target = render_.target();
        if (pixels) artworkTransition_.update(target, dpi, key_, pixels);
        const auto artworkDrawn = artworkTransition_.draw(
            target, rectangle, motionMode());
        bitmapStatus_ = artworkDrawn ? S_OK : pixelsReady_ ? E_FAIL : S_FALSE;
        if (!artworkDrawn) {
            const auto logoEdge = std::min(42.0f, edge * 0.32f);
            const auto centerX = (rectangle.left + rectangle.right) * 0.5f;
            const auto centerY = (rectangle.top + rectangle.bottom) * 0.5f;
            render_.nekoLogo(D2D1::RectF(
                centerX - logoEdge * 0.5f,
                centerY - logoEdge * 0.5f,
                centerX + logoEdge * 0.5f,
                centerY + logoEdge * 0.5f));
        }
        const auto theme = ThemeService::instance().snapshot();
        GlyphFlowDrawStyle glyphStyle;
        glyphStyle.color = D2D1::ColorF(
            theme.accent.red,
            theme.accent.green,
            theme.accent.blue,
            1.0f);
        glyphStyle.strength = 0.12f;
        glyphFlowRenderer_.draw(
            target, rectangle, motionMode(), glyphStyle);
        render_.line(rectangle.left, rectangle.top,
            rectangle.right, rectangle.top, PanelColor::border);
        render_.line(rectangle.left, rectangle.bottom,
            rectangle.right, rectangle.bottom, PanelColor::border);
        render_.line(rectangle.left, rectangle.top,
            rectangle.left, rectangle.bottom, PanelColor::border);
        render_.line(rectangle.right, rectangle.top,
            rectangle.right, rectangle.bottom, PanelColor::border);
        const auto artworkTicket = artworkTransition_.pendingPresentation();
        render_.end();
        const bool accepted = render_.lastFrameResult() == FrameResult::accepted;
        artworkTransition_.presented(artworkTicket, accepted);
        glyphFlowRenderer_.presented(accepted);
        EndPaint(window, &paintStruct);
        syncGlyphFlowClock();
    }

    void syncGlyphFlowClock()
    {
        if (canPresent(window_) &&
            (GlyphFlowService::instance().active(motionMode()) ||
                artworkTransition_.active(motionMode()) ||
                ThemeService::instance().transitionActive())) {
            frameClock_.start();
        } else {
            frameClock_.stop();
        }
    }

    HWND window_ = nullptr;
    PanelD2D render_;
    WindowFrameClock frameClock_;
    GlyphFlowRenderer glyphFlowRenderer_;
    ArtworkTransitionRenderer artworkTransition_;
    ArtCache artCache_;
    metadb_handle_ptr track_;
    std::wstring key_;
    bool pixelsReady_ = false;
    bool paintObserved_ = false;
    bool renderReady_ = false;
    HRESULT bitmapStatus_ = S_FALSE;
    std::uint32_t pixelWidth_ = 0;
    std::uint32_t pixelHeight_ = 0;
    std::uint32_t targetPhysicalEdge_ = 0;
    D2D1_RECT_F artworkRect_{};
    bool coverPressed_ = false;
};

uie::window_factory<HeaderPanel> g_headerPanelFactory;
uie::window_factory<MonitorPanel> g_monitorPanelFactory;
uie::window_factory<TransportPanel> g_transportPanelFactory;
uie::window_factory<TrackCaptionPanel> g_trackCaptionPanelFactory;
uie::window_factory<CoverPanel> g_coverPanelFactory;

} // namespace

void notifyNativePanelsPlaybackChanged(bool artworkChanged)
{
    UiInvalidationBus::instance().publish(
        UiInvalidationSurface::nativePanels,
        artworkChanged
            ? UiDirty::semantic | UiDirty::artwork
            : UiDirty::semantic);
    notifyWaveSeekPlaybackChanged();
}

} // namespace nekorox::component
