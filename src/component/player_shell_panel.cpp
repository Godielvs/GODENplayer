#include "player_shell_panel.h"
#include "accessible_surface.h"
#include "telemetry_span.h"
#include "c9_qa.h"
#include "realtime_analysis_hub.h"
#include <array>
#include "curated/curated_preferences.h"
#include "a4/a4_preferences.h"
#include "a4/a4_trace.h"

#include "album_session_service.h"
#include "album_ambient_controller.h"
#include "album_booklet_model.h"
#include "album_mode_renderer.h"
#include "collection/visual_picker.h"
#include "album_mode_view.h"
#include "art_cache.h"
#include "database_service.h"
#include "display_environment.h"
#include "frame_clock.h"
#include "interaction_latency_guard.h"
#include "library_model.h"
#include "listen_window.h"
#include "collection/c6_presentation_policy.h"
#include "local_content_cache.h"
#include "lyrics_service.h"
#include "note_editor.h"
#include "persona_editor.h"
#include "realtime_analysis_hub.h"
#include "runtime.h"
#include "rollout_flags.h"
#include "text_util.h"
#include "theme_service.h"
#include "ui_design_system.h"
#include "ui_invalidation_bus.h"
#include "ui_preferences.h"

#include "nekorox/scroll_controller.h"

#include <columns_ui-sdk/ui_extension.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <utility>
#include <vector>
#include <windowsx.h>

namespace nekorox::component {
namespace {

const GUID kPlayerShellPanelGuid = {
    0xa3dc5b91, 0x875e, 0x4f0a,
    {0xa7, 0x43, 0x7d, 0x31, 0x83, 0xe6, 0xf4, 0x2c}};
const GUID kWorkspacePanelGuid = {
    0x6c52b18d, 0x4664, 0x4e8f,
    {0x8d, 0xcd, 0xce, 0x2b, 0x82, 0xb7, 0x6b, 0x32}};
const GUID kTransportPanelGuid = {0x37dfaaa1, 0x5040, 0x4966,
    {0x81, 0x9f, 0xd0, 0x12, 0xf4, 0x1a, 0x60, 0x78}};
const GUID kWaveSeekPanelGuid = {0x369b09d4, 0x58a7, 0x4730,
    {0xa2, 0xce, 0xdd, 0xd8, 0x24, 0xe8, 0x32, 0x13}};
const GUID kSpectrumPanelGuid = {0x3247c894, 0xe585, 0x4025,
    {0xa8, 0x66, 0xc7, 0xd4, 0x93, 0x3f, 0xb2, 0xe3}};
const GUID kCoverPanelGuid = {0x5d0cc69e, 0xd3d0, 0x4c82,
    {0x90, 0xdb, 0x40, 0xc7, 0xcf, 0x9c, 0x3c, 0x9e}};
const GUID kCaptionPanelGuid = {0x6d9a108b, 0xcc44, 0x44ba,
    {0x88, 0x54, 0x5f, 0x7b, 0xd0, 0xcd, 0x33, 0x10}};
const GUID kPlayerShellHostGuid = {
    0xe4f47a74, 0x8440, 0x4d1f,
    {0xb3, 0xf0, 0x9f, 0xc7, 0x78, 0x89, 0x6d, 0x3e}};

constexpr std::uint32_t kConfigVersion = 3U;
constexpr std::uint32_t kMaximumChildConfigBytes = 16U * 1024U * 1024U;
constexpr UINT_PTR kAlbumLyricsWakeTimer = 0x4e52U;
constexpr UINT kThemeTransitionMessage = WM_APP + 0x4ea;
constexpr UINT kShellRefreshMessage = WM_APP + 0x4eb;
constexpr std::uint32_t kAmbientDeferDeckResize = 1U << 0U;
constexpr std::uint32_t kAmbientDeferLyricsPress = 1U << 1U;
constexpr std::uint32_t kAmbientDeferTrackScroll = 1U << 2U;
constexpr std::uint32_t kAmbientDeferExplicitInput = 1U << 3U;
constexpr std::uint32_t kAmbientDeferPendingInput = 1U << 4U;
constexpr std::uint32_t kAmbientSentMessageAdvisory = 1U << 5U;
std::atomic<PlayerShellMode> g_playerShellMode{PlayerShellMode::normal};
std::atomic<std::uint64_t> g_shellEpoch{0};

double albumScrollClockSeconds() noexcept
{
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

UINT albumArtworkReadyMessage() noexcept
{
    static const auto value = RegisterWindowMessageW(
        L"GodenAlbumArtworkReadyV1");
    return value;
}

UINT albumContentReadyMessage() noexcept
{
    static const auto value = RegisterWindowMessageW(
        L"GodenAlbumContentReadyV1");
    return value;
}

UINT albumFrameMessage() noexcept
{
    static const auto value = RegisterWindowMessageW(L"GodenAlbumFrameV2");
    return value;
}

UINT albumAmbientPumpMessage() noexcept
{
    static const auto value = RegisterWindowMessageW(
        L"GodenAlbumAmbientPumpV1");
    return value;
}

UINT rootPresentationVisibilityMessage() noexcept
{
    static const auto value = RegisterWindowMessageW(
        L"Goden.RootPresentationVisibility.V1");
    return value;
}

UINT shellSurfaceRecoveryMessage() noexcept
{
    static const auto value = RegisterWindowMessageW(
        L"NekoRoX.ShellSurfaceRecovery.V1");
    return value;
}

std::uint64_t albumQpcNow() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart > 0
        ? static_cast<std::uint64_t>(value.QuadPart) : 0;
}

bool lyricsSurfaceAvailable(
    const std::shared_ptr<const LyricsSnapshot>& lyrics) noexcept
{
    if (!lyrics) return false;
    if (lyrics->document && (!lyrics->document->plainTextUtf8.empty() ||
            !lyrics->document->cues.empty())) return true;
    return lyrics->state == LyricsState::loadingLocal ||
        lyrics->state == LyricsState::searching;
}

UINT playerShellQaMessage() noexcept
{
    static const auto value = RegisterWindowMessageW(L"GodenPlayerShellQaV1");
    return value;
}

class PlayerShellPanel;

class ShellChildHost : public uie::window_host {
public:
    explicit ShellChildHost(PlayerShellPanel* owner) noexcept : owner_(owner) {}

    const GUID& get_host_guid() const override { return kPlayerShellHostGuid; }
    void on_size_limit_change(HWND, unsigned) override;
    unsigned is_resize_supported(HWND) const override { return 0U; }
    bool request_resize(HWND, unsigned, unsigned, unsigned) override
    {
        return false;
    }
    bool override_status_text_create(
        service_ptr_t<ui_status_text_override>& output) override
    {
        return ui_control::get()->override_status_text_create(output);
    }
    bool get_keyboard_shortcuts_enabled() const override;
    bool is_visible(HWND window) const override
    {
        return window != nullptr && IsWindowVisible(window) != FALSE;
    }
    bool is_visibility_modifiable(HWND, bool) const override { return true; }
    bool set_window_visibility(HWND window, bool visible) override
    {
        if (window == nullptr) return false;
        ShowWindow(window, visible ? SW_SHOWNA : SW_HIDE);
        return true;
    }
    void relinquish_ownership(HWND) override {}

private:
    PlayerShellPanel* owner_ = nullptr;
};

class PlayerShellPanel : public uie::container_uie_window_v3 {
    class AlbumKeyboardFilter final : public message_filter_impl_base {
    public:
        explicit AlbumKeyboardFilter(PlayerShellPanel& owner):owner_(owner) {}
        bool pretranslate_message(MSG* message) override {
            if (message) {
                switch (message->message) {
                case WM_KEYDOWN: case WM_SYSKEYDOWN:
                case WM_LBUTTONDOWN: case WM_LBUTTONUP:
                case WM_RBUTTONDOWN: case WM_RBUTTONUP:
                case WM_MBUTTONDOWN: case WM_MBUTTONUP:
                case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
                    InteractionLatencyGuard::instance().noteScroll();
                    break;
                default: break;
                }
            }
            if(message && message->message==WM_KEYDOWN) ++owner_.albumKeyboardMessages_;
            if(!message || message->message!=WM_KEYDOWN ||
                GetFocus()!=owner_.window_ ||
                owner_.mode_!=PlayerShellMode::album ||
                (GetKeyState(VK_CONTROL)&0x8000) || (GetKeyState(VK_MENU)&0x8000)) return false;
            return owner_.albumKey(static_cast<UINT>(message->wParam));
        }
    private:PlayerShellPanel& owner_;
    };
public:
    PlayerShellPanel()
        : artCache_(albumArtworkReadyMessage())
        , contentCache_(albumContentReadyMessage()) {}

    const GUID& get_extension_guid() const override
    {
        return kPlayerShellPanelGuid;
    }
    void get_name(pfc::string_base& output) const override
    {
        output = "GODENplayer Shell";
    }
    void get_category(pfc::string_base& output) const override
    {
        output = "Panels";
    }
    unsigned get_type() const override { return uie::type_panel; }

    void set_config(
        stream_reader* reader,
        t_size size,
        abort_callback& abort) override
    {
        mode_ = PlayerShellMode::normal;
        if (reader == nullptr || size < sizeof(std::uint32_t) * 2U) return;
        std::uint32_t version = 0;
        std::uint32_t mode = 0;
        reader->read_lendian_t(version, abort);
        reader->read_lendian_t(mode, abort);
        if (version == 1U && mode <= 1U) {
            mode_ = static_cast<PlayerShellMode>(mode);
            g_playerShellMode.store(mode_, std::memory_order_release);
            const auto remaining = size - sizeof(std::uint32_t) * 2U;
            if (remaining <= 16U * 1024U * 1024U) {
                workspaceConfig_.resize(remaining);
                if (remaining != 0U) {
                    reader->read_object(
                        workspaceConfig_.data(), remaining, abort);
                }
            }
        } else if ((version == 2U || version == kConfigVersion) && mode <= 1U &&
            size >= sizeof(std::uint32_t) * (version == 2U ? 3U : 4U)) {
            if (version == kConfigVersion) {
                std::int32_t spectrumHeight = 0;
                reader->read_lendian_t(spectrumHeight, abort);
                spectrumHeightDip_ = std::clamp(
                    static_cast<int>(spectrumHeight), 96, 360);
            }
            std::uint32_t count = 0;
            reader->read_lendian_t(count, abort);
            auto remaining = size - sizeof(std::uint32_t) *
                (version == 2U ? 3U : 4U);
            if (count != 6U) return;
            std::array<bool, 6> seen{};
            std::array<std::vector<std::uint8_t>, 6> parsed{};
            for (std::uint32_t index = 0; index < count; ++index) {
                if (remaining < sizeof(GUID) + sizeof(std::uint32_t)) return;
                GUID guid{};
                std::uint32_t bytes = 0;
                reader->read_object(&guid, sizeof(guid), abort);
                reader->read_lendian_t(bytes, abort);
                remaining -= sizeof(guid) + sizeof(bytes);
                if (bytes > remaining || bytes > kMaximumChildConfigBytes) return;
                const auto targetIndex = knownGuidIndex(guid);
                if (targetIndex < 0 || seen[static_cast<std::size_t>(targetIndex)]) {
                    return;
                }
                seen[static_cast<std::size_t>(targetIndex)] = true;
                std::vector<std::uint8_t> value(bytes);
                if (bytes != 0) reader->read_object(value.data(), bytes, abort);
                remaining -= bytes;
                parsed[static_cast<std::size_t>(targetIndex)] = std::move(value);
            }
            if (remaining != 0U) return;
            workspaceConfig_ = std::move(parsed[0]);
            transportConfig_ = std::move(parsed[1]);
            waveSeekConfig_ = std::move(parsed[2]);
            spectrumConfig_ = std::move(parsed[3]);
            captionConfig_ = std::move(parsed[4]);
            coverConfig_ = std::move(parsed[5]);
            mode_ = static_cast<PlayerShellMode>(mode);
            g_playerShellMode.store(mode_, std::memory_order_release);
        }
    }

    void get_config(stream_writer* writer, abort_callback& abort) const override
    {
        writer->write_lendian_t(kConfigVersion, abort);
        writer->write_lendian_t(static_cast<std::uint32_t>(mode_), abort);
        writer->write_lendian_t(static_cast<std::int32_t>(spectrumHeightDip_),
            abort);
        constexpr std::array<const GUID*, 6> guids{
            &kWorkspacePanelGuid, &kTransportPanelGuid, &kWaveSeekPanelGuid,
            &kSpectrumPanelGuid, &kCaptionPanelGuid, &kCoverPanelGuid};
        writer->write_lendian_t(static_cast<std::uint32_t>(guids.size()), abort);
        for (const auto* guid : guids) {
            writer->write_object(guid, sizeof(*guid), abort);
            const auto& child = childForGuid(*guid);
            pfc::array_t<std::uint8_t> current;
            const auto* stored = &configForGuid(*guid);
            if (child != nullptr && child->is_valid()) {
                (*child)->get_config_to_array(current, abort);
            }
            const auto bytes = current.get_size() != 0U
                ? static_cast<std::uint32_t>(current.get_size())
                : static_cast<std::uint32_t>(stored->size());
            writer->write_lendian_t(bytes, abort);
            if (current.get_size() != 0U) {
                writer->write_object(current.get_ptr(), current.get_size(), abort);
            } else if (!stored->empty()) {
                writer->write_object(stored->data(), stored->size(), abort);
            }
        }
    }

    void childLimitsChanged() noexcept
    {
        if (window_ != nullptr && get_host().is_valid()) {
            get_host()->on_size_limit_change(window_, uie::size_limit_all);
        }
    }

    bool keyboardShortcutsEnabled() const noexcept
    {
        return get_host().is_valid() &&
            get_host()->get_keyboard_shortcuts_enabled();
    }

private:
    [[nodiscard]] static int knownGuidIndex(const GUID& guid) noexcept
    {
        constexpr std::array<const GUID*, 6> values{
            &kWorkspacePanelGuid, &kTransportPanelGuid, &kWaveSeekPanelGuid,
            &kSpectrumPanelGuid, &kCaptionPanelGuid, &kCoverPanelGuid};
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (InlineIsEqualGUID(guid, *values[index])) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    [[nodiscard]] std::vector<std::uint8_t>* mutableConfigForGuid(
        const GUID& guid) noexcept
    {
        if (InlineIsEqualGUID(guid, kWorkspacePanelGuid)) return &workspaceConfig_;
        if (InlineIsEqualGUID(guid, kTransportPanelGuid)) return &transportConfig_;
        if (InlineIsEqualGUID(guid, kWaveSeekPanelGuid)) return &waveSeekConfig_;
        if (InlineIsEqualGUID(guid, kSpectrumPanelGuid)) return &spectrumConfig_;
        if (InlineIsEqualGUID(guid, kCaptionPanelGuid)) return &captionConfig_;
        if (InlineIsEqualGUID(guid, kCoverPanelGuid)) return &coverConfig_;
        return nullptr;
    }

    std::vector<std::uint8_t>& configForGuid(const GUID& guid)
    {
        auto* value = mutableConfigForGuid(guid);
        return value != nullptr ? *value : workspaceConfig_;
    }

    const std::vector<std::uint8_t>& configForGuid(const GUID& guid) const
    {
        return const_cast<PlayerShellPanel*>(this)->configForGuid(guid);
    }

    const uie::window_ptr* childForGuid(const GUID& guid) const noexcept
    {
        if (InlineIsEqualGUID(guid, kWorkspacePanelGuid)) return &workspace_;
        if (InlineIsEqualGUID(guid, kTransportPanelGuid)) return &transport_;
        if (InlineIsEqualGUID(guid, kWaveSeekPanelGuid)) return &waveSeek_;
        if (InlineIsEqualGUID(guid, kSpectrumPanelGuid)) return &spectrum_;
        if (InlineIsEqualGUID(guid, kCaptionPanelGuid)) return &caption_;
        if (InlineIsEqualGUID(guid, kCoverPanelGuid)) return &cover_;
        return nullptr;
    }

    uie::container_window_v3_config get_window_config() override
    {
        return {L"GodenPlayerShellV2", false};
    }

    static LRESULT CALLBACK albumWheelLowLevelHook(
        int code, WPARAM wParam, LPARAM lParam) noexcept
    {
        if (code >= 0 && wParam == WM_MOUSEWHEEL && lParam != 0 &&
                albumWheelHookOwner_ != nullptr) {
            ++albumWheelHookOwner_->c9WheelCallbacks_;
            albumWheelHookOwner_->c9LastWheelTick_=GetTickCount64();
            const auto& mouse =
                *reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
            if (albumWheelHookOwner_->pointerOverAlbumScrollSurfaceAt(
                    mouse.pt)) {
                // Queue one event per original delta. Do not run the handler
                // inside the low-level hook, merge reversals, or steal focus.
                // If posting fails, let the original event continue normally.
                const auto delta = static_cast<short>(
                    HIWORD(mouse.mouseData));
                if(albumWheelHookOwner_->postAlbumWheel(mouse.pt,delta))return 1;
            }
        }
        return CallNextHookEx(albumWheelHook_, code, wParam, lParam);
    }

    std::uint64_t c9WheelCallbacks_=0,c9WheelAccepted_=0,c9WheelPostFailures_=0;
    std::uint64_t c9WheelDelivered_=0,c9WheelCancelled_=0,c9HookFailures_=0;
    ULONGLONG c9LastWheelTick_=0;
    std::array<LRESULT,80> c9QaValues_{};
    std::uint64_t c9QaRevision_=0;
    bool c9QaLatched_=false;

    bool postAlbumWheel(POINT point,short delta) noexcept
    {
        const auto forward=c9WheelForwardMessage();
        if(forward && PostMessageW(window_,forward,
                MAKEWPARAM(c9WheelKeys(),static_cast<WORD>(delta)),
                MAKELPARAM(point.x,point.y))){++c9WheelAccepted_;return true;}
        ++c9WheelPostFailures_;return false;
    }

    void installAlbumWheelHook() noexcept
    {
        (void)c9WheelForwardMessage(); // registration is not done in the hook
        if (albumWheelHookOwner_ == this) return;
        // The product layout owns one GODENplayer shell per UI thread. Refuse a
        // second claimant instead of moving a live hook away from its window.
        if (albumWheelHookOwner_ != nullptr || albumWheelHook_ != nullptr) return;
        albumWheelHookOwner_ = this;
        const auto module = reinterpret_cast<HINSTANCE>(
            GetWindowLongPtrW(window_, GWLP_HINSTANCE));
        albumWheelHook_ = SetWindowsHookExW(WH_MOUSE_LL,
            albumWheelLowLevelHook, module, 0);
        if (albumWheelHook_ == nullptr) {
            ++c9HookFailures_;
            albumWheelHookOwner_ = nullptr;
        }
    }

    void uninstallAlbumWheelHook() noexcept
    {
        if (albumWheelHookOwner_ != this) return;
        albumWheelHookOwner_ = nullptr;
        if (albumWheelHook_ != nullptr) {
            UnhookWindowsHookEx(albumWheelHook_);
            albumWheelHook_ = nullptr;
        }
    }

    [[nodiscard]] bool pointerOverAlbumScrollSurfaceAt(
        POINT screenPoint) const noexcept
    {
        if (mode_ != PlayerShellMode::album || window_ == nullptr ||
            !IsWindowVisible(window_) || deckResizeActive_ || lyricsPress_ ||
            ratingPress_ || booklet_.openDestination() ||
            booklet_.sample(AlbumBookletModel::Clock::now()) > 0.001f) {
            return false;
        }
        const auto root = GetAncestor(window_, GA_ROOT);
        if (root == nullptr || GetForegroundWindow() != root) return false;
        const auto capture = GetCapture();
        if (capture != nullptr && capture != window_ &&
            !IsChild(window_, capture)) return false;
        const auto hit = WindowFromPoint(screenPoint);
        if (hit != window_ && !IsChild(window_, hit)) return false;

        auto clientPoint = screenPoint;
        if (!ScreenToClient(window_, &clientPoint)) return false;
        RECT client{};
        if (!GetClientRect(window_, &client) ||
            !PtInRect(&client, clientPoint)) return false;
        const auto dpi = std::max<UINT>(1U, GetDpiForWindow(window_));
        const auto x = clientPoint.x * 96.0f / dpi;
        const auto y = clientPoint.y * 96.0f / dpi - 36.0f;
        if (y < 0.0f) return false;
        const auto layout = computeAlbumModeLayout(
            (client.right - client.left) * 96.0f / dpi,
            albumContentHeightDip(
                (client.bottom - client.top) * 96.0f / dpi),
            true, compactSelection_, albumPanelVisibility());
        return pointInAlbumRect(layout.tracks, x, y) ||
            pointInAlbumRect(layout.lyrics, x, y);
    }

    LRESULT on_message(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam) override
    {
        if(c9WheelForwardMessage()!=0 && message==c9WheelForwardMessage()){
            if(mode_!=PlayerShellMode::album || !IsWindowEnabled(window_)){
                ++c9WheelCancelled_;return 0;
            }
            ++c9WheelDelivered_;
            message=WM_MOUSEWHEEL; // continue through the unchanged production handler
        }
        if(c9DiagnosticsMessage()!=0 && message==c9DiagnosticsMessage()){
            if(!c9QaAllowed())return E_ACCESSDENIED;
            if(wParam==0U)return 9;
            if(wParam==100U){
                c9QaValues_.fill(0);
                c9QaValues_[0]=9;
                c9QaValues_[1]=static_cast<LRESULT>(motionMode());
                c9QaValues_[2]=static_cast<LRESULT>(listenEffectsMode());
                c9QaValues_[3]=static_cast<LRESULT>(a4::style());
                c9QaValues_[10]=albumWheelHook_!=nullptr;
                c9QaValues_[11]=static_cast<LRESULT>(c9HookFailures_);
                c9QaValues_[12]=static_cast<LRESULT>(c9WheelCallbacks_);
                c9QaValues_[13]=static_cast<LRESULT>(c9WheelAccepted_);
                c9QaValues_[14]=static_cast<LRESULT>(c9WheelPostFailures_);
                c9QaValues_[15]=static_cast<LRESULT>(c9WheelDelivered_);
                c9QaValues_[16]=static_cast<LRESULT>(c9WheelCancelled_);
                c9QaValues_[17]=static_cast<LRESULT>(c9LastWheelTick_);
                c9QaValues_[18]=static_cast<LRESULT>(RealtimeAnalysisHub::instance().activeFeatureMask());
                // C9 Clovis publishes its eight regions on the curated AudioFrame.
                // The legacy ambient controller is idle whenever a curated style
                // is selected, so reading only its pose reported zeros for the
                // exact material C9 adds. Prefer the applied curated lights.
                const auto& curatedLights=albumRenderer_.appliedLights();
                const auto& ambientLights=ambientController_.lightDiagnostics();
                const auto& lights=curatedLights.valid?curatedLights:ambientLights;
                c9QaValues_[20]=static_cast<LRESULT>(lights.master*1000000.f);
                c9QaValues_[21]=lights.valid;
                for(unsigned i=0;i<8;++i){
                    c9QaValues_[22+i]=static_cast<LRESULT>(lights.level[i]*1000000.f);
                    c9QaValues_[30+i]=static_cast<LRESULT>(lights.texture[i]*1000000.f);
                }
                const unsigned collectionFields[]{0,1,2,3,4,5,6,7,16,17,11,41,42,46,47,48,15,49,43,44,45,9,10,12};
                for(unsigned i=0;i<std::size(collectionFields);++i)
                    c9QaValues_[40+i]=albumRenderer_.collectionQa(collectionFields[i],0);
                c9QaValues_[64]=static_cast<LRESULT>(trackScroll_.position()*1000.);
                c9QaValues_[65]=static_cast<LRESULT>(trackScroll_.target()*1000.);
                c9QaValues_[66]=static_cast<LRESULT>(trackScroll_.velocity()*1000.);
                c9QaValues_[67]=trackScroll_.active();
                c9QaValues_[68]=static_cast<LRESULT>(albumRenderer_.lyricScrollPosition()*1000.);
                c9QaValues_[69]=static_cast<LRESULT>(albumRenderer_.lyricScrollTarget()*1000.);
                c9QaValues_[70]=static_cast<LRESULT>(albumRenderer_.lyricScrollVelocity()*1000.);
                c9QaValues_[71]=albumRenderer_.lyricScrollActive();
                c9QaValues_[72]=static_cast<LRESULT>(albumRenderer_.lyricMaximumScroll()*1000.);
                c9QaLatched_=true;return static_cast<LRESULT>(++c9QaRevision_);
            }
            if(wParam==101U){c9QaLatched_=false;return TRUE;}
            if(wParam==102U)return static_cast<LRESULT>(c9QaRevision_);
            return c9QaLatched_ && wParam<c9QaValues_.size()?c9QaValues_[wParam]:-1;
        }
        if (message == AsyncTextView::readyMessage()) {
            if (mode_ == PlayerShellMode::album) {
                (void)frameClock_.requestImmediate();
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        }
        if (message == rootPresentationVisibilityMessage()) {
            setAlbumArtworkActive(mode_ == PlayerShellMode::album &&
                DisplayEnvironmentService::instance().presentationVisible(window_));
            if (wParam == 0U) {
                KillTimer(window_, kAlbumLyricsWakeTimer);
                lyricsTimerArmed_ = false;
                lyricsTimerDeadlineMs_ = 0;
                preparationRetryQpc_ = 0;
                KillTimer(window_,0xA4F1U);a4RetryArmed_=false;
                ambientController_.shutdown();
                albumRenderer_.suspendCurated();
                albumRenderer_.suspendTextPreparation();
                ambientPumpClock_.stop();
                frameClock_.stop();
            } else {
                (void)frameClock_.requestImmediate();
                invalidatePresentation();
            }
            return 0;
        }
        if (message == kShellRefreshMessage) {
            (void)UiInvalidationBus::instance().consume(
                UiInvalidationSurface::nativePanels, window_);
            // Album owns no playback child that can invalidate its parent.
            // Wake the parent explicitly so pause/resume, volume and track
            // changes re-evaluate ambient eligibility even after both clocks
            // were intentionally stopped while paused.
            if (mode_ == PlayerShellMode::album) {
                (void)frameClock_.requestImmediate();
                InvalidateRect(window_, nullptr, FALSE);
            } else {
                redrawNavigationNow();
            }
            return 0;
        }
        if (message == collection::visualChangedMessage()) {
            albumRenderer_.retryVisualStyle();ambientController_.retryRendering();
            (void)frameClock_.requestImmediate();invalidatePresentation();return 0;
        }
        if (message == collection::qaMessage()) {
            if(!c9QaAllowed())return E_ACCESSDENIED;
            if (wParam >= 2400U && wParam <= 2412U) {
                const auto stats = albumRenderer_.bookletTextStats();
                const std::uint64_t values[]{static_cast<std::uint64_t>(stats.status),
                    stats.generation, stats.submissions, stats.starts, stats.cancellations,
                    stats.publications, stats.layoutsCreated, stats.running ? 1U : 0U,
                    albumRenderer_.liveTextLayouts(), albumRenderer_.peakTextLayouts(),
                    static_cast<std::uint64_t>(albumRenderer_.bookletMaximumScroll()*1000.f),
                    static_cast<std::uint64_t>(albumRenderer_.bookletScrollPosition()*1000.f),
                    booklet_.openDestination() ? 1U : 0U};
                return static_cast<LRESULT>(values[wParam - 2400U]);
            }
            if (wParam >= 2413U && wParam <= 2420U) {
                const auto s = albumRenderer_.plainTextStats();
                const std::uint64_t values[]{static_cast<std::uint64_t>(s.status),s.generation,
                    s.submissions,s.starts,s.cancellations,s.publications,s.layoutsCreated,s.running?1U:0U};
                return static_cast<LRESULT>(values[wParam-2413U]);
            }
            if(wParam==2000U)return static_cast<LRESULT>(frameClock_.acquiredCredits());
            if(wParam==2001U)return static_cast<LRESULT>(frameClock_.begunCredits());
            if(wParam==2002U)return static_cast<LRESULT>(frameClock_.retiredHeldCredits());
            if(wParam>=2010U&&wParam<=2023U){const auto c=Runtime::instance().visualPumpCounters();
                const std::uint64_t values[]{c.calls,c.readFailures,c.catchupResets,c.publishRejected,c.softCatchups,
                    c.clockTransientFailures,c.authoritativeDiscontinuities,c.discontinuityCommitted,
                    c.clockBackwardResets,c.lastClockBackwardUs,c.lastDiscontinuityReason,
                    Runtime::instance().streamGeneration(),c.clockJitterIgnored,c.lastClockJitterUs};
                return static_cast<LRESULT>(values[wParam-2010U]);}
            const auto result=albumRenderer_.collectionQa(static_cast<unsigned>(wParam),lParam);
            if(wParam>=100U&&result==TRUE){
                (void)frameClock_.requestImmediate();
                InvalidateRect(window_,nullptr,FALSE);
            }
            return result;
        }
        if (message == playerShellModeMessage()) {
            if (wParam > 1U) return FALSE;
            return setMode(static_cast<PlayerShellMode>(wParam)) ? TRUE : FALSE;
        }
        if (message == albumArtworkReadyMessage() ||
            message == albumContentReadyMessage()) {
            if (mode_ == PlayerShellMode::album) {
                // A session/content change can select artwork already resident
                // in the broker. Cache hits do not emit an artwork-ready event.
                syncAlbumThemeIntent(true);
            }
            (void)frameClock_.requestImmediate();
            invalidatePresentation();
            return 0;
        }
        if (message == albumFrameMessage()) {
            if (frameClock_.consume(wParam)) {
                if (qaFastModePending_) {
                    qaFastModePending_=false;
                    const auto held=frameClock_.diagnosticDeliveryState() & 8U;
                    const auto retired=frameClock_.retiredHeldCredits();
                    (void)setMode(PlayerShellMode::normal);
                    (void)setMode(PlayerShellMode::album);
                    qaFastModeResult_=held && frameClock_.retiredHeldCredits()==retired ? 1 : 0;
                }
                if (mode_ == PlayerShellMode::normal) {
                    redrawNavigationNow();
                } else {
                    invalidatePresentation();
                    a4::trace("album.frame.consume",r11FrameCounter_);
                    // FrameNotification delivers this within WM_PAINT,
                    // after input. Its normal handler uses the grant.
                }
            }
            return 0;
        }
        if (message == albumAmbientPumpMessage()) {
            if (!ambientPumpClock_.consume(wParam)) return 0;
            const auto now = albumQpcNow();
            if (mode_ == PlayerShellMode::album &&
                    ambientController_.wantsPump()) {
                (void)ambientController_.pump(now);
                const auto defer = albumAmbientDeferState();
                if (defer.effective) {
                    ambientController_.deferTexture(now, defer.reasonMask);
                } else {
                    ambientController_.resumeTexture(now);
                    ambientController_.observeTexturePolicy(defer.reasonMask);
                }
                if (!qaAlbumAmbientWorkerOnly() &&
                        (!curated::selectedStyle(static_cast<unsigned>(a4::style())) && ambientController_.textureDue(now))) {
                    const auto due = ambientController_.nextTextureQpc();
                    if (due == 0 || now >= due) {
                        (void)frameClock_.requestImmediate();
                    } else {
                        (void)frameClock_.requestAtOrAfter(
                            due, kFrameReasonAmbient);
                    }
                }
                (void)ambientPumpClock_.requestAtOrAfter(
                    ambientController_.nextPumpQpc(now),
                    kFrameReasonAudioPump);
            } else {
                ambientPumpClock_.stop();
            }
            return 0;
        }
        if (message == kThemeTransitionMessage) {
            if (mode_ == PlayerShellMode::normal) {
                navigationThemePaints_ = 0;
                (void)frameClock_.requestImmediate();
                redrawNavigationNow();
            } else {
                (void)frameClock_.requestImmediate();
                invalidatePresentation();
            }
            return 0;
        }
        if (message == shellSurfaceRecoveryMessage()) {
            if (!c9QaAllowed()) return E_ACCESSDENIED;
            if (wParam == 1001U || wParam == 1002U) {
                if (lParam < 0 || lParam > 4096) return FALSE;
                albumRenderer_.queueSurfaceFaultsForQa(static_cast<std::uint32_t>(lParam));
                if (wParam == 1001U) InvalidateRect(window_, nullptr, FALSE);
                return TRUE;
            }
            switch (wParam) {
            case 0U: return static_cast<LRESULT>(albumRenderer_.surfaceAttemptsForQa());
            case 1U: return static_cast<LRESULT>(albumRenderer_.surfaceFaultsForQa());
            case 2U: return albumRenderer_.surfaceFaultsRemainingForQa();
            case 3U: return static_cast<LRESULT>(r11FrameCounter_);
            case 4U: return static_cast<LRESULT>(frameClock_.deliveredPulses());
            case 5U: return albumRenderer_.bindingKey().valid();
            case 6U: return static_cast<LRESULT>(albumRenderer_.surfaceBackend());
            case 7U: return static_cast<LRESULT>(albumRenderer_.lastFrameResult());
            case 8U: return static_cast<LRESULT>(mode_);
            case 9U: return DisplayEnvironmentService::instance().presentationVisible(window_);
            case 10U: return static_cast<LRESULT>(ambientPumpClock_.deliveredPulses());
            case 11U: return ambientController_.wantsPump();
            case 12U: return ThemeService::instance().transitionActive();
            case 13U: return albumRenderer_.animationActive();
            case 14U: return static_cast<LRESULT>(frameClock_.diagnosticDeliveryState());
            case 15U: return static_cast<LRESULT>(qaSurfaceAccepted_);
            case 16U: return static_cast<LRESULT>(preparationRetryQpc_);
            case 17U: return static_cast<LRESULT>(ambientController_.state());
            default: return 0;
            }
        }
        if (message == playerShellQaMessage()) {
            if(!c9QaAllowed())return E_ACCESSDENIED;
            if(wParam==0x4C01U)return static_cast<LRESULT>(
                LibraryModel::instance().diagnosticStaleDescriptions());
            if(wParam==0x4C02U)return static_cast<LRESULT>(
                LibraryModel::instance().diagnosticTitleformatCalls());
            if(wParam==239U)return static_cast<LRESULT>(frameClock_.diagnosticDeliveryState());
            if(wParam==240U)return static_cast<LRESULT>(frameClock_.diagnosticNotificationGeneration());
            if(wParam==242U)return qaFastModeResult_;
            if(wParam==241U) {
                // Reproduce fast cached navigation in one UI dispatch: no
                // intermediate Library Present can replenish a lost credit.
                if(mode_!=PlayerShellMode::album)return FALSE;
                qaFastModePending_=true;
                qaFastModeResult_=-1;
                if(frameClock_.requestImmediate())return TRUE;
                qaFastModePending_=false;
                qaFastModeResult_=0;
                return FALSE;
            }
            // Latch one versioned snapshot before a C2 reader asks for its
            // individual fields.  Without this, animation may advance between
            // SendMessage calls and manufacture an impossible mixed sample.
            if(wParam==152U) {
                curatedQaSnapshot_=albumRenderer_.curatedDiagnostics();
                curatedQaSnapshotValid_=true;
                return static_cast<LRESULT>(++curatedQaSnapshotVersion_);
            }
            if(wParam==153U) {
                curatedQaSnapshotValid_=false;
                return TRUE;
            }
            // C1 selectors are appended; all existing selector numbers remain stable.
            if(wParam>=130U && wParam<=141U) {
                const auto& d=curatedQaSnapshotValid_
                    ?curatedQaSnapshot_:albumRenderer_.curatedDiagnostics();
                switch(wParam) {
                case 130U:return static_cast<LRESULT>(d.referenceFrames);
                case 131U:return static_cast<LRESULT>(d.renderedFrames);
                case 132U:return static_cast<LRESULT>(d.outputRevision);
                case 133U:return static_cast<LRESULT>(d.acceptedRevision);
                case 134U:return static_cast<LRESULT>(d.resetCount);
                case 135U:return static_cast<LRESULT>(d.bytes);
                case 136U:return static_cast<LRESULT>(std::llround(d.gpuP95Us));
                case 137U:return static_cast<LRESULT>(d.lastHr);
                case 138U:return d.ready?TRUE:FALSE;
                case 139U:return d.budgetPaused?TRUE:FALSE;
                case 140U:return static_cast<LRESULT>(d.simulationDispatches);
                case 141U:return d.active?TRUE:FALSE;
                }
            }
            if((wParam>=142U&&wParam<=145U)||(wParam>=154U&&wParam<=156U)||wParam==177U) {
                if(wParam==142U)(void)curated::setMode(static_cast<curated::Mode>(std::clamp<LPARAM>(lParam,0,1)));
                if(wParam==143U)(void)curated::setPalette(static_cast<curated::Palette>(std::clamp<LPARAM>(lParam,0,1)));
                if(wParam==144U)curated::restart();
                if(wParam==145U)(void)a4::setStyle(static_cast<a4::Style>(std::clamp<LPARAM>(lParam,0,3)));
                if(wParam==154U)(void)curated::handleMenu(lParam==1?41113U:41112U);
                if(wParam==155U)(void)curated::handleMenu(41140U+static_cast<UINT>(std::clamp<LPARAM>(lParam,0,2)));
                if(wParam==156U)(void)a4::setMaterialStyle(static_cast<a4::MaterialStyle>(std::clamp<LPARAM>(lParam,0,2)));
                if(wParam==177U)(void)a4::setV2MainBands(static_cast<a4::V2MainBands>(std::clamp<LPARAM>(lParam,0,3)));
                curatedQaSnapshotValid_=false;
                albumRenderer_.retryVisualStyle();ambientController_.retryRendering();
                (void)frameClock_.requestImmediate();InvalidateRect(window_,nullptr,FALSE);return TRUE;
            }
            if(wParam==146U)return static_cast<LRESULT>(curated::mode());
            if(wParam==147U)return static_cast<LRESULT>(curated::palette());
            if(wParam==157U)return static_cast<LRESULT>(curated::response());
            if(wParam==158U){const auto speed=curated::causticsSpeed();return speed<1.25f?0:speed<1.75f?1:2;}
            if(wParam==159U)return static_cast<LRESULT>(a4::materialStyle());
            if(wParam==178U)return static_cast<LRESULT>(a4::v2MainBands());
            // C2 progress observability: production, draw and accepted Present
            // remain distinct, while loss of GPU timing is reported separately
            // from loss of the visual renderer.
            const auto& curatedQa=curatedQaSnapshotValid_
                ?curatedQaSnapshot_:albumRenderer_.curatedDiagnostics();
            if(wParam==148U)return static_cast<LRESULT>(curatedQa.drawnRevision);
            if(wParam==149U)return static_cast<LRESULT>(std::llround(curatedQa.prepareLastUs));
            if(wParam==150U)return static_cast<LRESULT>(std::llround(curatedQa.discardedCatchupSeconds*1000.0));
            if(wParam==151U)return curatedQa.gpuTimingAvailable?TRUE:FALSE;

            if(wParam==190U){playback_control::get()->next();return TRUE;}
            if(wParam==191U)return static_cast<LRESULT>(albumKeyboardMessages_);
            if(wParam==192U)return lastAlbumMenuSelection_;
            if(wParam==194U)return EndMenu();
            if(wParam==188U)return static_cast<LRESULT>(staleAlbumMenuActions_);
            if(wParam==189U){
                const auto s=AlbumSessionService::instance().snapshot();
                const auto user=DatabaseService::instance().userStateSnapshot();
                return s && s->state==AlbumSessionState::ready && s->currentInSession<s->tracks.size() &&
                    user && user->available && !user->stale && !s->tracks[s->currentInSession].entityId.empty() &&
                    user->entityId==s->tracks[s->currentInSession].entityId?TRUE:FALSE;
            }
            if (wParam == 0U) return spectrumHeightDip_;
            if (wParam == 1U) {
                RECT client{};
                GetClientRect(window_, &client);
                const auto dpi = std::max<UINT>(1U, GetDpiForWindow(window_));
                return computePlayerShellLayout(
                    client.right - client.left, client.bottom - client.top, dpi,
                    mode_ == PlayerShellMode::album,
                    spectrumHeightDip_, albumTheaterMode()).transport.y;
            }
            if (wParam == 2U) {
                return static_cast<LRESULT>(mode_);
            }
            if (wParam == 65U) return static_cast<LRESULT>(a4::style());
            if (wParam == 2200U) return albumTheaterMode() ? TRUE : FALSE;
            if (wParam == 2201U) {
                applyAlbumTheaterMode(lParam != 0);return TRUE;
            }
            if (wParam == 2202U) {
                Runtime::instance().simulateVisualSourceStallForQa();return TRUE;
            }
            if (wParam == 2203U)
                return static_cast<LRESULT>(Runtime::instance().visualSourceRecoveryCount());
            if (wParam == 66U) return static_cast<LRESULT>(a4::grain());
            if (wParam == 67U || wParam == 68U) {
                const bool changed = wParam == 67U
                    ? a4::setStyle(lParam != 0 ? a4::Style::spectralAlloy
                                               : a4::Style::flow31)
                    : a4::setGrain(static_cast<a4::Grain>(
                        std::clamp<LPARAM>(lParam, 0, 2)));
                if (changed) {
                    albumRenderer_.retryVisualStyle();
                    ambientController_.retryRendering();
                    (void)frameClock_.requestImmediate();
                    InvalidateRect(window_, nullptr, FALSE);
                }
                return TRUE;
            }
            if(wParam==98U) {
                const bool changed=albumRenderer_.capAlloyForDiagnostics(static_cast<unsigned>(lParam));
                if(changed){(void)frameClock_.requestImmediate();InvalidateRect(window_,nullptr,FALSE);}
                return changed?TRUE:FALSE;
            }
            if(wParam==99U) {
                albumRenderer_.retryVisualStyle();ambientController_.retryRendering();
                (void)frameClock_.requestImmediate();InvalidateRect(window_,nullptr,FALSE);return TRUE;
            }
            if(wParam==116U)return static_cast<LRESULT>(a4::qualityMode());
            if(wParam==117U){
                const bool changed=a4::setQualityMode(static_cast<a4::QualityMode>(
                    std::clamp<LPARAM>(lParam,0,3)));
                if(changed){albumRenderer_.retryVisualStyle();ambientController_.retryRendering();
                    (void)frameClock_.requestImmediate();InvalidateRect(window_,nullptr,FALSE);}
                return TRUE;
            }
            // R12 read-only counters. IDs 67/68 remain the only preference setters.
            if(wParam>=82U && wParam<=97U) {
                const auto& d=albumRenderer_.alloyDiagnostics();
                switch(wParam) {
                case 82U:return static_cast<LRESULT>(d.effectiveTier);
                case 83U:return static_cast<LRESULT>(std::llround(d.gpuDecorativeP95Us));
                case 84U:return static_cast<LRESULT>(std::llround(d.gpuBlurP95Us));
                case 85U:return static_cast<LRESULT>(std::llround(d.gpuSceneP95Us));
                case 86U:return static_cast<LRESULT>(std::llround(d.gpuFinishP95Us));
                case 87U:return static_cast<LRESULT>(d.budgetEpoch);
                case 88U:return static_cast<LRESULT>(d.budgetSamples);
                case 89U:return static_cast<LRESULT>(d.budgetWindows);
                case 90U:return static_cast<LRESULT>(d.filmUpdates);
                case 91U:return static_cast<LRESULT>(d.finishedReuses);
                case 92U:return static_cast<LRESULT>(d.queryDrops);
                case 93U:return static_cast<LRESULT>(d.queryDisjoint);
                case 94U:return d.measured?TRUE:FALSE;
                case 95U:return static_cast<LRESULT>(std::llround(d.cpuPrepareLastUs));
                case 96U:return static_cast<LRESULT>(std::llround(d.cpuStageSumUs));
                case 97U:return 12; // explicit capability, not inferred from a zero counter
                }
            }
            if (wParam >= 69U && wParam <= 81U) {
                const auto& diagnostics = albumRenderer_.alloyDiagnostics();
                switch (wParam) {
                case 69U: return static_cast<LRESULT>(diagnostics.fieldUpdates);
                case 70U: return static_cast<LRESULT>(diagnostics.sceneCompositions);
                case 71U: return static_cast<LRESULT>(diagnostics.resourceBytes);
                case 72U: return static_cast<LRESULT>(diagnostics.lastHr);
                case 73U: return diagnostics.ready ? TRUE : FALSE;
                case 74U: return diagnostics.low ? TRUE : FALSE;
                case 75U: return diagnostics.fieldProduced ? TRUE : FALSE;
                case 76U: return diagnostics.latched ? TRUE : FALSE;
                case 77U: return static_cast<LRESULT>(std::llround(
                    diagnostics.gpuFieldP95Us));
                case 78U: return static_cast<LRESULT>(std::llround(
                    diagnostics.gpuInclusiveP95Us));
                case 79U: return static_cast<LRESULT>(diagnostics.reusedScenes);
                case 80U: return static_cast<LRESULT>(diagnostics.fieldRevision);
                case 81U: return albumRenderer_.alloySceneReady() ? TRUE : FALSE;
                default: break;
                }
            }
            if (wParam >= 100U && wParam <= 111U) {
                const auto progress = ambientController_.progress();
                const auto input =
                    InteractionLatencyGuard::instance().snapshot();
                switch (wParam) {
                case 100U: return static_cast<LRESULT>(progress.desiredRevision);
                case 101U: return static_cast<LRESULT>(progress.producedRevision);
                case 102U: return static_cast<LRESULT>(progress.acceptedRevision);
                case 103U: return static_cast<LRESULT>(progress.nextTextureQpc);
                case 104U: return static_cast<LRESULT>(
                    progress.lastTextureProductionQpc);
                case 105U: return static_cast<LRESULT>(progress.attempts);
                case 106U: return static_cast<LRESULT>(progress.acceptedPresents);
                case 107U: return static_cast<LRESULT>(progress.rejectedPresents);
                case 108U: return static_cast<LRESULT>(progress.deferTransitions);
                case 109U: return static_cast<LRESULT>(progress.deferReasonMask);
                case 110U: return progress.deferred ? TRUE : FALSE;
                case 111U: return static_cast<LRESULT>(input.lastRealInputQpc);
                default: break;
                }
            }
            if (wParam >= 25U && wParam <= 32U) {
                const auto diagnostics = albumAmbientDiagnostics();
                switch (wParam) {
                case 25U: return static_cast<LRESULT>(diagnostics.mode);
                case 26U: return static_cast<LRESULT>(diagnostics.fallback);
                case 27U: return static_cast<LRESULT>(
                    diagnostics.textureUpdates);
                case 28U: return static_cast<LRESULT>(
                    diagnostics.featurePublications);
                case 29U: return static_cast<LRESULT>(
                    diagnostics.resourceBytes);
                case 30U: return static_cast<LRESULT>(std::llround(
                    diagnostics.gpuMicroseconds));
                case 31U: return static_cast<LRESULT>(std::llround(
                    diagnostics.gpuP95Microseconds));
                case 32U: return (diagnostics.gpuMeasured ? 1 : 0) |
                    (diagnostics.degraded ? 2 : 0);
                default: break;
                }
            }
            if (wParam >= 33U && wParam <= 42U) {
                const auto snapshot =
                    RealtimeAnalysisHub::instance().ambientSnapshot();
                switch (wParam) {
                case 33U: return static_cast<LRESULT>(
                    RealtimeAnalysisHub::instance().activeFeatureMask());
                case 34U: return static_cast<LRESULT>(
                    RealtimeAnalysisHub::instance().ambientProcessedWindows());
                case 35U: return static_cast<LRESULT>(
                    Runtime::instance().latestRealtimeSequence());
                case 36U: return static_cast<LRESULT>(
                    snapshot ? snapshot->publicationRevision : 0U);
                case 37U: return static_cast<LRESULT>(
                    snapshot ? snapshot->streamGeneration : 0U);
                case 38U: return static_cast<LRESULT>(
                    snapshot ? snapshot->sourceSequence : 0U);
                case 39U: return static_cast<LRESULT>(
                    Runtime::instance().streamGeneration());
                case 40U: return static_cast<LRESULT>(ambientController_.state());
                case 41U: return ambientController_.wantsPump() ? TRUE : FALSE;
                case 42U: return ambientController_.textureDue(albumQpcNow())
                    ? TRUE : FALSE;
                default: break;
                }
            }
            if (wParam >= 43U && wParam <= 52U) {
                const auto state = AlbumSessionService::instance().snapshot();
                const auto content = albumContent(state);
                const auto identity = albumArtworkIdentity(state);
                const auto artwork = albumArtwork(state, content, identity);
                const auto playback = Runtime::instance().playbackSnapshot();
                switch (wParam) {
                case 43U: return DisplayEnvironmentService::instance()
                    .presentationVisible(window_) ? TRUE : FALSE;
                case 44U: return playback.state == PlaybackState::playing
                    ? TRUE : FALSE;
                case 45U: return (playback.flags & kPlaybackFlagMuted) != 0
                    ? TRUE : FALSE;
                case 46U: return albumRenderer_.surfaceBackend() ==
                    SurfaceBackend::hardwareDxgi ? TRUE : FALSE;
                case 47U: return artwork ? TRUE : FALSE;
                case 48U: return booklet_.openDestination() ||
                    booklet_.sample(AlbumBookletModel::Clock::now()) > 0.001f
                    ? TRUE : FALSE;
                case 49U: return Runtime::instance().dawPreviewActive()
                    ? TRUE : FALSE;
                case 50U: return !state || state->outsideSession
                    ? TRUE : FALSE;
                case 51U: return motionMode() != MotionMode::normal
                    ? TRUE : FALSE;
                case 52U: return mode_ == PlayerShellMode::album
                    ? TRUE : FALSE;
                default: break;
                }
            }
            if (wParam >= 53U && wParam <= 64U) {
                const auto snapshot =
                    RealtimeAnalysisHub::instance().ambientSnapshot();
                switch (wParam) {
                case 53U: return static_cast<LRESULT>(
                    ambientController_.visualRevision());
                case 54U: return static_cast<LRESULT>(
                    ambientController_.presentedRevision());
                case 55U: return ambientController_.physicalMotion()
                    ? TRUE : FALSE;
                case 56U: return ambientController_.lastIngestAccepted()
                    ? TRUE : FALSE;
                case 57U: return static_cast<LRESULT>(
                    snapshot ? snapshot->playbackSerial : 0U);
                case 58U:
                case 59U:
                case 60U: {
                    const auto band = static_cast<std::size_t>(wParam - 58U);
                    const auto power = snapshot ? snapshot->bandPower[band] : 0.0;
                    const auto db = power > 0.0 && std::isfinite(power)
                        ? 10.0 * std::log10(power) : -120.0;
                    return static_cast<LRESULT>(std::llround(db * 100.0));
                }
                case 61U: return static_cast<LRESULT>(
                    Runtime::instance().playbackSerial());
                case 62U: return Runtime::instance().recordingIdentityHash() != 0
                    ? TRUE : FALSE;
                case 63U: return Runtime::instance().playbackSnapshot()
                    .trackIdentityHash != 0 ? TRUE : FALSE;
                case 64U: {
                    metadb_handle_ptr current;
                    return playback_control::get()->get_now_playing(current) &&
                        current.is_valid() ? TRUE : FALSE;
                }
                default: break;
                }
            }
            if (wParam >= 20U && wParam <= 24U) {
                const auto theme = ThemeService::instance().snapshot();
                switch (wParam) {
                case 20U: return static_cast<LRESULT>(
                    std::llround(theme.transitionProgress * 1000000.0f));
                case 21U: return static_cast<LRESULT>(navigationThemePaints_);
                case 22U: return 0;
                case 23U: return static_cast<LRESULT>(
                    frameClock_.deliveredPulses());
                case 24U: return static_cast<LRESULT>(
                    albumRenderer_.lastFrameResult());
                default: break;
                }
            }
            if (wParam >= 180U && wParam <= 185U) {
                const auto& d=albumRenderer_.artworkTransitionDiagnostics();
                switch(wParam) {
                case 180U: return static_cast<LRESULT>(d.acceptedFrames);
                case 181U: return static_cast<LRESULT>(d.rejectedFrames);
                case 182U: return static_cast<LRESULT>(d.fadeFallbackFrames);
                case 183U: return static_cast<LRESULT>(std::llround(d.maximumAcceptedIntervalMilliseconds*1000));
                case 184U: return static_cast<LRESULT>(std::llround(albumRenderer_.artworkIntervalP95Milliseconds()*1000));
                case 185U: return d.reconstructionPrepared?TRUE:FALSE;
                }
            }
            if (wParam >= 15U && wParam <= 19U) {
                const auto& diagnostics =
                    albumRenderer_.artworkTransitionDiagnostics();
                switch (wParam) {
                case 15U: return albumRenderer_.artworkTransitionActive()
                    ? TRUE : FALSE;
                case 16U: return static_cast<LRESULT>(
                    diagnostics.presentedFrames);
                case 17U: return static_cast<LRESULT>(
                    diagnostics.reconstructionFrames);
                case 18U: return static_cast<LRESULT>(std::llround(
                    diagnostics.maximumFrameMilliseconds * 1000.0));
                case 19U: return static_cast<LRESULT>(
                    diagnostics.generation);
                default: break;
                }
            }
            if (wParam >= 3U && wParam <= 14U) {
                const auto state = AlbumSessionService::instance().snapshot();
                const auto content = albumContent(state);
                const auto lyrics = LyricsService::instance().snapshot();
                reconcileAlbumTrackScroll(state, content);
                RECT client{};
                GetClientRect(window_, &client);
                const auto dpi = std::max<UINT>(1U, GetDpiForWindow(window_));
                const auto widthDip = (client.right - client.left) * 96.0f / dpi;
                constexpr auto topInsetDip = 36.0f;
                const auto layout = computeAlbumModeLayout(
                    widthDip,
                    albumContentHeightDip(
                        (client.bottom - client.top) * 96.0f / dpi),
                    lyricsSurfaceAvailable(lyrics), compactSelection_,
                    albumPanelVisibility());
                const auto scaled = [](float value) {
                    return static_cast<LRESULT>(std::llround(value * 1000.0f));
                };
                switch (wParam) {
                case 3U: return scaled(trackScrollDip_);
                case 4U: return scaled(albumTrackMaximumScroll(
                    layout, state ? state->tracks.size() : 0U));
                case 5U: return scaled(layout.cover.left);
                case 6U: return scaled(layout.cover.top + topInsetDip);
                case 7U: return scaled(layout.cover.right);
                case 8U: return scaled(layout.cover.bottom + topInsetDip);
                case 9U: return scaled(booklet_.sample(
                    AlbumBookletModel::Clock::now()));
                case 10U: return static_cast<LRESULT>(
                    state ? state->tracks.size() : 0U);
                case 11U: return scaled(layout.tracks.left);
                case 12U: return scaled(layout.tracks.top + topInsetDip);
                case 13U: return scaled(layout.tracks.right);
                case 14U: return scaled(layout.tracks.bottom + topInsetDip);
                default: break;
                }
            }
            if (wParam >= 112U && wParam <= 115U) {
                const auto lyrics = LyricsService::instance().snapshot();
                RECT client{};
                GetClientRect(window_, &client);
                const auto dpi = std::max<UINT>(1U, GetDpiForWindow(window_));
                const auto layout = computeAlbumModeLayout(
                    (client.right - client.left) * 96.0f / dpi,
                    albumContentHeightDip(
                        (client.bottom - client.top) * 96.0f / dpi),
                    lyricsSurfaceAvailable(lyrics), compactSelection_,
                    albumPanelVisibility());
                const auto scaled = [](float value) {
                    return static_cast<LRESULT>(std::llround(value * 1000.0f));
                };
                constexpr auto topInsetDip = 36.0f;
                switch (wParam) {
                case 112U: return scaled(layout.lyrics.left);
                case 113U: return scaled(layout.lyrics.top + topInsetDip);
                case 114U: return scaled(layout.lyrics.right);
                case 115U: return scaled(layout.lyrics.bottom + topInsetDip);
                default: break;
                }
            }
            // Album scroll telemetry. Read-only views of the two independent
            // controllers plus one explicit reset so scripted wheel schedules
            // start from a known offset. Selector 121/125 report liveness and
            // never the animated value, which selectors 118/122 already carry.
            if (wParam >= 118U && wParam <= 127U) {
                const auto scaled = [](double value) {
                    return static_cast<LRESULT>(std::llround(value * 1000.0));
                };
                switch (wParam) {
                case 118U: return scaled(trackScroll_.position());
                case 119U: return scaled(trackScroll_.target());
                case 120U: return scaled(trackScroll_.velocity());
                case 121U: return trackScroll_.active() ? TRUE : FALSE;
                case 122U: return scaled(albumRenderer_.lyricScrollPosition());
                case 123U: return scaled(albumRenderer_.lyricScrollTarget());
                case 124U: return scaled(albumRenderer_.lyricScrollVelocity());
                case 125U: return albumRenderer_.lyricScrollActive()
                    ? TRUE : FALSE;
                case 126U: return scaled(albumRenderer_.lyricMaximumScroll());
                case 127U:
                    trackScroll_.setPosition(0.0);
                    trackScrollDip_ = 0.0f;
                    albumRenderer_.resetLyricScroll();
                    // Reset position only. Follow remains a real production behaviour;
                // a measurement must not silently disable it for the next hour.
                    (void)frameClock_.requestImmediate();
                    InvalidateRect(window_, nullptr, FALSE);
                    return TRUE;
                default: break;
                }
            }
            if (wParam >= 160U && wParam <= 176U) {
                const auto library = LibraryModel::instance().diagnostics();
                const auto visual = Runtime::instance().visualPumpCounters();
                const auto analysis = RealtimeAnalysisHub::instance().snapshot();
                const auto dynamics = analysis ? analysis->dynamics : nullptr;
                switch (wParam) {
                case 160U: return static_cast<LRESULT>(
                    DisplayEnvironmentService::instance()
                        .displayConfigQueryCount());
                case 161U: return static_cast<LRESULT>(library.fullRebuilds);
                case 162U: return static_cast<LRESULT>(library.deltaRebuilds);
                case 163U: return static_cast<LRESULT>(library.describedTracks);
                case 164U: return static_cast<LRESULT>(
                    library.publicationDeepCopyBytes);
                case 165U: return static_cast<LRESULT>(
                    library.indexedNowPlayingHits);
                case 166U: return static_cast<LRESULT>(
                    library.nowPlayingLinearScans);
                case 167U: return static_cast<LRESULT>(
                    visual.clockTransientFailures);
                case 168U: return static_cast<LRESULT>(
                    visual.authoritativeDiscontinuities);
                case 169U: return static_cast<LRESULT>(visual.publishRejected);
                case 170U: return static_cast<LRESULT>(
                    visual.discontinuityCommitted);
                case 171U: return static_cast<LRESULT>(
                    RealtimeAnalysisHub::instance().droppedPublications());
                case 172U: return static_cast<LRESULT>(analysis
                    ? analysis->discontinuityReasonMask : 0U);
                case 173U: return static_cast<LRESULT>(dynamics
                    ? dynamics->partialBinsCommitted : 0U);
                case 174U: return static_cast<LRESULT>(dynamics
                    ? dynamics->unknownMicroseconds : 0U);
                case 175U: return static_cast<LRESULT>(dynamics
                    ? dynamics->sanitizedSamples : 0U);
                case 176U: return static_cast<LRESULT>(analysis
                    ? analysis->discontinuityRejectedPublications : 0U);
                default: break;
                }
            }
            return -1;
        }
        switch (message) {
        case WM_GETOBJECT:
            if(static_cast<LONG>(lParam)==OBJID_CLIENT){
                if(!albumAccessible_) albumAccessible_=new AccessibleSurface(window_,
                    [this]{return albumAccessibleNodes();},[this]{return albumFocus_;},
                    [this](long id,bool activate){focusAlbumControl(id);if(activate)activateAlbumControl(id);});
                return LresultFromObject(IID_IAccessible,wParam,albumAccessible_);
            }
            break;
        case WM_GETDLGCODE:
            if(mode_==PlayerShellMode::album) return DLGC_WANTARROWS|DLGC_WANTTAB|DLGC_WANTCHARS;
            break;
        case WM_SHOWWINDOW:
            if(wParam==0){
                setAlbumArtworkActive(false);
                preparationRetryQpc_ = 0;
                albumRenderer_.suspendTextPreparation();
                KillTimer(window_,0xA4F1U);a4RetryArmed_=false;
                ambientController_.shutdown();albumRenderer_.suspendCurated();
                ambientPumpClock_.stop();frameClock_.stop();trackPress_.reset();
            }else{
                (void)frameClock_.requestImmediate();invalidatePresentation();
            }
            break;
        case WM_SETFOCUS:
            InvalidateRect(window_,nullptr,FALSE);
            break;
        case WM_KILLFOCUS:
            albumRenderer_.setFocusRect({});InvalidateRect(window_,nullptr,FALSE);
            break;
        case WM_GETMINMAXINFO:
            if (lParam != 0) {
                auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
                const auto dpi = window != nullptr
                    ? std::max<UINT>(1U, GetDpiForWindow(window)) : 96U;
                limits->ptMinTrackSize.x = static_cast<LONG>(
                    (640U * dpi + 48U) / 96U);
                limits->ptMinTrackSize.y = static_cast<LONG>(
                    (420U * dpi + 48U) / 96U);
            }
            return 0;
        case WM_CREATE:
            window_ = window;
            albumKeyboardFilter_=std::make_unique<AlbumKeyboardFilter>(*this);
            SetWindowLongPtrW(window_, GWL_EXSTYLE,
                GetWindowLongPtrW(window_, GWL_EXSTYLE) | WS_EX_CONTROLPARENT);
            installAlbumWheelHook();
            // A process cold start always has a useful, populated landing
            // surface. Album remains an explicit navigation action and its
            // cached session/data are preserved rather than cleared.
            mode_ = PlayerShellMode::normal;
            g_playerShellMode.store(mode_, std::memory_order_release);
            frameClock_.attach(window_, albumFrameMessage());
            frameClock_.setTelemetrySurface(TelemetrySurface::album);
            ambientPumpClock_.attach(window_, albumAmbientPumpMessage());
            ambientPumpClock_.setTelemetrySurface(TelemetrySurface::albumAnalysis);
            contentCache_.attach(window_);
            LyricsService::instance().subscribe(
                window_, albumContentReadyMessage());
            ThemeService::instance().subscribe(
                window_, kThemeTransitionMessage);
            UiInvalidationBus::instance().registerWindow(
                UiInvalidationSurface::nativePanels,
                window_, kShellRefreshMessage);
            createChildren();
            layoutChildren();
            g_shellEpoch.fetch_add(1, std::memory_order_acq_rel);
            return 0;
        case WM_DESTROY:
            preparationRetryQpc_ = 0;
            albumKeyboardFilter_.reset();
            if(albumAccessible_){albumAccessible_->detach();albumAccessible_->Release();albumAccessible_=nullptr;}
            uninstallAlbumWheelHook();
            KillTimer(window,0xA4F1U);a4RetryArmed_=false;
            KillTimer(window, kAlbumLyricsWakeTimer);
            lyricsTimerArmed_ = false;
            lyricsTimerDeadlineMs_ = 0;
            UiInvalidationBus::instance().unregisterWindow(
                UiInvalidationSurface::nativePanels, window_);
            ThemeService::instance().unsubscribe(window_);
            frameClock_.detach();
            ambientController_.shutdown();
            albumRenderer_.suspendCurated();
            ambientPumpClock_.detach();
            destroyChildren();
            childHost_.release();
            setAlbumArtworkActive(false);
            contentCache_.detach();
            LyricsService::instance().unsubscribe(window_);
            albumRenderer_.reset();
            if (uiFont_ != nullptr) {
                DeleteObject(uiFont_);
                uiFont_ = nullptr;
                uiFontDpi_ = 0;
            }
            window_ = nullptr;
            return 0;
        case WM_SIZE:
            layoutChildren();
            if (mode_ == PlayerShellMode::album) {
                // Library owns a different surface. Resizing the hidden Album
                // swap-chain on every Library pointer step only stalls the UI
                // thread and discards a retained ambient frame unnecessarily.
                // The Album path catches up when it becomes visible below.
                albumRenderer_.resize();
                // Preserve presentation credits while ResizeBuffers retains
                // the same swap-chain. The clock owns a duplicate HANDLE, so
                // a failed resize can retire its surface before reconciliation.
                frameClock_.setReadinessBinding(albumRenderer_.bindingKey());
            }
            reconcileAlbumTrackScroll(
                AlbumSessionService::instance().snapshot());
            InvalidateRect(window_, nullptr, FALSE);
            // A shrinking window exposes no new region, so Windows may never
            // deliver the WM_PAINT that would rebuild the composited Album
            // surface and re-arm its clock. Any style whose demand can settle
            // (a static album visual) would then stay idle until unrelated
            // input. A resize is never idle, so ask for the next frame here.
            if (mode_ == PlayerShellMode::album) {
                (void)frameClock_.requestImmediate();
            }
            return 0;
        case WM_DPICHANGED:
            layoutChildren();
            frameClock_.setReadinessBinding({});
            albumRenderer_.releaseGraphics();
            if (get_host().is_valid()) {
                get_host()->on_size_limit_change(window_, uie::size_limit_all);
            }
            return 0;
        case WM_SETTINGCHANGE:
        case WM_SYSCOLORCHANGE:
        case WM_TIMECHANGE:
            if (workspace_.is_valid() && workspace_->get_wnd() != nullptr) {
                SendMessage(workspace_->get_wnd(), message, wParam, lParam);
            }
            return 0;
        case WM_TIMER:
            if(wParam==0xA4F1U){
                KillTimer(window_,0xA4F1U);a4RetryArmed_=false;
                ambientController_.retryRendering();
                (void)frameClock_.requestImmediate();InvalidateRect(window_,nullptr,FALSE);return 0;
            }
            if (wParam == kAlbumLyricsWakeTimer) {
                KillTimer(window, kAlbumLyricsWakeTimer);
                lyricsTimerArmed_ = false;
                lyricsTimerDeadlineMs_ = 0;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            return TRUE;
        case WM_PARENTNOTIFY:
            if (LOWORD(wParam) == WM_LBUTTONDOWN &&
                beginDeckResize(GET_Y_LPARAM(lParam))) {
                return 0;
            }
            break;
        case WM_LBUTTONDOWN:
            albumFocus_=0;
            albumRenderer_.setFocusRect({});
            if (beginDeckResize(GET_Y_LPARAM(lParam))) return 0;
            if (beginAlbumControlPress(
                    GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) return 0;
            if (beginRatingPress(GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)))return 0;
            if (beginLyricsPress(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)))
                return 0;
            trackPress_=trackPressAt(GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam));
            if(trackPress_){SetCapture(window_);return 0;}
            break;
        case WM_MOUSEMOVE:
            if(ratingPress_){
                const auto dx=GET_X_LPARAM(lParam)-ratingPressPoint_.x;
                const auto dy=GET_Y_LPARAM(lParam)-ratingPressPoint_.y;
                const auto threshold=std::max(1,MulDiv(4,int(GetDpiForWindow(window_)),96));
                if(dx*dx+dy*dy>threshold*threshold)ratingPressMoved_=true;
                return 0;
            }
            if(!r11TrackingMouse_){
                TRACKMOUSEEVENT event{sizeof(TRACKMOUSEEVENT),TME_LEAVE,window_,0};
                r11TrackingMouse_=TrackMouseEvent(&event)!=FALSE;
            }
            if(mode_==PlayerShellMode::album&&!deckResizeActive_&&!lyricsPress_&&
                a4::style()==a4::Style::spectralAlloy&&motionMode()==MotionMode::normal){
                RECT r{};GetClientRect(window_,&r);
                if(albumRenderer_.setOpticsPointer(
                        2.f*GET_X_LPARAM(lParam)/std::max(1L,r.right)-1.f,
                        2.f*GET_Y_LPARAM(lParam)/std::max(1L,r.bottom)-1.f))
                    (void)frameClock_.requestImmediate();
            }
            if (deckResizeActive_) {
                updateDeckResize(GET_Y_LPARAM(lParam));
                return 0;
            }
            if (lyricsPress_) {
                const auto dx = GET_X_LPARAM(lParam) - lyricsPressPoint_.x;
                const auto dy = GET_Y_LPARAM(lParam) - lyricsPressPoint_.y;
                const auto dpi = std::max<UINT>(1U, GetDpiForWindow(window_));
                const auto threshold = std::max(1, MulDiv(4, dpi, 96));
                if (dx * dx + dy * dy > threshold * threshold)
                    lyricsPressMoved_ = true;
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if(trackPress_){
                const auto pressed=*trackPress_;const auto released=trackPressAt(GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam));
                trackPress_.reset();if(GetCapture()==window_)ReleaseCapture();
                if(released&&pressed.session==released->session&&pressed.composition==released->composition&&
                    pressed.handle==released->handle&&pressed.row==released->row&&pressed.scroll==released->scroll&&
                    EqualRect(&pressed.client,&released->client)){
                    (void)AlbumSessionService::instance().playTrack(pressed.row);syncAlbumThemeIntent(false);
                }
                return 0;
            }
            if (albumControlPress_ != AlbumControl::none) {
                finishAlbumControlPress(
                    GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                return 0;
            }
            if(ratingPress_){
                const auto token=*ratingPress_;ratingPress_.reset();
                RECT r{};GetClientRect(window_,&r);const auto dpi=std::max<UINT>(1U,GetDpiForWindow(window_));
                const auto hit=albumRenderer_.ratingHitAt(GET_X_LPARAM(lParam)*96.f/dpi,
                    GET_Y_LPARAM(lParam)*96.f/dpi-36.f);
                const bool moved=ratingPressMoved_;
                if(GetCapture()==window_)ReleaseCapture();
                if(!moved&&EqualRect(&r,&ratingPressClient_)&&hit&&
                    hit->row==token.row&&hit->value==token.value&&albumRenderer_.validateRatingHit(token))
                    (void)albumRenderer_.applyRatingHit(token,window_);
                ratingPressMoved_=false;(void)frameClock_.requestImmediate();InvalidateRect(window_,nullptr,FALSE);return 0;
            }
            if(!deckResizeActive_&&!lyricsPress_&&showVisualPreferences(GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)))return 0;
            if (deckResizeActive_) {
                deckResizeActive_ = false;
                if (GetCapture() == window_) ReleaseCapture();
                return 0;
            }
            if (lyricsPress_) {
                const auto token = *lyricsPress_;
                RECT current{};
                GetClientRect(window_, &current);
                const auto sameLayout =
                    current.right - current.left == lyricsPressClient_.cx &&
                    current.bottom - current.top == lyricsPressClient_.cy;
                const auto valid = !lyricsPressMoved_ && sameLayout &&
                    albumRenderer_.validateLyricsHit(token);
                lyricsPress_.reset();
                lyricsPressMoved_ = false;
                if (GetCapture() == window_) ReleaseCapture();
                if (valid && playback_control::get()->playback_can_seek()) {
                    albumRenderer_.noteLyricsSeekIntent(token);
                    playback_control::get()->playback_seek(token.seekSeconds);
                    (void)frameClock_.requestImmediate();
                    InvalidateRect(window_, nullptr, FALSE);
                }
                return 0;
            }
            {
                const auto dpi = std::max<UINT>(1U, GetDpiForWindow(window_));
                const auto xDip = static_cast<unsigned>(GET_X_LPARAM(lParam)) *
                    96U / dpi;
                const auto yDip = static_cast<unsigned>(GET_Y_LPARAM(lParam)) *
                    96U / dpi;
                if (yDip < 36U) {
                    if (xDip < 108U) {
                        (void)setMode(PlayerShellMode::normal);
                    } else if (xDip < 216U) {
                        // The tab owns entry into the album experience, not
                        // merely its visual mode. Refreshing here also lets a
                        // second click follow a newly selected/playing track.
                        (void)AlbumSessionService::instance()
                            .enterFromHostContext();
                        (void)setMode(PlayerShellMode::album);
                    }
                    return 0;
                }
            }
            if (mode_ == PlayerShellMode::album) {
                const auto dpi = std::max<UINT>(1U, GetDpiForWindow(window_));
                const auto xDip = static_cast<unsigned>(GET_X_LPARAM(lParam)) *
                    96U / dpi;
                const auto yDip = static_cast<unsigned>(GET_Y_LPARAM(lParam)) *
                    96U / dpi;
                RECT client{};
                GetClientRect(window_, &client);
                const auto widthDip = static_cast<unsigned>(
                    std::max<LONG>(0, client.right - client.left)) * 96U / dpi;
                const auto heightDip = static_cast<unsigned>(
                    std::max<LONG>(0, client.bottom - client.top)) * 96U / dpi;
                const auto contentHeightDip = albumContentHeightDip(
                    static_cast<float>(heightDip));
                const auto contentYDip = static_cast<float>(yDip) - 36.0f;
                const auto state = AlbumSessionService::instance().snapshot();
                const auto content = albumContent(state);
                const auto lyrics = LyricsService::instance().snapshot();
                const auto layout = computeAlbumModeLayout(
                    static_cast<float>(widthDip), contentHeightDip,
                    lyricsSurfaceAvailable(lyrics), compactSelection_,
                    albumPanelVisibility());
                const auto bookletProgress = booklet_.sample(
                    AlbumBookletModel::Clock::now());
                if (bookletProgress > 0.001f ||
                    booklet_.openDestination()) {
                    const auto bookletLayout = computeAlbumBookletLayout(
                        static_cast<float>(widthDip),
                        contentHeightDip, albumPanelVisibility());
                    if (pointInAlbumRect(bookletLayout.close,
                            static_cast<float>(xDip), contentYDip) ||
                        (!pointInAlbumRect(bookletLayout.artwork,
                            static_cast<float>(xDip), contentYDip) &&
                         !pointInAlbumRect(bookletLayout.albumDetails,
                            static_cast<float>(xDip), contentYDip) &&
                         !pointInAlbumRect(bookletLayout.writtenDetails,
                            static_cast<float>(xDip), contentYDip))) {
                        setBookletOpen(false);
                    }
                    return 0;
                }
                if (pointInAlbumRect(layout.cover,
                        static_cast<float>(xDip), contentYDip) &&
                        state && state->state==AlbumSessionState::ready && state->sessionId != 0U) {
                    SetFocus(window_);
                    setBookletOpen(true);
                } else if ((!state || state->state!=AlbumSessionState::ready) &&
                    pointInAlbumRect(layout.chooseAlbum,
                        static_cast<float>(xDip), contentYDip)) {
                    if(state && state->state==AlbumSessionState::error)
                        (void)AlbumSessionService::instance().retry();
                    else setPlayerShellMode(PlayerShellMode::normal);
                } else if (state && state->outsideSession &&
                    pointInAlbumRect(layout.followCurrent,
                        static_cast<float>(xDip), contentYDip)) {
                    (void)AlbumSessionService::instance().enterFromHostContext();
                } else if (pointInAlbumRect(layout.follow,
                        static_cast<float>(xDip), contentYDip)) {
                    followOrRetryAlbumLyrics();
                    InvalidateRect(window_, nullptr, FALSE);
                } else if (pointInAlbumRect(layout.lyricsTab,
                        static_cast<float>(xDip), contentYDip)) {
                    compactSelection_ = AlbumPanelKind::lyrics;
                    InvalidateRect(window_, nullptr, FALSE);
                } else if (pointInAlbumRect(layout.tracksTab,
                        static_cast<float>(xDip), contentYDip)) {
                    compactSelection_ = AlbumPanelKind::tracks;
                    InvalidateRect(window_, nullptr, FALSE);
                } else if (pointInAlbumRect(layout.lyrics,
                        static_cast<float>(xDip), contentYDip)) {
                    // Timed lyric seeks are captured on mouse-down and
                    // validated transactionally on mouse-up.
                }
            }
            return 0;
        case WM_KEYDOWN:
            if(mode_==PlayerShellMode::album && albumKey(static_cast<UINT>(wParam))) return 0;
            if (mode_ == PlayerShellMode::album && wParam == VK_ESCAPE &&
                (booklet_.openDestination() ||
                    booklet_.sample(AlbumBookletModel::Clock::now()) > 0.001f)) {
                setBookletOpen(false);
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            trackPress_.reset();
            ratingPress_.reset();ratingPressMoved_=false;
            deckResizeActive_ = false;
            lyricsPress_.reset();
            lyricsPressMoved_ = false;
            albumControlPress_ = AlbumControl::none;
            break;
        case WM_MOUSELEAVE:
            r11TrackingMouse_=false;
            if(albumRenderer_.setOpticsPointer(0.f,0.f)&&
                    mode_==PlayerShellMode::album&&a4::style()==a4::Style::spectralAlloy)
                (void)frameClock_.requestImmediate();
            return 0;
        case WM_SETCURSOR:
            if (deckResizeActive_) {
                SetCursor(LoadCursor(nullptr, IDC_SIZENS));
                return TRUE;
            }
            break;
        case WM_MOUSEWHEEL:
            if(trackPress_){trackPress_.reset();if(GetCapture()==window_)ReleaseCapture();}
            if (mode_ == PlayerShellMode::album) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ScreenToClient(window_, &point);
                const auto dpi = std::max<UINT>(1U, GetDpiForWindow(window_));
                const auto x = point.x * 96.0f / dpi;
                const auto y = point.y * 96.0f / dpi - 36.0f;
                RECT client{};
                GetClientRect(window_, &client);
                const auto state = AlbumSessionService::instance().snapshot();
                const auto lyrics = LyricsService::instance().snapshot();
                const auto delta = -GET_WHEEL_DELTA_WPARAM(wParam) /
                    120.0f * 56.0f;
                if (booklet_.openDestination() ||
                    booklet_.sample(AlbumBookletModel::Clock::now()) > 0.001f) {
                    const auto bookletLayout = computeAlbumBookletLayout(
                        client.right * 96.0f / dpi,
                        albumContentHeightDip(client.bottom * 96.0f / dpi),
                        albumPanelVisibility());
                    if (albumShowLyrics() && pointInAlbumRect(
                            bookletLayout.writtenDetails, x, y))
                        albumRenderer_.scrollBooklet(delta);
                    InvalidateRect(window_, nullptr, FALSE);
                    return 0;
                }
                const auto layout = computeAlbumModeLayout(
                    client.right * 96.0f / dpi,
                    albumContentHeightDip(client.bottom * 96.0f / dpi),
                    lyricsSurfaceAvailable(lyrics), compactSelection_,
                    albumPanelVisibility());
                if (pointInAlbumRect(layout.tracks, x, y)) {
                    const auto maximum = albumTrackMaximumScroll(
                        layout, state ? state->tracks.size() : 0U);
                    trackScroll_.setBounds(0.0, maximum);
                    trackScroll_.setResponseScale(
                        motionMode() == MotionMode::off ? 0.0 :
                        motionMode() == MotionMode::reduced ? 0.55 : 1.0);
                    (void)trackScroll_.addWheelImpulseAt(
                        delta, albumScrollClockSeconds());
                    trackScrollDip_ = static_cast<float>(
                        trackScroll_.position());
                    (void)frameClock_.requestImmediate();
                } else if (pointInAlbumRect(layout.lyrics, x, y)) {
                    albumRenderer_.scrollLyrics(delta);
                    // WM_PAINT is lower priority than a sustained wheel burst.
                    // Tracks already wakes the readiness-backed clock above;
                    // Lyrics must do the same or its scroll, the A4 scene and
                    // sibling realtime panels appear frozen until input stops.
                    (void)frameClock_.requestImmediate();
                }
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_CONTEXTMENU:
            if (mode_ == PlayerShellMode::album) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (point.x == -1 && point.y == -1) {
                    RECT bounds{};
                    GetWindowRect(window_, &bounds);
                    point = {(bounds.left + bounds.right) / 2,
                        (bounds.top + bounds.bottom) / 2};
                }
                showAlbumContextMenu(point);
                return 0;
            }
            break;
        case WM_PAINT:
            paintFallback();
            return 0;
        }
        return DefWindowProc(window, message, wParam, lParam);
    }

    void createChildren()
    {
        childHost_ = new service_impl_t<ShellChildHost>(this);
        createChild(kWorkspacePanelGuid, workspace_, workspaceConfig_);
        createChild(kTransportPanelGuid, transport_, transportConfig_);
        createChild(kWaveSeekPanelGuid, waveSeek_, waveSeekConfig_);
        createChild(kSpectrumPanelGuid, spectrum_, spectrumConfig_);
        createChild(kCaptionPanelGuid, caption_, captionConfig_);
        createChild(kCoverPanelGuid, cover_, coverConfig_);
        syncVisibility();
    }

    void createChild(const GUID& guid, uie::window_ptr& child,
        const std::vector<std::uint8_t>& config)
    {
        if (!uie::window::create_by_guid(guid, child) ||
            !child->is_available(childHost_)) {
            child.release();
            return;
        }
        if (!config.empty()) child->set_config_from_ptr(
            config.data(), config.size(), fb2k::noAbort);
        const ui_helpers::window_position_t position(0, 0, 1, 1);
        if (child->create_or_transfer_window(
                window_, childHost_, position) == nullptr) child.release();
    }

    void destroyChildren() noexcept
    {
        for (auto* child : {&workspace_, &transport_, &waveSeek_,
                &spectrum_, &caption_, &cover_}) {
            if (child->is_valid() && (*child)->get_wnd() != nullptr) {
                (*child)->destroy_window();
            }
            child->release();
        }
    }

    [[nodiscard]] float albumBottomInsetDip(float heightDip) const noexcept
    {
        if (albumTheaterMode()) return 32.0f + 36.0f;
        const auto maximumSpectrum = std::max(96, std::min(360,
            static_cast<int>(heightDip) - 286));
        const auto spectrum = static_cast<float>(std::clamp(
            spectrumHeightDip_, 96, maximumSpectrum));
        return 32.0f + 36.0f + spectrum + 22.0f;
    }

    [[nodiscard]] float albumContentHeightDip(float heightDip) const noexcept
    {
        return std::max(0.0f,
            heightDip - 36.0f - albumBottomInsetDip(heightDip));
    }

    void layoutChildren() const noexcept
    {
        if (window_ == nullptr) return;
        RECT client{};
        GetClientRect(window_, &client);
        const auto dpi = std::max<UINT>(1U, GetDpiForWindow(window_));
        const auto width = std::max<LONG>(0, client.right - client.left);
        const auto height = std::max<LONG>(0, client.bottom - client.top);
        const auto layout = computePlayerShellLayout(width, height, dpi,
            mode_ == PlayerShellMode::album, spectrumHeightDip_, albumTheaterMode());
        // Hidden children do not need intermediate GPU resize transactions.
        // setMode(normal) applies the latest rectangles before showing them.
        if (mode_ == PlayerShellMode::normal)
            placeChild(workspace_, layout.workspace);
        placeChild(transport_, layout.transport);
        placeChild(waveSeek_, layout.waveSeek);
        if (mode_ != PlayerShellMode::album || !albumTheaterMode()) {
            // A constrained work area can exhaust a visible band. Apply its
            // zero height too, so its previous HWND cannot cover another band.
            placeChild(spectrum_, layout.spectrum);
            placeChild(caption_, layout.caption);
        }
        if (mode_ == PlayerShellMode::normal)
            placeChild(cover_, layout.cover);
    }

    [[nodiscard]] int clampedSpectrumHeightDip(int requested) const noexcept
    {
        RECT client{};
        GetClientRect(window_, &client);
        const auto dpi = std::max<UINT>(1U, GetDpiForWindow(window_));
        const auto heightDip = static_cast<int>(
            std::max<LONG>(0, client.bottom - client.top) * 96L / dpi);
        const auto maximum = std::max(96, std::min(360, heightDip - 286));
        return std::clamp(requested, 96, maximum);
    }

    struct AmbientDeferState {
        std::uint32_t reasonMask = 0;
        bool effective = false;
    };

    [[nodiscard]] AmbientDeferState albumAmbientDeferState() const noexcept
    {
        AmbientDeferState result;
        if (deckResizeActive_) result.reasonMask |= kAmbientDeferDeckResize;
        if (lyricsPress_) result.reasonMask |= kAmbientDeferLyricsPress;
        if (trackScroll_.active()) result.reasonMask |= kAmbientDeferTrackScroll;
        const auto input = InteractionLatencyGuard::instance().snapshot();
        if (input.explicitWindow)
            result.reasonMask |= kAmbientDeferExplicitInput;
        if (input.pendingRealInput)
            result.reasonMask |= kAmbientDeferPendingInput;
        if (input.sentMessageAdvisory)
            result.reasonMask |= kAmbientSentMessageAdvisory;
        // Flags above remain diagnostic. Inertia/hover/reading a lyric
        // do not pause the simulation. Repeated queries do not move a deadline.
        result.effective = c6Admission_.shouldDefer(
            static_cast<std::uint64_t>(GetTickCount64()),
            deckResizeActive_, input.explicitWindow || input.pendingRealInput);
        return result;
    }

    [[nodiscard]] bool beginDeckResize(int yPixels) noexcept
    {
        if (window_ == nullptr) return false;
        if (mode_ == PlayerShellMode::album && albumTheaterMode()) return false;
        RECT client{};
        GetClientRect(window_, &client);
        const auto dpi = std::max<UINT>(1U, GetDpiForWindow(window_));
        const auto layout = computePlayerShellLayout(
            client.right - client.left, client.bottom - client.top, dpi,
            mode_ == PlayerShellMode::album, spectrumHeightDip_);
        const auto tolerance = std::max<LONG>(3L,
            static_cast<LONG>((6U * dpi + 48U) / 96U));
        const auto distanceToTransport = std::abs(
            static_cast<LONG>(yPixels) - layout.transport.y);
        const auto distanceToWaveSeek = std::abs(
            static_cast<LONG>(yPixels) - layout.waveSeek.y);
        if (std::min(distanceToTransport, distanceToWaveSeek) > tolerance) {
            return false;
        }
        deckResizeActive_ = true;
        deckResizeStartY_ = yPixels;
        deckResizeStartSpectrumHeightDip_ = spectrumHeightDip_;
        SetCapture(window_);
        SetCursor(LoadCursor(nullptr, IDC_SIZENS));
        return true;
    }

    void updateDeckResize(int yPixels) noexcept
    {
        const auto dpi = std::max<UINT>(1U, GetDpiForWindow(window_));
        const auto deltaDip = MulDiv(
            deckResizeStartY_ - yPixels, 96, static_cast<int>(dpi));
        const auto requested = clampedSpectrumHeightDip(
            deckResizeStartSpectrumHeightDip_ + deltaDip);
        if (requested == spectrumHeightDip_) return;
        spectrumHeightDip_ = requested;
        // The divider changes child rectangles only. The shell client and its
        // swapchain did not resize, so rebuilding the Album backing here both
        // wastes work and can discard the last accepted scene mid-drag.
        layoutChildren();
        InvalidateRect(window_, nullptr, FALSE);
    }

    static void placeChild(const uie::window_ptr& child,
        const UiPixelRect& rectangle) noexcept
    {
        placeChild(child, rectangle.x, rectangle.y,
            rectangle.width, rectangle.height);
    }

    static void placeChild(const uie::window_ptr& child,
        LONG x, LONG y, LONG width, LONG height) noexcept
    {
        if (!child.is_valid() || child->get_wnd() == nullptr) return;
        SetWindowPos(child->get_wnd(), nullptr, x, y,
            std::max<LONG>(0, width), std::max<LONG>(0, height),
            SWP_NOACTIVATE | SWP_NOZORDER);
    }

    void setAlbumArtworkActive(bool active)
    {
        if (albumArtworkActive_ == active) return;
        albumArtworkActive_ = active;
        if (active) {
            artCache_.attach(window_);
            artworkPrefetchSessionId_ = 0;
            artworkPrefetchEdge_ = 0;
            artworkPrefetchCenter_ = static_cast<std::size_t>(-1);
            // A hidden session can advance to a resident cover without a new
            // decoder completion. Restore its palette when attaching again.
            syncAlbumThemeIntent(true);
        } else {
            // Revoke this panel's main cover, variant and neighbour requests.
            // The broker retains resident pixels and other panels' interests.
            artCache_.detach();
        }
    }

    bool setMode(PlayerShellMode mode) noexcept
    {
        TelemetrySpan span(TelemetrySurface::album,101,static_cast<unsigned>(mode));
        if (mode_ == mode) return true;
        if (!completeTopology()) return false;
        mode_ = mode;
        g_playerShellMode.store(mode, std::memory_order_release);
        // Hide Album-bound children immediately, but when returning to
        // Library resize the hidden workspace first.  Showing an old retained
        // buffer before its WM_SIZE invalidation caused a stale columns frame.
        if (mode_ == PlayerShellMode::album) syncVisibility();
        setAlbumArtworkActive(mode_ == PlayerShellMode::album &&
            DisplayEnvironmentService::instance().presentationVisible(window_));
        if (mode_ != PlayerShellMode::album) {
            albumRenderer_.suspendTextPreparation();
            KillTimer(window_,0xA4F1U);a4RetryArmed_=false;
            ambientController_.shutdown();
            albumRenderer_.suspendCurated();
            ambientPumpClock_.stop();
            booklet_.closeImmediately();
            frameClock_.stop();
            KillTimer(window_, kAlbumLyricsWakeTimer);
            lyricsTimerArmed_ = false;
            lyricsTimerDeadlineMs_ = 0;
        } else {
            trackScrollDip_ = 0.0f;
            trackScroll_.setPosition(0.0);
            trackScrollSessionId_ = 0;
            syncAlbumThemeIntent(true);
        }
        layoutChildren();
        if (mode_ == PlayerShellMode::album) {
            // Workspace/Cover remained at their last Library size while
            // hidden; Album's retained surface must now match the current
            // shell client before its first visible presentation.
            albumRenderer_.resize();
            // An unchanged swap-chain still owns its one-shot readiness
            // credit. Reconcile identity after resize without retiring that
            // credit on a fast Album -> Library -> Album transition.
            frameClock_.setReadinessBinding(albumRenderer_.bindingKey());
        }
        syncVisibility();
        if (mode_ == PlayerShellMode::normal && workspace_.is_valid() &&
            workspace_->get_wnd() != nullptr) {
            RedrawWindow(workspace_->get_wnd(), nullptr, nullptr,
                RDW_INVALIDATE | RDW_ALLCHILDREN);
        }
        // A cached album/artwork need not publish any new semantic event.
        // The mode/layout revision itself must request a presentation, even
        // while paused, and survive an unavailable first readiness grant.
        (void)frameClock_.requestImmediate();
        InvalidateRect(window_, nullptr, FALSE);
        return true;
    }

    void syncVisibility() const noexcept
    {
        if (workspace_.is_valid() && workspace_->get_wnd() != nullptr) {
            ShowWindow(workspace_->get_wnd(),
                mode_ == PlayerShellMode::normal ? SW_SHOWNA : SW_HIDE);
        }
        for (const auto* child : {&transport_, &waveSeek_}) {
            if (child->is_valid() && (*child)->get_wnd() != nullptr) {
                ShowWindow((*child)->get_wnd(), SW_SHOWNA);
            }
        }
        for (const auto* child : {&spectrum_, &caption_}) {
            if (child->is_valid() && (*child)->get_wnd() != nullptr)
                ShowWindow((*child)->get_wnd(), mode_ == PlayerShellMode::album &&
                    albumTheaterMode() ? SW_HIDE : SW_SHOWNA);
        }
        if (cover_.is_valid() && cover_->get_wnd() != nullptr) {
            ShowWindow(cover_->get_wnd(),
                mode_ == PlayerShellMode::normal ? SW_SHOWNA : SW_HIDE);
        }
    }

    bool completeTopology() const noexcept
    {
        return workspace_.is_valid() && transport_.is_valid() &&
            waveSeek_.is_valid() && spectrum_.is_valid() &&
            caption_.is_valid() && cover_.is_valid();
    }

    void ensureUiFont(UINT dpi) noexcept
    {
        dpi = std::max<UINT>(1U, dpi);
        if (uiFont_ != nullptr && uiFontDpi_ == dpi) return;
        if (uiFont_ != nullptr) DeleteObject(uiFont_);
        uiFont_ = CreateFontW(
            -MulDiv(11, static_cast<int>(dpi), 96),
            0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
        uiFontDpi_ = dpi;
    }

    void invalidatePresentation() const noexcept
    {
        if (window_ == nullptr) return;
        if (mode_ == PlayerShellMode::album) {
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        const auto dpi = std::max<UINT>(1U, GetDpiForWindow(window_));
        RECT navigation{};
        GetClientRect(window_, &navigation);
        navigation.bottom = static_cast<LONG>((36U * dpi + 48U) / 96U);
        InvalidateRect(window_, &navigation, FALSE);
    }

    void redrawNavigationNow() const noexcept
    {
        if (window_ == nullptr) return;
        const auto dpi = std::max<UINT>(1U, GetDpiForWindow(window_));
        RECT navigation{};
        GetClientRect(window_, &navigation);
        navigation.bottom = static_cast<LONG>((36U * dpi + 48U) / 96U);
        // Notices arrive within WM_PAINT; preserve the narrow navigation
        // damage without recursively presenting from a posted callback.
        InvalidateRect(window_, &navigation, FALSE);
    }

    void schedulePreparationRetry() noexcept
    {
        if (!DisplayEnvironmentService::instance().presentationVisible(window_) ||
                ListenWindow::instance().occludes(window_)) {
            preparationRetryQpc_ = 0;
            frameClock_.stop();
            return;
        }
        // A semantic/input request can replace the delayed request with an
        // immediate one. Restore the same deadline rather than postponing it.
        if (frameClock_.logicalNotBeforeQpc() != preparationRetryQpc_)
            frameClock_.stop();
        (void)frameClock_.requestAtOrAfter(preparationRetryQpc_, kFrameReasonAmbient);
    }

    void paintFallback() noexcept
    {
        if(painting_)return;
        painting_=true;
        struct PaintGuard {bool& value;~PaintGuard(){value=false;}} guard{painting_};
        const bool albumPresentationVisible = mode_ == PlayerShellMode::album &&
            DisplayEnvironmentService::instance().presentationVisible(window_);
        setAlbumArtworkActive(albumPresentationVisible);
        a4::trace("album.paint.begin",++r11FrameCounter_);
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window_, &paint);
        RECT client{};
        GetClientRect(window_, &client);
        SetBkMode(dc, TRANSPARENT);
        const auto dpi = std::max<UINT>(1U, GetDpiForWindow(window_));
        ensureUiFont(dpi);
        const auto previousFont = uiFont_ != nullptr
            ? SelectObject(dc, uiFont_) : nullptr;
        const auto finishPaint = [&] {
            if (previousFont != nullptr) SelectObject(dc, previousFont);
            EndPaint(window_, &paint);
            a4::trace("album.paint.end",r11FrameCounter_);
        };
        if (preparationRetryQpc_ != 0 && albumQpcNow() < preparationRetryQpc_) {
            finishPaint();
            schedulePreparationRetry();
            return;
        }
        if (!albumRenderer_.prepareSurface(window_)) {
            // Preparation can retire the surface before an opportunity exists.
            // Do not retry again inside paint()/paintNavigation(), or let input
            // and expose messages turn resource recovery into a busy loop.
            frameClock_.setReadinessBinding(albumRenderer_.bindingKey());
            frameClock_.stop();
            const auto now = albumQpcNow();
            if (preparationRetryQpc_ == 0) {
                ambientController_.suspendPresentation(now);
                albumRenderer_.suspendCurated();
                albumRenderer_.suspendTextPreparation();
                ambientPumpClock_.stop();
                KillTimer(window_, kAlbumLyricsWakeTimer);
                lyricsTimerArmed_ = false;
                lyricsTimerDeadlineMs_ = 0;
            }
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            preparationRetryQpc_ = now + std::max<std::uint64_t>(1,
                static_cast<std::uint64_t>(frequency.QuadPart) / 4U);
            finishPaint();
            schedulePreparationRetry();
            return;
        }
        preparationRetryQpc_ = 0;
        bool readinessOpportunity = false;
        {
            const auto readiness = albumRenderer_.bindingKey();
            if (mode_ == PlayerShellMode::album) {
                frameClock_.setReadinessBinding(readiness);
                if (readiness.valid()) {
                    readinessOpportunity =
                        frameClock_.beginPresentationOpportunity();
                    if (!readinessOpportunity) {
                        finishPaint();
                        return;
                    }
                }
            } else {
                // Library owns only the navigation strip and deliberately
                // paints its first semantic revision without waiting for the
                // Album swap-chain. Keeping that waitable handle attached
                // let the immediate paint race its one-shot readiness signal;
                // the palette clock then stalled until an unrelated repaint.
                // Frame-clock pacing plus non-blocking Present keeps every colour
                // sample moving and preserves the instant Album -> Library
                // selection change.
                frameClock_.setReadinessBinding({});
            }
        }
        SetTextColor(dc, RGB(118, 132, 134));
        const auto text = mode_ == PlayerShellMode::album
            ? L"ALBUM mode has no active session"
            : L"GODENplayer Workspace unavailable";
        if (mode_ == PlayerShellMode::album) {
            const auto state = AlbumSessionService::instance().snapshot();
            const auto content = albumShowLyrics()
                ? albumContent(state)
                : std::shared_ptr<const LocalTrackContent>{};
            const auto lyrics = LyricsService::instance().snapshot();
            reconcileAlbumTrackScroll(state, content);
            const auto artworkIdentity = albumArtworkIdentity(state);
            ArtCacheStatus artworkStatus{};
            const auto artwork = albumArtwork(state,content,artworkIdentity,&artworkStatus);
            const auto nowQpc = albumQpcNow();
            const auto display =
                DisplayEnvironmentService::instance().snapshot(window_);
            const auto playback = Runtime::instance().playbackSnapshot();
            AlbumAmbientEligibility ambientEligibility;
            ambientEligibility.visible =
                albumPresentationVisible &&
                !ListenWindow::instance().occludes(window_);
            const auto textVisible = ambientEligibility.visible && state && state->state == AlbumSessionState::ready;
            albumRenderer_.setSurfaceTextDemand(textVisible);
            albumRenderer_.setTextDemand(textVisible && booklet_.openDestination());
            ambientEligibility.playing =
                playback.state == PlaybackState::playing;
            ambientEligibility.muted =
                (playback.flags & kPlaybackFlagMuted) != 0;
            ambientEligibility.hardware =
                albumRenderer_.surfaceBackend() == SurfaceBackend::hardwareDxgi;
            ambientEligibility.hasArtwork = artwork != nullptr;
            ambientEligibility.albumMode = true;
            // The booklet changes foreground/layout, not the eligibility of live audio visuals.
            ambientEligibility.bookletOpen = false;
            ambientEligibility.dawPreview = Runtime::instance().dawPreviewActive();
            ambientEligibility.outsideSession = !state ||
                state->state!=AlbumSessionState::ready || state->outsideSession;
            ambientEligibility.reducedMotion = motionMode() != MotionMode::normal;
            ambientEligibility.mode = albumAmbientMode();
            const bool isCurated=curated::selectedStyle(static_cast<unsigned>(a4::style()));
            albumRenderer_.setCuratedActivity(isCurated && ambientEligibility.visible &&
                (ambientEligibility.playing || playback.state==PlaybackState::paused) && !ambientEligibility.muted && ambientEligibility.hardware &&
                !ambientEligibility.bookletOpen && !ambientEligibility.dawPreview &&
                !ambientEligibility.outsideSession && !ambientEligibility.reducedMotion &&
                ambientEligibility.mode!=AlbumAmbientMode::off && !qaAlbumAmbientWorkerOnly());
            albumRenderer_.setCuratedPlaying(ambientEligibility.playing);
            ambientController_.setUserPaused(playback.state==PlaybackState::paused,nowQpc);
            if(isCurated) {
                ambientEligibility.hasArtwork=true;
                // Original uses the fixed simulation clock and does not request band analysis.
                if(curated::mode()==curated::Mode::original)ambientEligibility.mode=AlbumAmbientMode::off;
            }
            ambientController_.sync(ambientEligibility,
                Runtime::instance().streamGeneration(), display.refreshHz(), nowQpc);
            if (ambientController_.wantsPump()) {
                (void)ambientPumpClock_.requestAtOrAfter(
                    ambientController_.nextPumpQpc(nowQpc),
                    kFrameReasonAudioPump);
            }
            const auto workerOnly = qaAlbumAmbientWorkerOnly();
            const auto defer = albumAmbientDeferState();
            if (defer.effective) {
                ambientController_.deferTexture(nowQpc, defer.reasonMask);
            } else {
                ambientController_.resumeTexture(nowQpc);
                ambientController_.observeTexturePolicy(defer.reasonMask);
            }
            const auto latencySensitive = defer.effective;
            albumRenderer_.setCuratedDefer(latencySensitive);
            const auto ambientWasDue = !workerOnly && !latencySensitive &&
                (isCurated?albumRenderer_.curatedFrameDue():ambientController_.textureDue(nowQpc));
            const auto ambientPose = ambientController_.sample(nowQpc);
            const auto playbackSeconds = state && state->outsideSession
                ? -1.0 : Runtime::instance().presentationPlaybackPosition();
            const auto personalNote = albumShowLyrics()
                ? albumPersonalNote(state)
                : std::pair<std::wstring, std::uint64_t>{};
            if((!state || state->state!=AlbumSessionState::ready) && booklet_.openDestination())
                booklet_.closeImmediately();
            updateAlbumFocusRect();
            const auto outcome = albumRenderer_.paint(
                    window_, dc, state, content, lyrics, artwork, artworkIdentity,
                    playbackSeconds,
                    trackScrollDip_, compactSelection_, albumPanelVisibility(),
                    booklet_.sample(AlbumBookletModel::Clock::now()),
                    personalNote.first, personalNote.second, ambientPose,
                    ambientWasDue, 36.0f,
                    albumBottomInsetDip(client.bottom * 96.0f / dpi),artworkStatus);
            {
                const auto animation = albumRenderer_.animationActive() ||
                    booklet_.active(AlbumBookletModel::Clock::now()) ||
                    trackScroll_.active() ||
                    ThemeService::instance().transitionActive();
                const auto frameResult = outcome.state;
                const auto accepted = frameResult == FrameResult::accepted;
                if (accepted) ++qaSurfaceAccepted_;
                if (albumRenderer_.lastAmbientTextureProduced())
                    c6Admission_.produced(static_cast<std::uint64_t>(GetTickCount64()));
                frameClock_.setLatencyCritical(animation);
                if (ambientWasDue) {
                    ambientController_.texturePresented(nowQpc,
                        albumRenderer_.lastAmbientTextureProduced(), accepted,
                        ambientPose.publicationRevision);
                }
                if (albumRenderer_.ambientFallbackRequired()) {
                    ambientController_.renderUnavailable(nowQpc);
                    const auto retry=albumRenderer_.ambientRetryMilliseconds();
                    if(retry&&!a4RetryArmed_){
                        a4RetryArmed_=SetTimer(window_,0xA4F1U,retry,nullptr)!=0;
                    }
                }else if(a4RetryArmed_){KillTimer(window_,0xA4F1U);a4RetryArmed_=false;}
                WindowFrameClock::reportPresentOutcome(
                    frameResult == FrameResult::notReady);
                // EndDraw can retire a lost surface. Rebind only after the
                // attempt so no wait observes a handle closed by reset().
                frameClock_.setReadinessBinding(
                    albumRenderer_.bindingKey());
                if (readinessOpportunity) {
                    const auto ambientAnimation = !workerOnly &&
                        (isCurated?albumRenderer_.curatedAnimationActive():ambientController_.animationActive(nowQpc));
                    frameClock_.completePresentationOpportunity(
                        animation || ambientAnimation || !accepted,
                        !animation && ambientAnimation
                            ? (isCurated?albumRenderer_.curatedDeadlineQpc():ambientController_.nextTextureQpc()) : 0,
                        !animation && ambientAnimation
                            ? kFrameReasonAmbient : 0);
                } else if (animation) {
                    (void)frameClock_.requestImmediate();
                } else if (!workerOnly &&
                        (isCurated?albumRenderer_.curatedAnimationActive():ambientController_.animationActive(nowQpc))) {
                    (void)frameClock_.requestAtOrAfter(
                        (isCurated?albumRenderer_.curatedDeadlineQpc():ambientController_.nextTextureQpc()),
                        kFrameReasonAmbient);
                } else if (!accepted) {
                    (void)frameClock_.requestImmediate();
                } else {
                    frameClock_.stop();
                }
                updateLyricsTimer(playbackSeconds);
                finishPaint();
                return;
            }
        }
        FillRect(dc, &client,
            static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        const auto navigationOutcome = albumRenderer_.paintNavigation(
            window_, dc, mode_ == PlayerShellMode::album, 36.0f);
        ++navigationThemePaints_;
        DrawTextW(dc, text, -1, &client, DT_CENTER | DT_VCENTER |
            DT_SINGLELINE | DT_END_ELLIPSIS);
        const auto navigationAnimation =
            ThemeService::instance().transitionActive();
        const auto navigationFrame = navigationOutcome.state;
        if (navigationFrame == FrameResult::accepted) ++qaSurfaceAccepted_;
        // Library's partial navigation frame is intentionally frame-clock
        // paced. Its non-blocking Present result below supplies backpressure without
        // coupling colour interpolation to Album's one-shot readiness event.
        frameClock_.setReadinessBinding({});
        if (navigationFrame != FrameResult::accepted) {
            (void)frameClock_.requestImmediate();
        } else if (navigationAnimation) {
            // Only pigment changes here. Audio/hover retain their own display
            // clocks, and new input can still request an immediate frame.
            (void)frameClock_.requestAtOrAfter(
                ThemeService::instance().nextSampleQpc(), kFrameReasonAmbient);
        } else {
            frameClock_.stop();
        }
        finishPaint();
    }

    bool beginRatingPress(int xp,int yp) noexcept
    {
        if(mode_!=PlayerShellMode::album||!albumShowTracks()||
            booklet_.openDestination()||
            booklet_.sample(AlbumBookletModel::Clock::now())>.001f)return false;
        const auto dpi=std::max<UINT>(1U,GetDpiForWindow(window_));
        const auto hit=albumRenderer_.ratingHitAt(xp*96.0f/dpi,yp*96.0f/dpi-36.0f);
        if(!hit)return false;
        ratingPress_=hit;ratingPressPoint_={xp,yp};ratingPressMoved_=false;
        GetClientRect(window_,&ratingPressClient_);SetCapture(window_);return true;
    }
    bool showVisualPreferences(int xp,int yp)
    {
        if(mode_!=PlayerShellMode::album)return false;
        RECT client{};GetClientRect(window_,&client);
        const auto dpi=std::max<UINT>(1U,GetDpiForWindow(window_));
        const auto width=client.right*96.0f/dpi;
        const auto height=albumContentHeightDip(client.bottom*96.0f/dpi);
        const auto x=xp*96.0f/dpi,y=yp*96.0f/dpi-36.0f;
        const auto focused=booklet_.openDestination()||
            booklet_.sample(AlbumBookletModel::Clock::now())>.001f;
        D2D1_RECT_F visual{};
        if(focused)visual=computeAlbumBookletLayout(
            width,height,albumPanelVisibility()).visualMenu;
        else visual=albumRenderer_.visualMenuRect(computeAlbumModeLayout(
            width,height,true,compactSelection_,albumPanelVisibility()));
        if(!pointInAlbumRect(visual,x,y))return false;
        POINT point{xp,yp};ClientToScreen(window_,&point);
        if(collection::showVisualPicker(window_,point)){
            albumRenderer_.retryVisualStyle();ambientController_.retryRendering();
            (void)frameClock_.requestImmediate();InvalidateRect(window_,nullptr,FALSE);
        }
        return true;
    }

    bool albumLyricsRetryAvailable() const {
        const auto state=AlbumSessionService::instance().snapshot();
        const auto lyrics=LyricsService::instance().snapshot();
        return state && state->state==AlbumSessionState::ready && !state->outsideSession &&
            state->currentInSession<state->tracks.size() && lyrics && lyrics->state==LyricsState::failed &&
            !lyrics->document && !state->tracks[state->currentInSession].entityId.empty() &&
            state->tracks[state->currentInSession].entityId==lyrics->request.recording.entityId;
    }
    void followOrRetryAlbumLyrics(){
        if(albumLyricsRetryAvailable())LyricsService::instance().retryCurrent();
        else albumRenderer_.followLyrics();
    }
    // Fixed MSAA child IDs survive responsive layout changes. Invisible
    // controls stay in the tree but are skipped by Tab and accessibility navigation.
    std::array<D2D1_RECT_F,15> albumFocusRects() const {
        std::array<D2D1_RECT_F,15> r{};RECT client{};GetClientRect(window_,&client);
        const auto dpi=std::max<UINT>(1U,GetDpiForWindow(window_));
        const auto w=client.right*96.f/dpi,h=albumContentHeightDip(client.bottom*96.f/dpi);
        r[0]={0,-36,std::min(108.f,w),0};r[1]={108,-36,std::min(216.f,w),0};
        if(mode_!=PlayerShellMode::album)return r;
        const auto state=AlbumSessionService::instance().snapshot();
        const auto l=computeAlbumModeLayout(w,h,true,compactSelection_,albumPanelVisibility());
        if(!state || state->state!=AlbumSessionState::ready){r[13]=l.chooseAlbum;return r;}
        if(booklet_.openDestination()){
            const auto b=computeAlbumBookletLayout(w,h,albumPanelVisibility());
            r[3]=b.tracksToggle;r[4]=b.lyricsToggle;r[5]=b.panelsMenu;r[6]=b.visualMenu;r[14]=b.close;
            return r;
        }
        r[2]=l.cover;r[3]=l.tracksToggle;r[4]=l.lyricsToggle;r[5]=l.panelsMenu;r[6]=l.visualMenu;
        r[7]=l.follow;if(state->outsideSession)r[8]=l.followCurrent;
        r[9]=l.lyricsTab;r[10]=l.tracksTab;r[11]=l.tracks;r[12]=l.lyrics;
        return r;
    }
    std::vector<AccessibleSurfaceNode> albumAccessibleNodes() const {
        const auto rects=albumFocusRects();const auto state=AlbumSessionService::instance().snapshot();
        const wchar_t* names[]{L"Library",L"Album",L"Open album booklet",L"Show tracks",L"Show lyrics",L"Panels and artwork",L"Choose visual",L"Follow lyrics",L"Follow playing album",L"Lyrics tab",L"Tracks tab",L"Album tracks",L"Album lyrics",L"Album recovery",L"Close booklet"};
        std::vector<AccessibleSurfaceNode> result;result.reserve(15);
        const auto dpi=std::max<UINT>(1U,GetDpiForWindow(window_));POINT origin{};ClientToScreen(window_,&origin);
        for(std::size_t i=0;i<15;++i){
            AccessibleSurfaceNode n;n.name=names[i];const auto r=rects[i];
            n.screen={origin.x+LONG(r.left*dpi/96.f),origin.y+LONG((r.top+36)*dpi/96.f),origin.x+LONG(r.right*dpi/96.f),origin.y+LONG((r.bottom+36)*dpi/96.f)};
            if(r.right<=r.left||r.bottom<=r.top||!IsWindowVisible(window_))n.state|=STATE_SYSTEM_INVISIBLE;
            if(i==3||i==4){n.role=ROLE_SYSTEM_CHECKBUTTON;if(i==3?albumShowTracks():albumShowLyrics())n.state|=STATE_SYSTEM_CHECKED;}
            if(i==0||i==1||i==9||i==10)n.role=ROLE_SYSTEM_PAGETAB;
            if(i==11){n.role=ROLE_SYSTEM_LIST;if(state && !state->tracks.empty()){
                const auto row=std::min(albumTrackFocus_,state->tracks.size()-1);
                n.value=state->tracks[row].title+L", "+std::to_wstring(row+1)+L" of "+std::to_wstring(state->tracks.size());}}
            if(i==7 && albumLyricsRetryAvailable())n.name=L"Retry lyrics";
            if(i==12){n.role=ROLE_SYSTEM_LIST;n.value=LyricsService::instance().statusText();}
            if(i==13)n.name=state && state->state==AlbumSessionState::error?L"Retry album":L"Choose album in library";
            result.push_back(std::move(n));
        }
        return result;
    }
    void focusAlbumControl(long id){
        if(id<1||id>15)return;const auto r=albumFocusRects()[id-1];
        if(r.right<=r.left||r.bottom<=r.top)return;
        albumFocus_=id;SetFocus(window_);updateAlbumFocusRect();
        NotifyWinEvent(EVENT_OBJECT_FOCUS,window_,OBJID_CLIENT,id);
        InvalidateRect(window_,nullptr,FALSE);
    }
    void updateAlbumFocusRect(){
        const auto state=AlbumSessionService::instance().snapshot();
        if(state && state->sessionId!=albumFocusSession_){
            albumFocusSession_=state->sessionId;
            albumTrackFocus_=state->currentInSession<state->tracks.size()?state->currentInSession:0;
        }
        albumRenderer_.setFocusedTrack(GetFocus()==window_ && albumFocus_==12?std::optional<std::size_t>(albumTrackFocus_):std::nullopt);
        const auto rects=albumFocusRects();
        albumRenderer_.setFocusRect(GetFocus()==window_ && albumFocus_>0 && albumFocus_<=15?rects[albumFocus_-1]:D2D1_RECT_F{});
    }
    void activateAlbumControl(long id){
        const auto state=AlbumSessionService::instance().snapshot();
        switch(id){
        case 1:(void)setMode(PlayerShellMode::normal);break;
        case 2:(void)AlbumSessionService::instance().enterFromHostContext();(void)setMode(PlayerShellMode::album);break;
        case 3:if(state && state->state==AlbumSessionState::ready)setBookletOpen(true);break;
        case 4:applyAlbumVisibility(!albumShowTracks(),albumShowLyrics());break;
        case 5:applyAlbumVisibility(albumShowTracks(),!albumShowLyrics());break;
        case 6:case 7:{const auto r=albumFocusRects()[id-1];const auto dpi=std::max<UINT>(1,GetDpiForWindow(window_));
            const int x=int((r.left+r.right)*.5f*dpi/96),y=int(((r.top+r.bottom)*.5f+36)*dpi/96);
            if(id==6)showAlbumPanelsMenu(x,y);else (void)showVisualPreferences(x,y);break;}
        case 8:case 13:followOrRetryAlbumLyrics();break;
        case 9:(void)AlbumSessionService::instance().enterFromHostContext();break;
        case 10:compactSelection_=AlbumPanelKind::lyrics;break;
        case 11:compactSelection_=AlbumPanelKind::tracks;break;
        case 12:if(state && state->state==AlbumSessionState::ready && albumTrackFocus_<state->tracks.size())(void)AlbumSessionService::instance().playTrack(albumTrackFocus_);break;
        case 14:if(state && state->state==AlbumSessionState::error)(void)AlbumSessionService::instance().retry();else (void)setMode(PlayerShellMode::normal);break;
        case 15:setBookletOpen(false);focusAlbumControl(3);break;
        }
        updateAlbumFocusRect();(void)frameClock_.requestImmediate();InvalidateRect(window_,nullptr,FALSE);
    }
    bool albumKey(UINT key){
        GUITHREADINFO gui{};gui.cbSize=sizeof(gui);
        if(GetGUIThreadInfo(GetCurrentThreadId(),&gui) &&
            (gui.flags&(GUI_INMENUMODE|GUI_POPUPMENUMODE|GUI_SYSTEMMENUMODE)))return false;
        if(key==VK_TAB){const auto rects=albumFocusRects();const int delta=GetKeyState(VK_SHIFT)<0?-1:1;
            auto id=albumFocus_==0&&delta<0?1:albumFocus_;for(unsigned n=0;n<15;++n){id=(id-1+delta+15)%15+1;const auto r=rects[id-1];
                if(r.right>r.left&&r.bottom>r.top){focusAlbumControl(id);break;}}return true;}
        if((key==VK_RETURN||key==VK_SPACE)&&albumFocus_>0){activateAlbumControl(albumFocus_);return true;}
        if(albumFocus_==12 && (key==VK_UP||key==VK_DOWN||key==VK_HOME||key==VK_END)){
            const auto s=AlbumSessionService::instance().snapshot();if(!s||s->tracks.empty())return true;
            albumTrackFocus_=std::min(albumTrackFocus_,s->tracks.size()-1);
            if(key==VK_HOME)albumTrackFocus_=0;else if(key==VK_END)albumTrackFocus_=s->tracks.size()-1;
            else if(key==VK_UP&&albumTrackFocus_>0)--albumTrackFocus_;else if(key==VK_DOWN&&albumTrackFocus_+1<s->tracks.size())++albumTrackFocus_;
            const auto r=albumFocusRects()[11];const auto viewport=std::max(0.f,r.bottom-r.top-38);
            const auto top=float(albumTrackFocus_)*28;
            const auto wanted=top<trackScrollDip_?top:top+28>trackScrollDip_+viewport?top+28-viewport:trackScrollDip_;
            trackScroll_.setPosition(std::max(0.f,wanted));trackScrollDip_=float(trackScroll_.position());
            NotifyWinEvent(EVENT_OBJECT_VALUECHANGE,window_,OBJID_CLIENT,12);
            InvalidateRect(window_,nullptr,FALSE);return true;
        }
        if(albumFocus_==13 && (key==VK_UP||key==VK_DOWN||key==VK_PRIOR||key==VK_NEXT)){
            const auto r=albumFocusRects()[12];const float distance=(key==VK_UP||key==VK_DOWN)?36.f:(r.bottom-r.top)*.8f;
            albumRenderer_.scrollLyrics((key==VK_UP||key==VK_PRIOR)?-distance:distance);
            (void)frameClock_.requestImmediate();InvalidateRect(window_,nullptr,FALSE);return true;
        }
        return false;
    }
    std::uint64_t staleAlbumMenuActions_=0;
    UINT lastAlbumMenuSelection_=0;
    std::uint64_t albumKeyboardMessages_=0;
    std::unique_ptr<AlbumKeyboardFilter> albumKeyboardFilter_;
    AccessibleSurface* albumAccessible_=nullptr;
    long albumFocus_=0;
    std::size_t albumTrackFocus_=0;
    std::uint64_t albumFocusSession_=0;

    enum class AlbumControl : std::uint8_t {
        none,
        tracks,
        lyrics,
        panels,
    };

    AlbumControl albumControlAt(int xPixels, int yPixels) noexcept
    {
        if (mode_ != PlayerShellMode::album || window_ == nullptr)
            return AlbumControl::none;
        RECT client{};
        GetClientRect(window_, &client);
        const auto dpi = std::max<UINT>(1U, GetDpiForWindow(window_));
        const auto x = xPixels * 96.0f / dpi;
        const auto y = yPixels * 96.0f / dpi - 36.0f;
        if (y < 0.0f) return AlbumControl::none;
        const auto width = (client.right - client.left) * 96.0f / dpi;
        const auto height = albumContentHeightDip(
            (client.bottom - client.top) * 96.0f / dpi);
        const auto visibility = albumPanelVisibility();
        const auto focused = booklet_.openDestination() ||
            booklet_.sample(AlbumBookletModel::Clock::now()) > .001f;
        D2D1_RECT_F tracks{}, lyrics{}, panels{};
        if (focused) {
            const auto layout = computeAlbumBookletLayout(
                width, height, visibility);
            tracks = layout.tracksToggle;
            lyrics = layout.lyricsToggle;
            panels = layout.panelsMenu;
        } else {
            const auto layout = computeAlbumModeLayout(width, height, true,
                compactSelection_, visibility);
            tracks = layout.tracksToggle;
            lyrics = layout.lyricsToggle;
            panels = layout.panelsMenu;
        }
        if (pointInAlbumRect(tracks, x, y)) return AlbumControl::tracks;
        if (pointInAlbumRect(lyrics, x, y)) return AlbumControl::lyrics;
        if (pointInAlbumRect(panels, x, y)) return AlbumControl::panels;
        return AlbumControl::none;
    }

    bool beginAlbumControlPress(int xPixels, int yPixels) noexcept
    {
        const auto control = albumControlAt(xPixels, yPixels);
        if (control == AlbumControl::none) return false;
        albumControlPress_ = control;
        GetClientRect(window_, &albumControlPressClient_);
        SetCapture(window_);
        return true;
    }

    void applyAlbumVisibility(bool tracks, bool lyrics) noexcept
    {
        const bool tracksChanged = setAlbumShowTracks(tracks);
        const bool lyricsChanged = setAlbumShowLyrics(lyrics);
        if (!tracksChanged && !lyricsChanged) return;
        if (!tracks) {
            ratingPress_.reset();
            trackScroll_.setPosition(trackScroll_.position());
        }
        if (!lyrics) {
            KillTimer(window_, kAlbumLyricsWakeTimer);
            lyricsTimerArmed_ = false;
            lyricsTimerDeadlineMs_ = 0;
            lyricsPress_.reset();
            albumRenderer_.suspendLyricsScroll();
        }
        if (compactSelection_ == AlbumPanelKind::tracks && !tracks && lyrics)
            compactSelection_ = AlbumPanelKind::lyrics;
        if (compactSelection_ == AlbumPanelKind::lyrics && !lyrics && tracks)
            compactSelection_ = AlbumPanelKind::tracks;
        (void)frameClock_.requestImmediate();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void showAlbumPanelsMenu(int xPixels, int yPixels)
    {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING | (albumShowTracks() ? MF_CHECKED : 0),
            1U, L"Tracks");
        AppendMenuW(menu, MF_STRING | (albumShowLyrics() ? MF_CHECKED : 0),
            2U, L"Lyrics");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu,MF_STRING | (albumTheaterMode()?MF_CHECKED:0),7U,L"Modo teatro");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu,MF_STRING | (albumShowArtwork()?MF_CHECKED:0),3U,L"Capa do álbum");
        AppendMenuW(menu,MF_STRING,4U,L"Capa à esquerda");
        AppendMenuW(menu,MF_STRING,5U,L"Capa centralizada");
        AppendMenuW(menu,MF_STRING,6U,L"Capa à direita");
        CheckMenuRadioItem(menu,4U,6U,4U+static_cast<unsigned>(albumArtworkAlignment()),MF_BYCOMMAND);
        POINT point{xPixels, yPixels};
        ClientToScreen(window_, &point);
        const auto selected = TrackPopupMenu(menu,
            TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN,
            point.x, point.y, 0, window_, nullptr);
        DestroyMenu(menu);
        if (selected == 1U)
            applyAlbumVisibility(!albumShowTracks(), albumShowLyrics());
        else if (selected == 2U)
            applyAlbumVisibility(albumShowTracks(), !albumShowLyrics());
        else if (selected == 7U)
            applyAlbumTheaterMode(!albumTheaterMode());
        else if(selected>=3U&&selected<=6U){
            if(selected==3U)(void)setAlbumShowArtwork(!albumShowArtwork());
            else (void)setAlbumArtworkAlignment(static_cast<goden::c11::ArtworkAlignment>(selected-4U));
            reconcileAlbumTrackScroll(AlbumSessionService::instance().snapshot());
            (void)frameClock_.requestImmediate();
            InvalidateRect(window_,nullptr,FALSE);
        }
    }

    void finishAlbumControlPress(int xPixels, int yPixels)
    {
        const auto pressed = albumControlPress_;
        albumControlPress_ = AlbumControl::none;
        RECT current{};
        GetClientRect(window_, &current);
        if (GetCapture() == window_) ReleaseCapture();
        if (!EqualRect(&current, &albumControlPressClient_) ||
            albumControlAt(xPixels, yPixels) != pressed) return;
        if (pressed == AlbumControl::tracks)
            applyAlbumVisibility(!albumShowTracks(), albumShowLyrics());
        else if (pressed == AlbumControl::lyrics)
            applyAlbumVisibility(albumShowTracks(), !albumShowLyrics());
        else if (pressed == AlbumControl::panels)
            showAlbumPanelsMenu(xPixels, yPixels);
    }

    bool beginLyricsPress(int xPixels, int yPixels) noexcept
    {
        if (mode_ != PlayerShellMode::album || !albumShowLyrics() ||
            window_ == nullptr ||
            booklet_.openDestination() || booklet_.sample(AlbumBookletModel::Clock::now())>.001f) return false;
        const auto dpi = std::max<UINT>(1U, GetDpiForWindow(window_));
        const auto xDip = xPixels * 96.0f / dpi;
        const auto yDip = yPixels * 96.0f / dpi;
        if (yDip < 36.0f) return false;
        RECT client{};
        GetClientRect(window_, &client);
        const auto lyrics = LyricsService::instance().snapshot();
        const auto layout = computeAlbumModeLayout(
            (client.right - client.left) * 96.0f / dpi,
            albumContentHeightDip((client.bottom - client.top) * 96.0f / dpi),
            lyricsSurfaceAvailable(lyrics), compactSelection_,
            albumPanelVisibility());
        const auto token = albumRenderer_.lyricsHitAt(
            layout, xDip, yDip - 36.0f);
        if (!token || !albumRenderer_.validateLyricsHit(*token)) return false;
        lyricsPress_ = token;
        lyricsPressPoint_ = {xPixels, yPixels};
        lyricsPressClient_ = {
            client.right - client.left, client.bottom - client.top};
        lyricsPressMoved_ = false;
        SetCapture(window_);
        return true;
    }

    void applyAlbumTheaterMode(bool enabled) noexcept
    {
        if (!setAlbumTheaterMode(enabled)) return;
        if (deckResizeActive_) {
            deckResizeActive_ = false;
            if (GetCapture() == window_) ReleaseCapture();
        }
        // Hide before layout so the spectrum stops its own presentation loop.
        // Keep its retained size/configuration for the next visible layout.
        if (enabled && mode_ == PlayerShellMode::album) syncVisibility();
        layoutChildren();
        syncVisibility();
        if (mode_ == PlayerShellMode::album) albumRenderer_.resize();
        reconcileAlbumTrackScroll(AlbumSessionService::instance().snapshot());
        updateAlbumFocusRect();
        (void)frameClock_.requestImmediate();
        InvalidateRect(window_, nullptr, FALSE);
    }

    struct TrackPress {
        std::uint64_t session=0,composition=0;
        metadb_handle_ptr handle;std::size_t row=0;float scroll=0;RECT client{};
    };
    std::optional<TrackPress> trackPressAt(int x,int y) const
    {
        if(mode_!=PlayerShellMode::album||!albumShowTracks()||booklet_.openDestination()||
            booklet_.sample(AlbumBookletModel::Clock::now())>.001f)return {};
        const auto state=AlbumSessionService::instance().snapshot();
        if(!state||state->state!=AlbumSessionState::ready)return {};
        RECT r{};GetClientRect(window_,&r);const auto dpi=std::max<UINT>(1U,GetDpiForWindow(window_));
        const auto lyrics=LyricsService::instance().snapshot();
        const auto layout=computeAlbumModeLayout((r.right-r.left)*96.f/dpi,
            albumContentHeightDip((r.bottom-r.top)*96.f/dpi),lyricsSurfaceAvailable(lyrics),compactSelection_,albumPanelVisibility());
        const auto row=albumTrackAtPoint(layout,x*96.f/dpi,y*96.f/dpi-36.f,state->tracks.size(),trackScrollDip_);
        if(!row)return {};
        return TrackPress{state->sessionId,state->compositionRevision,state->tracks[*row].handle,*row,trackScrollDip_,r};
    }

    mutable c6::InteractionAdmission c6Admission_;
    bool painting_=false,a4RetryArmed_=false,r11TrackingMouse_=false;
    inline static thread_local HHOOK albumWheelHook_=nullptr;
    inline static thread_local PlayerShellPanel* albumWheelHookOwner_=nullptr;
    std::uint64_t r11FrameCounter_=0;
    std::uint64_t qaSurfaceAccepted_ = 0;
    std::uint64_t preparationRetryQpc_ = 0;
    std::optional<a4::RatingHit> ratingPress_;
    std::optional<TrackPress> trackPress_;
    POINT ratingPressPoint_{};
    RECT ratingPressClient_{};
    bool ratingPressMoved_=false;
    HWND window_ = nullptr;
    HFONT uiFont_ = nullptr;
    UINT uiFontDpi_ = 0;
    PlayerShellMode mode_ = PlayerShellMode::normal;
    uie::window_ptr workspace_;
    uie::window_ptr transport_;
    uie::window_ptr waveSeek_;
    uie::window_ptr spectrum_;
    uie::window_ptr caption_;
    uie::window_ptr cover_;
    uie::window_host_ptr childHost_;
    std::vector<std::uint8_t> workspaceConfig_;
    std::vector<std::uint8_t> transportConfig_;
    std::vector<std::uint8_t> waveSeekConfig_;
    std::vector<std::uint8_t> spectrumConfig_;
    std::vector<std::uint8_t> captionConfig_;
    std::vector<std::uint8_t> coverConfig_;
    int spectrumHeightDip_ = 152;
    bool deckResizeActive_ = false;
    int deckResizeStartY_ = 0;
    int deckResizeStartSpectrumHeightDip_ = 152;
    std::optional<LyricsHitToken> lyricsPress_;
    POINT lyricsPressPoint_{};
    SIZE lyricsPressClient_{};
    bool lyricsPressMoved_ = false;
    AlbumControl albumControlPress_ = AlbumControl::none;
    RECT albumControlPressClient_{};
    mutable ArtCache artCache_;
    bool albumArtworkActive_ = false;
    mutable LocalContentCache contentCache_;
    mutable AlbumModeRenderer albumRenderer_;
    curated::Diagnostics curatedQaSnapshot_{};
    std::uint64_t curatedQaSnapshotVersion_=0;
    bool curatedQaSnapshotValid_=false;
    bool qaFastModePending_=false;
    LRESULT qaFastModeResult_=-1;
    mutable float trackScrollDip_ = 0.0f;
    mutable nekorox::ScrollController trackScroll_{};
    mutable std::uint64_t trackScrollSessionId_ = 0;
    mutable std::uint64_t artworkPrefetchSessionId_ = 0;
    mutable std::uint32_t artworkPrefetchEdge_ = 0;
    mutable ArtAnalysisFlags artworkPrefetchAnalysis_ = ArtAnalysisFlags::legacy;
    mutable std::size_t artworkPrefetchCenter_ =
        static_cast<std::size_t>(-1);
    std::wstring albumThemeIdentity_;
    AlbumPanelKind compactSelection_ = AlbumPanelKind::lyrics;
    mutable AlbumBookletModel booklet_;
    mutable WindowFrameClock frameClock_;
    mutable WindowFrameClock ambientPumpClock_;
    mutable AlbumAmbientController ambientController_;
    std::uint64_t navigationThemePaints_ = 0;
    bool lyricsTimerArmed_ = false;
    ULONGLONG lyricsTimerDeadlineMs_ = 0;

    [[nodiscard]] static AlbumPanelVisibility albumPanelVisibility() noexcept
    {
        return {albumShowTracks(),albumShowLyrics(),albumShowArtwork(),albumArtworkAlignment()};
    }

    void updateLyricsTimer(double playbackSeconds) noexcept
    {
        std::optional<std::uint32_t> wake;
        if (mode_ == PlayerShellMode::album && albumShowLyrics() &&
                DisplayEnvironmentService::instance().presentationVisible(window_) &&
                Runtime::instance().playbackSnapshot().state ==
                PlaybackState::playing) {
            wake = albumRenderer_.nextLyricsWakeMilliseconds(playbackSeconds);
        }
        if (!wake) {
            if (lyricsTimerArmed_) KillTimer(window_, kAlbumLyricsWakeTimer);
            lyricsTimerArmed_ = false;
            lyricsTimerDeadlineMs_ = 0;
            return;
        }
        const auto deadline = GetTickCount64() + *wake;
        const auto delta = deadline > lyricsTimerDeadlineMs_
            ? deadline - lyricsTimerDeadlineMs_
            : lyricsTimerDeadlineMs_ - deadline;
        // Ambient-only paints observe essentially the same absolute cue
        // deadline. Preserve the existing timer instead of kill/rearming it.
        if (lyricsTimerArmed_ && delta <= 50U) return;
        if (lyricsTimerArmed_) KillTimer(window_, kAlbumLyricsWakeTimer);
        if (SetTimer(window_, kAlbumLyricsWakeTimer,
                std::max<std::uint32_t>(1U, *wake), nullptr) != 0) {
            lyricsTimerArmed_ = true;
            lyricsTimerDeadlineMs_ = deadline;
        } else {
            lyricsTimerArmed_ = false;
            lyricsTimerDeadlineMs_ = 0;
        }
    }

    void setBookletOpen(bool open) noexcept
    {
        if (!open) albumRenderer_.setTextDemand(false);
        const auto mode = motionMode();
        const auto duration = mode == MotionMode::normal
            ? std::chrono::milliseconds(320)
            : mode == MotionMode::reduced
                ? std::chrono::milliseconds(120)
                : std::chrono::milliseconds(0);
        booklet_.setOpen(open, AlbumBookletModel::Clock::now(), duration);
        (void)frameClock_.requestImmediate();
        InvalidateRect(window_, nullptr, FALSE);
    }

    std::shared_ptr<const LocalTrackContent> albumContent(
        const std::shared_ptr<const AlbumSessionSnapshot>& state) const
    {
        if (!state || state->tracks.empty()) return {};
        auto index = state->currentInSession;
        if (index >= state->tracks.size()) index = 0;
        const auto& track = state->tracks[index];
        return contentCache_.getOrRequest(track.handle, track.artist,
            state->album, track.title);
    }

    void reconcileAlbumTrackScroll(
        const std::shared_ptr<const AlbumSessionSnapshot>& state,
        const std::shared_ptr<const LocalTrackContent>& content = {}) const
    {
        const auto sessionId = state ? state->sessionId : 0U;
        if (sessionId != trackScrollSessionId_) {
            trackScrollSessionId_ = sessionId;
            trackScrollDip_ = 0.0f;
            trackScroll_.setPosition(0.0);
        }
        if (window_ == nullptr) return;
        RECT client{};
        GetClientRect(window_, &client);
        const auto dpi = std::max<UINT>(1U, GetDpiForWindow(window_));
        (void)content;
        const auto lyrics = LyricsService::instance().snapshot();
        const auto layout = computeAlbumModeLayout(
            (client.right - client.left) * 96.0f / dpi,
            albumContentHeightDip(
                (client.bottom - client.top) * 96.0f / dpi),
            lyricsSurfaceAvailable(lyrics),
            compactSelection_, albumPanelVisibility());
        const auto maximum = albumTrackMaximumScroll(
            layout, state ? state->tracks.size() : 0U);
        trackScroll_.setBounds(0.0, maximum);
        (void)trackScroll_.advanceTo(albumScrollClockSeconds());
        trackScrollDip_ = static_cast<float>(trackScroll_.position());
    }

    std::wstring albumArtworkIdentity(
        const std::shared_ptr<const AlbumSessionSnapshot>& state) const
    {
        if (!state) return {};
        if (state->tracks.empty()) return artworkTrackIdentity(state->anchor);
        auto index = state->currentInSession;
        if (index >= state->tracks.size()) index = 0;
        return artworkTrackIdentity(state->tracks[index].handle);
    }

    std::shared_ptr<const ArtPixels> albumArtwork(
        const std::shared_ptr<const AlbumSessionSnapshot>& state,
        const std::shared_ptr<const LocalTrackContent>& content,
        const std::wstring& artworkIdentity,ArtCacheStatus* resolvedStatus=nullptr) const
    {
        if(resolvedStatus)*resolvedStatus=ArtCacheStatus::missing;
        if (!albumArtworkActive_) return {};
        if (!state || state->tracks.empty() || artworkIdentity.empty()) return {};
        (void)content;
        auto index = state->currentInSession;
        if (index >= state->tracks.size()) index = 0;
        RECT client{};
        GetClientRect(window_, &client);
        const auto dpi = std::max<UINT>(1U, GetDpiForWindow(window_));
        const auto widthDip = (client.right - client.left) * 96.0f / dpi;
        const auto heightDip = albumContentHeightDip(
            (client.bottom - client.top) * 96.0f / dpi);
        const auto layout = computeAlbumModeLayout(
            widthDip, heightDip,
            lyricsSurfaceAvailable(LyricsService::instance().snapshot()),
            compactSelection_, albumPanelVisibility());
        const auto booklet = computeAlbumBookletLayout(
            widthDip, heightDip, albumPanelVisibility());
        const auto edgeDip = std::max({
            layout.cover.right - layout.cover.left,
            layout.cover.bottom - layout.cover.top,
            booklet.artwork.right - booklet.artwork.left,
            booklet.artwork.bottom - booklet.artwork.top,
            1.0f});
        const auto physical = static_cast<std::uint32_t>(std::clamp(
            std::ceil(static_cast<double>(edgeDip) * dpi / 96.0),
            1.0, 2048.0));
        const auto analysis = a4::style()==a4::Style::spectralAlloy &&
                a4::materialStyle()==a4::MaterialStyle::spectralAlloyV2
            ? ArtAnalysisFlags::alloyV2 : ArtAnalysisFlags::legacy;
        auto result = artCache_.getOrRequestVariant(
            artworkIdentity,
            state->tracks[index].handle, physical, true,
            ArtRequestPriority::visibleMain, analysis);
        if(!result && analysis==ArtAnalysisFlags::alloyV2 &&
                artCache_.status(artworkIdentity,physical,
                    ArtAnalysisFlags::legacy)==ArtCacheStatus::ready){
            // Keep the immutable material from this same artwork visible while
            // its coalesced V2 profile is prepared by the artwork worker.
            result=artCache_.getOrRequestVariant(artworkIdentity,
                state->tracks[index].handle,physical,true,
                ArtRequestPriority::visibleMain,ArtAnalysisFlags::legacy);
        }

        if(resolvedStatus){
            const auto status=result?ArtCacheStatus::ready:
                artCache_.status(artworkIdentity,physical,analysis);
            *resolvedStatus=result?ArtCacheStatus::ready:
                status==ArtCacheStatus::missing?ArtCacheStatus::missing:ArtCacheStatus::loading;
        }
        if (artworkPrefetchSessionId_ != state->sessionId ||
            artworkPrefetchEdge_ != physical ||
            artworkPrefetchAnalysis_ != analysis ||
            artworkPrefetchCenter_ != index) {
            artworkPrefetchSessionId_ = state->sessionId;
            artworkPrefetchEdge_ = physical;
            artworkPrefetchAnalysis_ = analysis;
            artworkPrefetchCenter_ = index;
            constexpr std::size_t kPrefetchLimit = 12U;
            std::size_t scheduled = 0;
            std::vector<std::size_t> candidates;
            std::vector<std::wstring> identities;
            candidates.reserve(kPrefetchLimit);
            identities.reserve(kPrefetchLimit);
            for (std::size_t distance = 1;
                 scheduled < kPrefetchLimit &&
                     (index + distance < state->tracks.size() ||
                         distance <= index);
                 ++distance) {
                const auto request = [&](std::size_t candidate) {
                    const auto identity = artworkTrackIdentity(
                        state->tracks[candidate].handle);
                    if (identity.empty()) return;
                    candidates.push_back(candidate);
                    identities.push_back(identity);
                    ++scheduled;
                };
                if (index + distance < state->tracks.size() &&
                    scheduled < kPrefetchLimit) {
                    request(index + distance);
                }
                if (distance <= index && scheduled < kPrefetchLimit) {
                    request(index - distance);
                }
            }
            artCache_.retainPrefetch(identities, physical, analysis);
            for (std::size_t candidate = 0; candidate < candidates.size(); ++candidate)
                (void)artCache_.getOrRequest(identities[candidate],
                    state->tracks[candidates[candidate]].handle, physical,
                    ArtRequestPriority::prefetch, analysis);
        }
        return result;
    }

    void syncAlbumThemeIntent(bool resolveArtwork)
    {
        const auto state = AlbumSessionService::instance().snapshot();
        const auto identity = albumArtworkIdentity(state);
        if (identity != albumThemeIdentity_) {
            albumThemeIdentity_ = identity;
            ThemeService::instance().resetForTrack(identity);
        }
        if (!resolveArtwork || !albumArtworkActive_ || identity.empty()) return;
        // The anchor is known while membership is loading, but a lack of
        // rows at this point is not evidence that the cover is missing.
        if (!state || state->state == AlbumSessionState::loading) return;
        const auto content = albumContent(state);
        ArtCacheStatus status{};
        const auto artwork = albumArtwork(state, content, identity,&status);
        if (artwork) {
            (void)ThemeService::instance().updateFromArtwork(identity,*artwork);
        }else if(status==ArtCacheStatus::missing){
            (void)ThemeService::instance().resolveWithoutArtwork(identity);
        }
    }

    std::pair<std::wstring, std::uint64_t> albumPersonalNote(
        const std::shared_ptr<const AlbumSessionSnapshot>& state) const
    {
        if (!state || state->currentInSession >= state->tracks.size()) return {};
        const auto user = DatabaseService::instance().userStateSnapshot();
        const auto& track = state->tracks[state->currentInSession];
        if (!user || !user->available || track.entityId.empty() ||
            user->entityId != track.entityId) return {};
        return {utf8ToWide(user->note), user->generation};
    }

    void showAlbumContextMenu(POINT screenPoint)
    {
        const auto state = AlbumSessionService::instance().snapshot();
        if (!state || state->currentInSession >= state->tracks.size()) return;
        const auto& current = state->tracks[state->currentInSession];
        if (!current.handle.is_valid()) return;

        constexpr UINT kFavorite = 1U;
        constexpr UINT kNote = 2U;
        constexpr UINT kPersona = 3U;
        constexpr UINT kLyricsWrong = 4U;
        constexpr UINT kLyricsStatic = 5U;
        constexpr UINT kLyricsRetry = 6U;
        constexpr UINT kLyricsFollow = 7U;
        constexpr UINT kContextBase = 1000U;
        constexpr UINT kContextCount = 24000U;
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr) return;
        const auto user = DatabaseService::instance().userStateSnapshot();
        metadb_handle_ptr representedPlaying;
        const bool representedPlayback=playback_control::get()->get_now_playing(representedPlaying) && representedPlaying==current.handle;
        const bool userAvailable=!state->outsideSession && representedPlayback && user && user->available && !user->stale &&
            !current.entityId.empty() && user->entityId==current.entityId;
        const auto persona=DatabaseService::instance().personaSnapshot();
        const bool personaAvailable=userAvailable && persona && persona->available && !persona->aliasKey.empty();
        AppendMenuW(menu, MF_STRING | (userAvailable?MF_ENABLED:MF_GRAYED), kFavorite,
            userAvailable && user->favorite ? L"Remove favorite" : L"Add favorite");
        AppendMenuW(menu, MF_STRING | (userAvailable?MF_ENABLED:MF_GRAYED), kNote, L"Edit private note…");
        AppendMenuW(menu, MF_STRING | (personaAvailable?MF_ENABLED:MF_GRAYED), kPersona, L"Edit artist persona…");
        const auto lyrics = LyricsService::instance().snapshot();
        const bool lyricsAvailable=lyrics && !state->outsideSession && userAvailable &&
            lyrics->request.recording.entityId==current.entityId;
        const auto hasLyrics = lyricsAvailable && lyrics->document;
        const auto hasTiming = hasLyrics &&
            (lyrics->document->timing == TimingStatus::catalogMatched ||
             lyrics->document->timing == TimingStatus::audioVerified);
        HMENU lyricsMenu = CreatePopupMenu();
        if (lyricsMenu != nullptr) {
            AppendMenuW(lyricsMenu, MF_STRING, kLyricsFollow,
                L"Follow lyrics");
            AppendMenuW(lyricsMenu,
                MF_STRING | (hasLyrics ? MF_ENABLED : MF_GRAYED),
                kLyricsWrong, L"Wrong lyrics");
            AppendMenuW(lyricsMenu,
                MF_STRING | (hasTiming ? MF_ENABLED : MF_GRAYED),
                kLyricsStatic, L"Show without sync");
            AppendMenuW(lyricsMenu, MF_STRING | (lyricsAvailable?MF_ENABLED:MF_GRAYED), kLyricsRetry,
                L"Try lyrics again");
            AppendMenuW(menu, MF_POPUP,
                reinterpret_cast<UINT_PTR>(lyricsMenu), L"Lyrics");
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        metadb_handle_list handles;
        handles.add_item(current.handle);
        auto manager = contextmenu_manager::g_create();
        constexpr unsigned flags = contextmenu_manager::flag_show_shortcuts |
            contextmenu_manager::flag_show_shortcuts_global |
            contextmenu_manager::flag_view_full;
        manager->init_context(handles, flags);
        manager->win32_build_menu(menu, kContextBase, kContextCount);
        menu_helpers::win32_auto_mnemonics(menu);

        auto root = GetAncestor(window_, GA_ROOT);
        if (root == nullptr) root = window_;
        SetForegroundWindow(root);
        const auto selected = TrackPopupMenuEx(menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
            screenPoint.x, screenPoint.y, window_, nullptr);
        lastAlbumMenuSelection_=selected;
        const auto liveUser=DatabaseService::instance().userStateSnapshot();
        const auto livePersona=DatabaseService::instance().personaSnapshot();
        const auto liveLyrics=LyricsService::instance().snapshot();
        const auto liveSession=AlbumSessionService::instance().snapshot();
        const bool sameSession=liveSession && liveSession->state==AlbumSessionState::ready && !liveSession->outsideSession && liveSession->sessionId==state->sessionId &&
            liveSession->currentInSession<liveSession->tracks.size() &&
            liveSession->tracks[liveSession->currentInSession].handle==current.handle;
        metadb_handle_ptr livePlaying;
        const bool samePlayback=playback_control::get()->get_now_playing(livePlaying) && livePlaying==current.handle;
        const bool sameUser=userAvailable && sameSession && samePlayback && liveUser && liveUser->available && !liveUser->stale &&
            liveUser->entityId==current.entityId && liveUser->trackId==user->trackId;
        const bool samePersona=personaAvailable && sameUser && livePersona &&
            livePersona->available && livePersona->aliasKey==persona->aliasKey;
        const bool sameLyrics=lyricsAvailable && sameSession && liveLyrics && liveLyrics->request==lyrics->request &&
            liveLyrics->presentationRevision==lyrics->presentationRevision && liveLyrics->request.recording.entityId==current.entityId;
        const bool stale=(selected==kFavorite || selected==kNote)?!sameUser:
            selected==kPersona?!samePersona:
            (selected==kLyricsWrong || selected==kLyricsStatic || selected==kLyricsRetry)?!sameLyrics:
            selected==kLyricsFollow?!sameSession:false;
        if(stale) {
            ++staleAlbumMenuActions_;
            albumRenderer_.setActionStatus(L"Track changed · reopen this menu");
            InvalidateRect(window_,nullptr,FALSE);
        } else if (selected == kFavorite) {
            (void)DatabaseService::instance().toggleCurrentFavorite();
        } else if (selected == kNote) {
            NoteEditorWindow::instance().show(root);
        } else if (selected == kPersona) {
            const auto content = albumContent(state);
            PersonaEditorWindow::instance().show(root,
                content ? content->biography : std::wstring{});
        } else if (selected == kLyricsFollow) {
            albumRenderer_.followLyrics();
            InvalidateRect(window_, nullptr, FALSE);
        } else if (selected == kLyricsWrong) {
            (void)LyricsService::instance().markCurrentWrongLyrics();
        } else if (selected == kLyricsStatic) {
            (void)LyricsService::instance().showCurrentWithoutSync();
        } else if (selected == kLyricsRetry) {
            LyricsService::instance().retryCurrent();
        } else if (selected >= kContextBase &&
            selected < kContextBase + kContextCount) {
            manager->execute_by_id(selected - kContextBase);
        }
        DestroyMenu(menu);
        PostMessageW(window_, WM_NULL, 0, 0);
    }
};

void ShellChildHost::on_size_limit_change(HWND, unsigned)
{
    if (owner_ != nullptr) owner_->childLimitsChanged();
}

bool ShellChildHost::get_keyboard_shortcuts_enabled() const
{
    return owner_ != nullptr && owner_->keyboardShortcutsEnabled();
}

uie::window_factory<PlayerShellPanel> g_playerShellPanelFactory;

} // namespace

const GUID& playerShellPanelGuid() noexcept
{
    return kPlayerShellPanelGuid;
}

UINT playerShellModeMessage() noexcept
{
    static const auto message = RegisterWindowMessageW(
        L"GodenPlayerShellModeV1");
    return message;
}

namespace {

BOOL CALLBACK visitPlayerShell(HWND window, LPARAM value)
{
    wchar_t name[64]{};
    if (GetClassNameW(window, name, static_cast<int>(std::size(name))) != 0 &&
        (std::wcscmp(name, L"GodenPlayerShellV2") == 0 ||
            std::wcscmp(name, L"GodenPlayerShellV1") == 0)) {
        if (value <= 1) {
            SendMessageW(window, playerShellModeMessage(),
                static_cast<WPARAM>(value), 0);
        } else {
            // Reconcile the selected cover/palette even when it is a cache hit.
            // Post rather than reentering session/theme work during publish().
            PostMessageW(window, albumContentReadyMessage(), 0, 0);
            InvalidateRect(window, nullptr, FALSE);
        }
    }
    return TRUE;
}

} // namespace

bool requestPlayerShellMode(PlayerShellMode mode) noexcept
{
    const auto root = static_cast<HWND>(core_api::get_main_window());
    if (root == nullptr) return false;
    struct Request {
        PlayerShellMode mode;
        bool accepted;
    } request{mode, false};
    EnumChildWindows(root, [](HWND window, LPARAM raw) -> BOOL {
        auto& request = *reinterpret_cast<Request*>(raw);
        wchar_t name[64]{};
        if (GetClassNameW(window, name, static_cast<int>(std::size(name))) != 0 &&
            std::wcscmp(name, L"GodenPlayerShellV2") == 0 &&
            SendMessageW(window, playerShellModeMessage(),
                static_cast<WPARAM>(request.mode), 0) != FALSE) {
            request.accepted = true;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&request));
    if (!request.accepted) return false;
    g_playerShellMode.store(mode, std::memory_order_release);
    InvalidateRect(root, nullptr, FALSE);
    return true;
}

void setPlayerShellMode(PlayerShellMode mode) noexcept
{
    (void)requestPlayerShellMode(mode);
}

PlayerShellMode playerShellMode() noexcept
{
    return g_playerShellMode.load(std::memory_order_acquire);
}

void notifyPlayerShellsChanged() noexcept
{
    const auto root = static_cast<HWND>(core_api::get_main_window());
    if (root != nullptr) EnumChildWindows(root, visitPlayerShell, 2);
}

} // namespace nekorox::component
