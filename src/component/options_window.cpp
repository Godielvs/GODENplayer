#include "qa_hooks.h"
#include "options_window.h"

#include "daw_window.h"
#include "discord_presence_service.h"
#include "frameless_window.h"
#include "keep_playing_service.h"
#include "listen_window.h"
#include "lyrics_service.h"
#include "text_util.h"
#include "ui_form_layout.h"
#include "ui_preferences.h"
#include "a4/a4_preferences.h"
#include "runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <commctrl.h>
#include <dwmapi.h>
#include <string>
#include <uxtheme.h>

namespace nekorox::component {
namespace {

constexpr wchar_t kWindowClass[] = L"GodenOptionsV1";
constexpr COLORREF kBackground = RGB(8, 10, 10);
constexpr COLORREF kPanel = RGB(15, 18, 18);
constexpr COLORREF kSurface = RGB(21, 25, 25);
constexpr COLORREF kBorder = RGB(48, 56, 57);
constexpr COLORREF kText = RGB(216, 223, 223);
constexpr COLORREF kMuted = RGB(118, 132, 134);
constexpr COLORREF kAccent = RGB(0, 216, 208);
constexpr COLORREF kWarning = RGB(224, 140, 94);
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
constexpr int kIdentityId = 1001;
constexpr int kFallbackButtonId = 1002;
constexpr int kFallbackValueId = 1003;
constexpr int kMotionId = 1004;
constexpr int kListenEffectsId = 1005;
constexpr int kListenEffectsStatusId = 1006;
constexpr int kAlbumAmbientId = 1007;
constexpr int kAlbumAmbientStatusId = 1008;
constexpr int kKeepPlayingId = 1010;
constexpr int kKeepModeId = 1011;
constexpr int kLyricsOnlineId = 1012;
constexpr int kLyricsOfflineId = 1013;
constexpr int kPresenceId = 1020;
constexpr int kPresencePausedId = 1021;
constexpr int kApplicationId = 1022;
constexpr int kApplyPresenceId = 1023;
constexpr int kPresenceStatusId = 1024;
constexpr int kPresenceCoversId = 1025;
constexpr int kCoverProviderId=1026;
constexpr int kRelayEndpointId=1027;
constexpr int kRelayCredentialId=1028;
constexpr int kCoverDisclosureId=1029;
constexpr int kListenId = 1030;
constexpr int kDawId = 1031;
constexpr int kCloseId = 1040;
constexpr int kViewportId = 1090;
constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT_PTR kCheckboxSubclassId = 0x474f4445U;
constexpr UINT_PTR kButtonSubclassId = 0x474f4446U;
constexpr wchar_t kCheckboxStateProperty[] = L"Goden.Options.CheckState";
constexpr wchar_t kHoverProperty[] = L"Goden.Options.Hover";

COLORREF blend(COLORREF from, COLORREF to, float amount) noexcept
{
    const auto channel = [amount](BYTE left, BYTE right) {
        return static_cast<BYTE>(std::lround(
            static_cast<float>(left) +
            (static_cast<float>(right) - static_cast<float>(left)) * amount));
    };
    return RGB(
        channel(GetRValue(from), GetRValue(to)),
        channel(GetGValue(from), GetGValue(to)),
        channel(GetBValue(from), GetBValue(to)));
}

void updateHover(HWND control, UINT message)
{
    if (message == WM_MOUSEMOVE &&
            GetPropW(control, kHoverProperty) == nullptr) {
        SetPropW(control, kHoverProperty,
            reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, control, 0};
        TrackMouseEvent(&tracking);
        InvalidateRect(control, nullptr, FALSE);
    } else if (message == WM_MOUSELEAVE) {
        RemovePropW(control, kHoverProperty);
        InvalidateRect(control, nullptr, FALSE);
    }
}

bool qaBackgroundRequested() noexcept
{
    if (!qa::hooksAllowed()) return false;
    wchar_t hooks[8]{};
    wchar_t profile[8]{};
    wchar_t background[8]{};
    return GetEnvironmentVariableW(L"NEKOROX_QA_HOOKS", hooks,
               static_cast<DWORD>(std::size(hooks))) == 1U &&
        hooks[0] == L'1' &&
        GetEnvironmentVariableW(L"NEKOROX_QA_PROFILE", profile,
               static_cast<DWORD>(std::size(profile))) == 1U &&
        profile[0] == L'1' &&
        GetEnvironmentVariableW(L"NEKOROX_QA_BACKGROUND", background,
               static_cast<DWORD>(std::size(background))) == 1U &&
        background[0] == L'1';
}

int dip(HWND window, int value) noexcept
{
    return MulDiv(value, static_cast<int>((std::max)(96U,
        GetDpiForWindow(window))), 96);
}

void darkTheme(HWND control)
{
    if (control != nullptr) SetWindowTheme(control, L"DarkMode_Explorer", nullptr);
}

HWND makeControl(HWND parent, DWORD exStyle, const wchar_t* type,
    const wchar_t* text, DWORD style, int id)
{
    auto result = CreateWindowExW(exStyle, type, text,
        WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        core_api::get_my_instance(), nullptr);
    darkTheme(result);
    return result;
}

LRESULT CALLBACK checkboxSubclassProc(HWND control, UINT message,
    WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR)
{
    updateHover(control, message);
    switch (message) {
    case BM_GETCHECK:
        return GetPropW(control, kCheckboxStateProperty) != nullptr
            ? BST_CHECKED : BST_UNCHECKED;
    case BM_SETCHECK:
        if (wParam == BST_CHECKED) {
            SetPropW(control, kCheckboxStateProperty,
                reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
        } else {
            RemovePropW(control, kCheckboxStateProperty);
        }
        InvalidateRect(control, nullptr, FALSE);
        return 0;
    case WM_NCDESTROY:
        RemovePropW(control, kCheckboxStateProperty);
        RemovePropW(control, kHoverProperty);
        RemoveWindowSubclass(control, checkboxSubclassProc, subclassId);
        break;
    default:
        break;
    }
    return DefSubclassProc(control, message, wParam, lParam);
}

LRESULT CALLBACK buttonSubclassProc(HWND control, UINT message,
    WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR)
{
    updateHover(control, message);
    if (message == WM_NCDESTROY) {
        RemovePropW(control, kHoverProperty);
        RemoveWindowSubclass(control, buttonSubclassProc, subclassId);
    }
    return DefSubclassProc(control, message, wParam, lParam);
}

HWND makeCheckbox(HWND parent, const wchar_t* text, int id)
{
    const auto result = makeControl(parent, 0, L"BUTTON", text,
        BS_OWNERDRAW | WS_TABSTOP, id);
    if (result != nullptr) {
        SetWindowSubclass(result, checkboxSubclassProc,
            kCheckboxSubclassId, 0);
    }
    return result;
}

HWND makeButton(HWND parent, const wchar_t* text, int id)
{
    const auto result = makeControl(parent, 0, L"BUTTON", text,
        BS_OWNERDRAW | WS_TABSTOP, id);
    if (result != nullptr) {
        SetWindowSubclass(result, buttonSubclassProc, kButtonSubclassId, 0);
    }
    return result;
}

void move(HWND control, int x, int y, int width, int height)
{
    if (control != nullptr) MoveWindow(control, x, y,
        (std::max)(1, width), (std::max)(1, height), TRUE);
}

} // namespace

OptionsWindow& OptionsWindow::instance()
{
    static OptionsWindow value;
    return value;
}

void OptionsWindow::show(HWND owner)
{
    if (window_ != nullptr) {
        refreshAll();
        if (!qaBackgroundRequested()) {
            ShowWindow(window_, SW_RESTORE);
            SetForegroundWindow(window_);
        }
        return;
    }
    owner_ = owner != nullptr ? GetAncestor(owner, GA_ROOT)
                              : static_cast<HWND>(core_api::get_main_window());
    if (owner_ == nullptr) owner_ = owner;

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = &OptionsWindow::windowProc;
    windowClass.hInstance = core_api::get_my_instance();
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);

    const auto dpiValue = owner_ != nullptr
        ? (std::max)(96U, GetDpiForWindow(owner_)) : 96U;
    const auto desiredWidth = MulDiv(720, dpiValue, 96);
    const auto desiredHeight = MulDiv(760, dpiValue, 96);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    const auto monitor = MonitorFromWindow(
        owner_, MONITOR_DEFAULTTONEAREST);
    RECT work{};
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitorInfo))
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    else
        work = monitorInfo.rcWork;
    const auto width = std::min(
        desiredWidth, static_cast<int>(work.right - work.left));
    const auto height = std::min(
        desiredHeight, static_cast<int>(work.bottom - work.top));
    RECT ownerBounds{work.left, work.top, work.right, work.bottom};
    if (owner_ != nullptr) GetWindowRect(owner_, &ownerBounds);
    const auto x = std::clamp(
        ownerBounds.left + (ownerBounds.right - ownerBounds.left - width) / 2,
        work.left, (std::max)(work.left, work.right - width));
    const auto y = std::clamp(
        ownerBounds.top + (ownerBounds.bottom - ownerBounds.top - height) / 2,
        work.top, (std::max)(work.top, work.bottom - height));
    // Keep the app-window independent; owner_ remains a placement/DPI anchor.
    window_ = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT,
        kWindowClass, L"GODENplayer Options",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x, y, width, height, nullptr, nullptr,
        core_api::get_my_instance(), this);
    if (window_ == nullptr) return;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
    const COLORREF caption = kBackground;
    const COLORREF captionText = kText;
    const COLORREF border = kBorder;
    DwmSetWindowAttribute(window_, DWMWA_CAPTION_COLOR,
        &caption, sizeof(caption));
    DwmSetWindowAttribute(window_, DWMWA_TEXT_COLOR,
        &captionText, sizeof(captionText));
    DwmSetWindowAttribute(window_, DWMWA_BORDER_COLOR,
        &border, sizeof(border));
    if (qaBackgroundRequested()) {
        ShowWindow(window_, SW_HIDE);
    } else {
        ShowWindow(window_, SW_SHOW);
        SetForegroundWindow(window_);
    }
}

void OptionsWindow::close()
{
    if (window_ != nullptr) DestroyWindow(window_);
}

LRESULT CALLBACK OptionsWindow::windowProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    OptionsWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<OptionsWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<OptionsWindow*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    return self != nullptr
        ? self->handleMessage(window, message, wParam, lParam)
        : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT OptionsWindow::handleMessage(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        window_ = window;
        backgroundBrush_ = CreateSolidBrush(kBackground);
        panelBrush_ = CreateSolidBrush(kPanel);
        editBrush_ = CreateSolidBrush(kSurface);
        if (!viewport_.create(window_, kViewportId)) return -1;
        createControls();
        SetTimer(window_, kStatusTimer, 1000, nullptr);
        return 0;
    case WM_SIZE:
        updateLayout();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
        if (viewport_.viewport() != nullptr)
            SendMessageW(viewport_.viewport(), message, wParam, lParam);
        return 0;
    case WM_DPICHANGED: {
        const auto* bounds = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window_, nullptr, bounds->left, bounds->top,
            bounds->right - bounds->left, bounds->bottom - bounds->top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        updateFont();
        updateLayout();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        applyDpiAwareMinimum(window, limits, 620, 480);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kStatusTimer && IsWindowVisible(window_)) {
            refreshPresenceStatus();
            refreshListenEffectsStatus();
            refreshAlbumAmbientStatus();
        }
        return 0;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT:
        paint();
        return 0;
    case WM_PRINTCLIENT:
        paint(reinterpret_cast<HDC>(wParam));
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        const auto dc = reinterpret_cast<HDC>(wParam);
        const auto control = reinterpret_cast<HWND>(lParam);
        const auto isSection = std::find(sectionLabels_.begin(),
            sectionLabels_.end(), control) != sectionLabels_.end();
        SetTextColor(dc, isSection ? kAccent :
            control == footerStatus_ || control == motionLabel_ ||
                    control == applicationLabel_ || control == presenceHint_
                ? kMuted : control == presenceStatus_ &&
                    DiscordPresenceService::instance().externalPublisherConflict()
                ? kWarning : kText);
        SetBkColor(dc, kPanel);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(panelBrush_);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        const auto dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kText);
        SetBkColor(dc, kSurface);
        return reinterpret_cast<LRESULT>(editBrush_);
    }
    case WM_DRAWITEM:
        if (const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            item.CtlID == kIdentityId || item.CtlID == kKeepPlayingId ||
            item.CtlID == kPresenceId || item.CtlID == kPresencePausedId ||
            item.CtlID == kPresenceCoversId || item.CtlID == kCoverDisclosureId ||
            item.CtlID == kLyricsOnlineId || item.CtlID == kLyricsOfflineId) {
            drawCheckbox(item);
        } else if (item.CtlID == kMotionId || item.CtlID == kKeepModeId ||
                   item.CtlID == kListenEffectsId ||
                   item.CtlID == kAlbumAmbientId || item.CtlID == kCoverProviderId) {
            drawComboItem(item);
        } else {
            drawButton(item);
        }
        return TRUE;
    case WM_MEASUREITEM: {
        auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (item != nullptr &&
                (item->CtlID == kMotionId || item->CtlID == kKeepModeId ||
                 item->CtlID == kListenEffectsId ||
                 item->CtlID == kAlbumAmbientId || item->CtlID == kCoverProviderId)) {
            item->itemHeight = dip(window_, 25);
            return TRUE;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
    case WM_COMMAND: {
        const auto id = LOWORD(wParam);
        const auto notification = HIWORD(wParam);
        if (id == kIdentityId && notification == BN_CLICKED) {
            setHeaderWordmarkEnabled(toggleCheckbox(identity_));
        } else if (id == kFallbackButtonId && notification == BN_CLICKED) {
            if (chooseFallbackAccentColor(window_)) updateFallbackLabel();
        } else if (id == kMotionId && notification == CBN_SELCHANGE) {
            const auto index = SendMessageW(motion_, CB_GETCURSEL, 0, 0);
            if (index >= 0 && index <= 3) {
                setMotionPreference(static_cast<MotionPreference>(index));
            }
        } else if (id == kListenEffectsId && notification == CBN_SELCHANGE) {
            const auto index = SendMessageW(
                listenEffects_, CB_GETCURSEL, 0, 0);
            if (index >= 0 && index <= 1) {
                setListenEffectsMode(static_cast<ListenEffectsMode>(index));
                refreshListenEffectsStatus();
            }
        } else if (id == kAlbumAmbientId && notification == CBN_SELCHANGE) {
            const auto index = SendMessageW(albumAmbient_, CB_GETCURSEL, 0, 0);
            if (index >= 0 && index <= 2) {
                setAlbumAmbientMode(static_cast<AlbumAmbientMode>(index));
                refreshAlbumAmbientStatus();
            }
        } else if (id == kKeepPlayingId && notification == BN_CLICKED) {
            KeepPlayingService::instance().setEnabled(
                toggleCheckbox(keepPlaying_));
        } else if (id == kKeepModeId && notification == CBN_SELCHANGE) {
            KeepPlayingService::instance().setMode(
                SendMessageW(keepMode_, CB_GETCURSEL, 0, 0) == 1
                    ? KeepPlayingMode::sequentialAlbums
                    : KeepPlayingMode::randomAlbums);
        } else if (id == kLyricsOnlineId && notification == BN_CLICKED) {
            LyricsService::instance().setOnlineEnabled(
                toggleCheckbox(lyricsOnline_));
            refreshAll();
        } else if (id == kLyricsOfflineId && notification == BN_CLICKED) {
            LyricsService::instance().setOfflineMode(
                toggleCheckbox(lyricsOffline_));
            refreshAll();
        } else if (id == kPresenceId && notification == BN_CLICKED) {
            const auto enabled = toggleCheckbox(presence_);
            auto& service = DiscordPresenceService::instance();
            service.setEnabled(enabled);
            if (enabled && service.externalPublisherConflict()) {
                MessageBoxW(window_,
                    L"Music Presence is also publishing foobar2000. "
                    L"Disable only foobar2000 in Music Presence to keep "
                    L"GODENplayer's complete progress card stable.",
                    L"Discord Presence conflict", MB_OK | MB_ICONWARNING);
            }
            refreshAll();
        } else if (id == kPresencePausedId && notification == BN_CLICKED) {
            DiscordPresenceService::instance().setShowPaused(
                toggleCheckbox(presencePaused_));
        } else if (id == kPresenceCoversId && notification == BN_CLICKED) {
            DiscordPresenceService::instance().setTemporaryCovers(
                toggleCheckbox(presenceCovers_));
            refreshPresenceStatus();
        } else if (id == kCoverProviderId && notification == CBN_SELCHANGE) {
            const auto provider=SendMessageW(coverProvider_,CB_GETCURSEL,0,0);
            EnableWindow(relayEndpoint_,provider==2);EnableWindow(relayCredential_,provider==2);
        } else if (id == kCoverDisclosureId && notification == BN_CLICKED) {
            toggleCheckbox(coverDisclosure_);
        } else if (id == kApplyPresenceId && notification == BN_CLICKED) {
            const auto readEdit=[](HWND edit) {
                std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(edit))+1U,L'\0');
                text.resize(GetWindowTextW(edit,text.data(),static_cast<int>(text.size())));
                return wideToUtf8(text);
            };
            const auto provider=static_cast<PresenceCoverProvider>(SendMessageW(coverProvider_,CB_GETCURSEL,0,0));
            const auto application = readEdit(applicationId_);
            if (application.size() < 16 || application.size() > 24 ||
                    !std::all_of(application.begin(), application.end(),
                        [](unsigned char c) { return c >= '0' && c <= '9'; })) {
                MessageBoxW(window_, L"Application ID must contain 16\u201324 decimal digits.",
                    L"GODENplayer Music Presence", MB_OK | MB_ICONWARNING);
                return 0;
            }
            if(!DiscordPresenceService::instance().setCoverConfiguration(provider,
                    readEdit(relayEndpoint_),readEdit(relayCredential_),
                    SendMessageW(coverDisclosure_,BM_GETCHECK,0,0)==BST_CHECKED)) {
                MessageBoxW(window_,L"Choose Catbox, or enter a valid HTTPS relay origin and private credential.",
                    L"Cover provider",MB_OK|MB_ICONWARNING);return 0;
            }
            SetWindowTextW(relayCredential_,L"");
            std::wstring value(static_cast<std::size_t>(
                GetWindowTextLengthW(applicationId_)) + 1U, L'\0');
            value.resize(static_cast<std::size_t>(GetWindowTextW(
                applicationId_, value.data(), static_cast<int>(value.size()))));
            if (!DiscordPresenceService::instance().setApplicationId(
                    wideToUtf8(value))) {
                MessageBoxW(window_,
                    L"Application ID must contain 16\u201324 decimal digits.",
                    L"GODENplayer Music Presence", MB_OK | MB_ICONWARNING);
            }
            refreshAll();
        } else if (id == kListenId && notification == BN_CLICKED) {
            ListenWindow::instance().show();
        } else if (id == kDawId && notification == BN_CLICKED) {
            DawWindow::instance().show(window_);
        } else if (id == kCloseId && notification == BN_CLICKED) {
            DestroyWindow(window_);
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kStatusTimer);
        releaseResources();
        window_ = nullptr;
        owner_ = nullptr;
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

void OptionsWindow::createControls()
{
    const auto content = viewport_.content();
    title_ = makeControl(window_, 0, L"STATIC", L"GODENplayer OPTIONS",
        SS_LEFT, 1080);
    footerStatus_ = makeControl(window_, 0, L"STATIC",
        L"Changes are saved immediately", SS_LEFT, 1081);
    closeButton_ = makeButton(window_, L"CLOSE", kCloseId);
    constexpr std::array<const wchar_t*, 5> sectionNames{
        L"APPEARANCE & MOTION", L"PLAYBACK CONTINUITY",
        L"AUTOMATIC LYRICS", L"DISCORD MUSIC PRESENCE", L"TOOLS"};
    for (std::size_t index = 0; index < sectionLabels_.size(); ++index)
        sectionLabels_[index] = makeControl(content, 0, L"STATIC",
            sectionNames[index], SS_LEFT, 1082 + static_cast<int>(index));
    motionLabel_ = makeControl(content, 0, L"STATIC", L"UI motion profile",
        SS_LEFT, 1087);
    applicationLabel_ = makeControl(content, 0, L"STATIC",
        L"Discord Application ID", SS_LEFT, 1088);
    presenceHint_ = makeControl(content, 0, L"STATIC",
        L"Catbox miniatures are public and may remain for years. Disabling covers stops new uploads. Private relay uses its own retention. Apply saves the provider and permission.",
        SS_LEFT, 1089);

    identity_ = makeCheckbox(content,
        L"Show GODENplayer identity in the header", kIdentityId);
    fallbackButton_ = makeButton(content, L"CHOOSE RGB\u2026", kFallbackButtonId);
    fallbackValue_ = makeControl(content, 0, L"STATIC", L"",
        SS_LEFT, kFallbackValueId);
    motion_ = makeControl(content, 0, WC_COMBOBOXW, L"",
        CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS |
            WS_TABSTOP | WS_VSCROLL, kMotionId);
    for (const auto* value : {L"Normal", L"Reduced", L"Off", L"Follow Windows"}) {
        SendMessageW(motion_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(value));
    }
    SendMessageW(motion_, CB_SETMINVISIBLE, 4, 0);
    listenEffects_ = makeControl(content, 0, WC_COMBOBOXW, L"",
        CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS |
            WS_TABSTOP | WS_VSCROLL, kListenEffectsId);
    SendMessageW(listenEffects_, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(L"Soft light"));
    SendMessageW(listenEffects_, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(L"No effects"));
    SendMessageW(listenEffects_, CB_SETMINVISIBLE, 2, 0);
    listenEffectsStatus_ = makeControl(content, 0, L"STATIC", L"",
        SS_LEFT, kListenEffectsStatusId);
    albumAmbient_ = makeControl(content, 0, WC_COMBOBOXW, L"",
        CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS |
            WS_TABSTOP | WS_VSCROLL, kAlbumAmbientId);
    for (const auto* value : {L"Off", L"Subtle", L"Reactive"}) {
        SendMessageW(albumAmbient_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(value));
    }
    SendMessageW(albumAmbient_, CB_SETMINVISIBLE, 3, 0);
    albumAmbientStatus_ = makeControl(content, 0, L"STATIC", L"",
        SS_LEFT, kAlbumAmbientStatusId);
    keepPlaying_ = makeCheckbox(content,
        L"Keep Playing after the current album", kKeepPlayingId);
    keepMode_ = makeControl(content, 0, WC_COMBOBOXW, L"",
        CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS |
            WS_TABSTOP | WS_VSCROLL, kKeepModeId);
    SendMessageW(keepMode_, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(L"Random albums \u00b7 shuffle bag"));
    SendMessageW(keepMode_, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(L"Sequence \u00b7 captured library order"));
    SendMessageW(keepMode_, CB_SETMINVISIBLE, 2, 0);
    lyricsOnline_ = makeCheckbox(content,
        L"Automatic online lyrics (artist/title/album/duration only)",
        kLyricsOnlineId);
    lyricsOffline_ = makeCheckbox(content,
        L"Offline lyrics mode (cancel network)", kLyricsOfflineId);

    presence_ = makeCheckbox(content,
        L"Enable native Discord Music Presence", kPresenceId);
    presencePaused_ = makeCheckbox(content,
        L"Keep the track visible while paused", kPresencePausedId);
    presenceCovers_ = makeCheckbox(content,
        L"Show current album cover in Discord", kPresenceCoversId);
    coverProvider_=makeControl(content,0,WC_COMBOBOXW,L"",
        CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_TABSTOP|WS_VSCROLL,kCoverProviderId);
    for(const auto* label:{L"Choose a cover provider",L"Catbox - public miniature",L"Private relay"})
        SendMessageW(coverProvider_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
    relayEndpoint_=makeControl(content,WS_EX_CLIENTEDGE,L"EDIT",L"",
        ES_LEFT|ES_AUTOHSCROLL|WS_TABSTOP,kRelayEndpointId);
    relayCredential_=makeControl(content,WS_EX_CLIENTEDGE,L"EDIT",L"",
        ES_LEFT|ES_AUTOHSCROLL|ES_PASSWORD|WS_TABSTOP,kRelayCredentialId);
    SendMessageW(relayEndpoint_,EM_SETLIMITTEXT,2048,0);
    SendMessageW(relayCredential_,EM_SETLIMITTEXT,256,0);
    SendMessageW(relayEndpoint_,EM_SETCUEBANNER,0,reinterpret_cast<LPARAM>(L"Private relay HTTPS origin"));
    SendMessageW(relayCredential_,EM_SETCUEBANNER,0,reinterpret_cast<LPARAM>(L"Private credential (blank keeps saved value)"));
    coverDisclosure_=makeCheckbox(content,L"Allow sending cover miniatures to the chosen provider",kCoverDisclosureId);
    applicationId_ = makeControl(content, WS_EX_CLIENTEDGE, L"EDIT", L"",
        ES_LEFT | ES_AUTOHSCROLL | WS_TABSTOP, kApplicationId);
    SendMessageW(applicationId_, EM_SETLIMITTEXT, 24, 0);
    applyPresence_ = makeButton(content, L"APPLY / RECONNECT", kApplyPresenceId);
    presenceStatus_ = makeControl(content, 0, L"STATIC", L"",
        SS_LEFT, kPresenceStatusId);
    listen_ = makeButton(content, L"OPEN GODENplayer LISTEN", kListenId);
    daw_ = makeButton(content, L"OPEN DAW MODE", kDawId);
    for (const auto control : {identity_, fallbackButton_, fallbackValue_, motion_,
            listenEffects_, listenEffectsStatus_, albumAmbient_,
            albumAmbientStatus_, keepPlaying_, keepMode_,
            lyricsOnline_, lyricsOffline_, presence_, presencePaused_,
            presenceCovers_, coverProvider_,relayEndpoint_,relayCredential_,coverDisclosure_,
            applicationId_, applyPresence_, presenceStatus_,
            listen_, daw_}) viewport_.trackControl(control);
    updateFont();
    updateLayout();
    refreshAll();
}

void OptionsWindow::updateLayout()
{
    if (window_ == nullptr) return;
    RECT client{};
    GetClientRect(window_, &client);
    const auto headerHeight = dip(window_, 56);
    const auto footerHeight = dip(window_, 56);
    const auto clientWidth = static_cast<int>(client.right);
    const auto clientHeight = static_cast<int>(client.bottom);
    const auto preservedScroll = viewport_.scrollOffset();
    viewport_.scrollTo(0);
    viewport_.setBounds(0, headerHeight, clientWidth,
        (std::max)(1, clientHeight - headerHeight - footerHeight),
        dip(window_, 820));
    RECT contentClient{};
    GetClientRect(viewport_.content(), &contentClient);
    const auto dpiValue = static_cast<int>((std::max)(96U,
        GetDpiForWindow(window_)));
    const auto widthDip = MulDiv(static_cast<int>(contentClient.right), 96,
        dpiValue);
    const auto paddingDip = 26;
    const auto usableDip = (std::max)(1, widthDip - paddingDip * 2);
    const auto buttonDip = (std::min)(150, (std::max)(96, usableDip / 3));
    const auto comboDip = (std::min)(210, (std::max)(130, usableDip / 2));
    auto px = [&](int value) { return MulDiv(value, dpiValue, 96); };
    auto place = [&](HWND control, const UiFormRect& rect,
            int = -1) {
        move(control, px(rect.x), px(rect.y), px(rect.width),
            px(rect.height));
    };
    UiFormFlow flow(12);
    auto section = [&](std::size_t index) {
        place(sectionLabels_[index], flow.take(
            paddingDip, usableDip, 24, 12));
    };
    auto fullRow = [&](HWND control, int gap = 12) {
        place(control, flow.take(paddingDip, usableDip, 30, gap));
    };

    section(0);
    fullRow(identity_);
    auto row = flow.take(paddingDip, usableDip, 30, 12);
    place(fallbackButton_, {row.x, row.y, buttonDip, row.height});
    place(fallbackValue_, {row.x + buttonDip + 12, row.y,
        (std::max)(1, row.width - buttonDip - 12), row.height});
    row = flow.take(paddingDip, usableDip, 30, 12);
    place(motion_, {row.x, row.y, comboDip, row.height}, 220);
    place(motionLabel_, {row.x + comboDip + 12, row.y,
        (std::max)(1, row.width - comboDip - 12), row.height});
    row = flow.take(paddingDip, usableDip, 30, 20);
    place(listenEffects_, {row.x, row.y, comboDip, row.height}, 120);
    place(listenEffectsStatus_, {row.x + comboDip + 12, row.y,
        (std::max)(1, row.width - comboDip - 12), row.height});
    row = flow.take(paddingDip, usableDip, 30, 20);
    place(albumAmbient_, {row.x, row.y, comboDip, row.height}, 180);
    place(albumAmbientStatus_, {row.x + comboDip + 12, row.y,
        (std::max)(1, row.width - comboDip - 12), row.height});

    section(1);
    fullRow(keepPlaying_);
    row = flow.take(paddingDip, usableDip, 30, 20);
    place(keepMode_, {(widthDip - (std::min)(330, usableDip)) / 2,
        row.y, (std::min)(330, usableDip), row.height}, 220);

    section(2);
    fullRow(lyricsOnline_);
    fullRow(lyricsOffline_, 20);

    section(3);
    fullRow(presence_);
    fullRow(presencePaused_);
    fullRow(presenceCovers_);
    row=flow.take(paddingDip,usableDip,30,8);place(coverProvider_,row,180);
    fullRow(coverDisclosure_);
    fullRow(relayEndpoint_);
    fullRow(relayCredential_);
    place(applicationLabel_, flow.take(paddingDip, usableDip, 20, 4));
    row = flow.take(paddingDip, usableDip, 30, 10);
    place(applicationId_, {row.x, row.y,
        (std::max)(1, row.width - buttonDip - 10), row.height});
    place(applyPresence_, {row.x + row.width - buttonDip, row.y,
        buttonDip, row.height});
    fullRow(presenceStatus_, 8);
    place(presenceHint_, flow.take(paddingDip, usableDip, 42, 20));

    section(4);
    row = flow.take(paddingDip, usableDip, 30, 24);
    const auto toolWidth = (std::max)(1, (row.width - 12) / 2);
    place(daw_, {row.x, row.y, toolWidth, row.height});
    place(listen_, {row.x + toolWidth + 12, row.y, toolWidth, row.height});

    viewport_.setBounds(0, headerHeight, clientWidth,
        (std::max)(1, clientHeight - headerHeight - footerHeight),
        px(flow.cursor()));
    move(title_, dip(window_, 26), dip(window_, 14),
        clientWidth - dip(window_, 52), dip(window_, 30));
    const auto footerTop = clientHeight - footerHeight;
    const auto closeWidth = dip(window_, 112);
    move(footerStatus_, dip(window_, 26), footerTop + dip(window_, 13),
        (std::max)(1, clientWidth - dip(window_, 64) - closeWidth),
        dip(window_, 30));
    move(closeButton_, clientWidth - dip(window_, 26) - closeWidth,
        footerTop + dip(window_, 12), closeWidth, dip(window_, 30));
    viewport_.scrollTo(preservedScroll);
}

void OptionsWindow::updateFont()
{
    if (font_ != nullptr) DeleteObject(font_);
    if (headingFont_ != nullptr) DeleteObject(headingFont_);
    const auto dpiValue = window_ != nullptr
        ? (std::max)(96U, GetDpiForWindow(window_)) : 96U;
    font_ = CreateFontW(-MulDiv(10, dpiValue, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    headingFont_ = CreateFontW(-MulDiv(13, dpiValue, 72), 0, 0, 0, FW_SEMIBOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (const auto control : {identity_, fallbackButton_, fallbackValue_, motion_,
            listenEffects_, listenEffectsStatus_, albumAmbient_,
            albumAmbientStatus_,
            keepPlaying_, keepMode_, lyricsOnline_, lyricsOffline_, presence_,
            presencePaused_, presenceCovers_, coverProvider_, relayEndpoint_,
            relayCredential_, coverDisclosure_,
            applicationId_,
            applyPresence_, presenceStatus_, listen_, daw_, motionLabel_,
            applicationLabel_, presenceHint_, footerStatus_, closeButton_}) {
        if (control != nullptr) SendMessageW(control, WM_SETFONT,
            reinterpret_cast<WPARAM>(font_), TRUE);
    }
    if (title_ != nullptr) SendMessageW(title_, WM_SETFONT,
        reinterpret_cast<WPARAM>(headingFont_), TRUE);
    for (const auto control : sectionLabels_) {
        if (control != nullptr) SendMessageW(control, WM_SETFONT,
            reinterpret_cast<WPARAM>(font_), TRUE);
    }
}

void OptionsWindow::refreshAll()
{
    SendMessageW(identity_, BM_SETCHECK,
        headerWordmarkEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(motion_, CB_SETCURSEL,
        static_cast<WPARAM>(motionPreference()), 0);
    SendMessageW(listenEffects_, CB_SETCURSEL,
        static_cast<WPARAM>(listenEffectsMode()), 0);
    SendMessageW(albumAmbient_, CB_SETCURSEL,
        static_cast<WPARAM>(albumAmbientMode()), 0);
    const auto& keep = KeepPlayingService::instance();
    SendMessageW(keepPlaying_, BM_SETCHECK,
        keep.enabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(keepMode_, CB_SETCURSEL,
        keep.mode() == KeepPlayingMode::sequentialAlbums ? 1 : 0, 0);
    const auto& lyrics = LyricsService::instance();
    SendMessageW(lyricsOnline_, BM_SETCHECK,
        lyrics.onlineEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(lyricsOffline_, BM_SETCHECK,
        lyrics.offlineMode() ? BST_CHECKED : BST_UNCHECKED, 0);
    const auto presence = DiscordPresenceService::instance().settings();
    SendMessageW(coverProvider_,CB_SETCURSEL,static_cast<WPARAM>(presence.coverProvider),0);
    SetWindowTextW(relayEndpoint_,utf8ToWide(presence.relayEndpoint).c_str());
    SetWindowTextW(relayCredential_,L"");
    SendMessageW(coverDisclosure_,BM_SETCHECK,presence.coverDisclosureAccepted?BST_CHECKED:BST_UNCHECKED,0);
    const bool privateRelay=presence.coverProvider==PresenceCoverProvider::privateRelay;
    EnableWindow(relayEndpoint_,privateRelay); EnableWindow(relayCredential_,privateRelay);

    SendMessageW(presence_, BM_SETCHECK,
        presence.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(presencePaused_, BM_SETCHECK,
        presence.showPaused ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(presenceCovers_, BM_SETCHECK,
        presence.temporaryCovers ? BST_CHECKED : BST_UNCHECKED, 0);
    applicationIdText_ = utf8ToWide(presence.applicationId);
    SetWindowTextW(applicationId_, applicationIdText_.c_str());
    for (const auto control : {presencePaused_, presenceCovers_,
            applicationId_, applyPresence_}) {
        EnableWindow(control, presence.enabled ? TRUE : FALSE);
    }
    updateFallbackLabel();
    refreshListenEffectsStatus();
    refreshAlbumAmbientStatus();
    refreshPresenceStatus();
}

void OptionsWindow::refreshListenEffectsStatus()
{
    if (listenEffectsStatus_ == nullptr) return;
    const auto diagnostics = listenEffectsDiagnostics();
    std::wstring value;
    if (diagnostics.mode == ListenEffectsMode::noEffects) {
        value = L"GPU pass bypassed";
    } else if (diagnostics.fallback ==
            ListenEffectsFallback::softwareRasterizer) {
        value = L"Base scope active \u00B7 WARP fallback";
    } else if (diagnostics.fallback == ListenEffectsFallback::unavailable) {
        value = L"Base scope active \u00B7 shader unavailable";
    } else if (diagnostics.gpuMeasured) {
        wchar_t text[128]{};
        std::swprintf(text, std::size(text),
            L"GPU %.0f us \u00B7 %.1f MiB",
            diagnostics.gpuMicroseconds,
            static_cast<double>(diagnostics.resourceBytes) /
                (1024.0 * 1024.0));
        value = text;
    } else {
        value = diagnostics.submissions == 0
            ? L"Waiting for a visible scope"
            : L"Soft light active \u00B7 GPU timing pending";
    }
    wchar_t current[192]{};
    GetWindowTextW(listenEffectsStatus_, current,
        static_cast<int>(std::size(current)));
    if (value != current) SetWindowTextW(listenEffectsStatus_, value.c_str());
}

void OptionsWindow::refreshAlbumAmbientStatus()
{
    if (albumAmbientStatus_ == nullptr) return;
    const auto diagnostics = albumAmbientDiagnostics();
    std::wstring value;
    switch (diagnostics.fallback) {
    case AlbumAmbientFallback::none:
        value = Runtime::instance().playbackSnapshot().state == PlaybackState::playing
            ? L"Flow ambience active" : L"Flow ambience \u00b7 playback paused or stopped";
        break;
    case AlbumAmbientFallback::disabled: value = L"Static ambient \u00b7 Off"; break;
    case AlbumAmbientFallback::rolloutDisabled: value = L"Static ambient \u00b7 QA rollout disabled"; break;
    case AlbumAmbientFallback::reducedMotion: value = L"Static ambient \u00b7 Reduced Motion"; break;
    case AlbumAmbientFallback::softwareRasterizer: value = L"Static ambient \u00b7 software renderer"; break;
    case AlbumAmbientFallback::noArtwork: value = L"Static ambient \u00b7 no artwork"; break;
    case AlbumAmbientFallback::unavailable: value = L"Static ambient \u00b7 effect unavailable"; break;
    case AlbumAmbientFallback::hidden: value = L"Static ambient \u00b7 Album hidden"; break;
    case AlbumAmbientFallback::sourceStale: value = L"Flow ambience \u00b7 waiting for fresh audio"; break;
    }
    if (a4::style() != a4::Style::flow31) {
        value = L"Flow ambience \u00b7 selected visual uses its own renderer";
    } else if (diagnostics.fallback == AlbumAmbientFallback::none) {
        value += diagnostics.degraded ? L" \u00b7 up to 320\u00d7180" : L" \u00b7 up to 512\u00d7288";
        if (diagnostics.gpuMeasured) {
            wchar_t timing[64]{};
            swprintf_s(timing, L" \u00b7 GPU p95 %.3f ms",
                diagnostics.gpuP95Microseconds / 1000.0);
            value += timing;
        }
    }
    wchar_t current[192]{};
    GetWindowTextW(albumAmbientStatus_, current,
        static_cast<int>(std::size(current)));
    if (value != current) SetWindowTextW(albumAmbientStatus_, value.c_str());
}

void OptionsWindow::refreshPresenceStatus()
{
    if (presenceStatus_ == nullptr) return;
    const auto value = DiscordPresenceService::instance().statusText();
    wchar_t current[256]{};
    GetWindowTextW(presenceStatus_, current,
        static_cast<int>(std::size(current)));
    if (value != current) SetWindowTextW(presenceStatus_, value.c_str());
}

void OptionsWindow::updateFallbackLabel()
{
    const auto color = fallbackAccentColor();
    const auto byte = [](float value) {
        return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    const auto text = L"Fallback signature \u00b7 RGB " +
        std::to_wstring(byte(color.red)) + L", " +
        std::to_wstring(byte(color.green)) + L", " +
        std::to_wstring(byte(color.blue));
    SetWindowTextW(fallbackValue_, text.c_str());
}

void OptionsWindow::drawButton(const DRAWITEMSTRUCT& item)
{
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool hovered = !disabled &&
        GetPropW(item.hwndItem, kHoverProperty) != nullptr;
    const auto fillColor = disabled ? kPanel :
        pressed ? blend(kSurface, kAccent, 0.62f) :
        hovered ? blend(kSurface, kAccent, 0.30f) : kSurface;
    HBRUSH fill = CreateSolidBrush(fillColor);
    FillRect(item.hDC, &item.rcItem, fill);
    DeleteObject(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, focused ? kAccent : kBorder);
    const auto oldPen = SelectObject(item.hDC, pen);
    const auto oldBrush = SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
    Rectangle(item.hDC, item.rcItem.left, item.rcItem.top,
        item.rcItem.right, item.rcItem.bottom);
    SelectObject(item.hDC, oldBrush);
    SelectObject(item.hDC, oldPen);
    DeleteObject(pen);
    wchar_t label[64]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? kMuted :
        pressed ? RGB(238, 250, 250) :
        focused || hovered ? kAccent : kText);
    SelectObject(item.hDC, font_);
    auto bounds = item.rcItem;
    if (pressed) {
        const auto shift = MulDiv(1,
            static_cast<int>((std::max)(96U, GetDpiForWindow(item.hwndItem))),
            144);
        OffsetRect(&bounds, shift, shift);
    }
    DrawTextW(item.hDC, label, -1, &bounds,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

bool OptionsWindow::toggleCheckbox(HWND control)
{
    if (control == nullptr) return false;
    const auto checked = SendMessageW(control, BM_GETCHECK, 0, 0) ==
        BST_CHECKED;
    SendMessageW(control, BM_SETCHECK,
        checked ? BST_UNCHECKED : BST_CHECKED, 0);
    InvalidateRect(control, nullptr, FALSE);
    return !checked;
}

void OptionsWindow::drawCheckbox(const DRAWITEMSTRUCT& item)
{
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool hovered = !disabled &&
        GetPropW(item.hwndItem, kHoverProperty) != nullptr;
    if (pressed || hovered) {
        HBRUSH stateFill = CreateSolidBrush(blend(
            kBackground, kAccent, pressed ? 0.62f : 0.30f));
        FillRect(item.hDC, &item.rcItem, stateFill);
        DeleteObject(stateFill);
    } else {
        FillRect(item.hDC, &item.rcItem, backgroundBrush_);
    }
    SetBkMode(item.hDC, TRANSPARENT);
    SelectObject(item.hDC, font_);

    const auto scale = static_cast<int>((std::max)(96U,
        GetDpiForWindow(item.hwndItem)));
    const auto edge = MulDiv(14, scale, 96);
    const auto left = MulDiv(1, scale, 96);
    const auto top = item.rcItem.top +
        ((item.rcItem.bottom - item.rcItem.top) - edge) / 2;
    RECT box{left, top, left + edge, top + edge};
    HBRUSH boxFill = CreateSolidBrush(kSurface);
    FillRect(item.hDC, &box, boxFill);
    DeleteObject(boxFill);
    HPEN border = CreatePen(PS_SOLID, 1,
        (item.itemState & ODS_FOCUS) != 0 ? kAccent : kBorder);
    const auto previousPen = SelectObject(item.hDC, border);
    const auto previousBrush = SelectObject(
        item.hDC, GetStockObject(NULL_BRUSH));
    Rectangle(item.hDC, box.left, box.top, box.right, box.bottom);

    if (SendMessageW(item.hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        HPEN check = CreatePen(PS_SOLID,
            (std::max)(1, MulDiv(2, scale, 96)), kAccent);
        SelectObject(item.hDC, check);
        MoveToEx(item.hDC, box.left + edge / 5,
            box.top + edge / 2, nullptr);
        LineTo(item.hDC, box.left + edge * 2 / 5,
            box.bottom - edge / 5);
        LineTo(item.hDC, box.right - edge / 6,
            box.top + edge / 5);
        SelectObject(item.hDC, border);
        DeleteObject(check);
    }
    SelectObject(item.hDC, previousBrush);
    SelectObject(item.hDC, previousPen);
    DeleteObject(border);

    wchar_t label[160]{};
    GetWindowTextW(item.hwndItem, label,
        static_cast<int>(std::size(label)));
    RECT textBounds = item.rcItem;
    textBounds.left = box.right + MulDiv(9, scale, 96);
    SetTextColor(item.hDC, disabled ? kMuted :
        pressed ? RGB(238, 250, 250) : hovered ? kAccent : kText);
    DrawTextW(item.hDC, label, -1, &textBounds,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if ((item.itemState & ODS_FOCUS) != 0) {
        RECT focus = textBounds;
        focus.right = (std::min)(focus.right,
            focus.left + static_cast<int>(wcslen(label)) *
                MulDiv(8, scale, 96) + MulDiv(4, scale, 96));
        InflateRect(&focus, -1, -MulDiv(5, scale, 96));
        DrawFocusRect(item.hDC, &focus);
    }
}

void OptionsWindow::drawComboItem(const DRAWITEMSTRUCT& item)
{
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    HBRUSH fill = CreateSolidBrush(selected ? RGB(29, 36, 36) : kSurface);
    FillRect(item.hDC, &item.rcItem, fill);
    DeleteObject(fill);
    if (item.itemID == static_cast<UINT>(-1)) return;
    wchar_t text[256]{};
    SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID,
        reinterpret_cast<LPARAM>(text));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? kMuted :
        (selected ? RGB(232, 245, 245) : kText));
    SelectObject(item.hDC, font_);
    auto bounds = item.rcItem;
    bounds.left += dip(window_, 8);
    bounds.right -= dip(window_, 8);
    DrawTextW(item.hDC, text, -1, &bounds,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void OptionsWindow::paint(HDC external)
{
    PAINTSTRUCT state{};
    const auto dc = external != nullptr ? external : BeginPaint(window_, &state);
    if (dc == nullptr) return;
    RECT client{};
    GetClientRect(window_, &client);
    FillRect(dc, &client, backgroundBrush_);
    const auto headerHeight = dip(window_, 56);
    const auto footerTop = client.bottom - dip(window_, 56);
    HPEN line = CreatePen(PS_SOLID, 1, kBorder);
    const auto oldPen = SelectObject(dc, line);
    MoveToEx(dc, 0, headerHeight - 1, nullptr);
    LineTo(dc, client.right, headerHeight - 1);
    MoveToEx(dc, 0, footerTop, nullptr);
    LineTo(dc, client.right, footerTop);
    SelectObject(dc, oldPen);
    DeleteObject(line);
    if (external == nullptr) EndPaint(window_, &state);
}

void OptionsWindow::releaseResources()
{
    viewport_.destroy();
    if (font_ != nullptr) DeleteObject(font_);
    if (headingFont_ != nullptr) DeleteObject(headingFont_);
    if (backgroundBrush_ != nullptr) DeleteObject(backgroundBrush_);
    if (panelBrush_ != nullptr) DeleteObject(panelBrush_);
    if (editBrush_ != nullptr) DeleteObject(editBrush_);
    font_ = nullptr;
    headingFont_ = nullptr;
    backgroundBrush_ = nullptr;
    panelBrush_ = nullptr;
    editBrush_ = nullptr;
    identity_ = fallbackButton_ = fallbackValue_ = motion_ = nullptr;
    listenEffects_ = listenEffectsStatus_ = nullptr;
    keepPlaying_ = keepMode_ = nullptr;
    lyricsOnline_ = lyricsOffline_ = nullptr;
    presence_ = presencePaused_ = presenceCovers_ = applicationId_ =
        applyPresence_ = nullptr;
    coverProvider_ = relayEndpoint_ = relayCredential_ = coverDisclosure_ = nullptr;
    presenceStatus_ = listen_ = daw_ = nullptr;
    title_ = motionLabel_ = applicationLabel_ = presenceHint_ = nullptr;
    footerStatus_ = closeButton_ = nullptr;
    sectionLabels_.fill(nullptr);
    applicationIdText_.clear();
}

} // namespace nekorox::component
