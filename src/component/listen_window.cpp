#include "qa_hooks.h"
#include "listen_window.h"
#include "rollout_flags.h"

#include "analysis_controller.h"
#include "database_service.h"
#include "display_environment.h"
#include "file_open_service.h"
#include "frameless_window.h"
#include "glyph_flow_service.h"
#include "interaction_latency_guard.h"
#include "packet_cost_controller.h"
#include "runtime.h"
#include "text_util.h"
#include "theme_service.h"
#include "ui_invalidation_bus.h"
#include "ui_preferences.h"

#include "nekorox/qa_contracts.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <string>

namespace nekorox::component {
namespace {

constexpr wchar_t kWindowClass[] = L"NekoRoXListenWindowV3";
constexpr UINT kRefreshMessage = WM_APP + 0x31;
constexpr UINT kArtworkReadyMessage = WM_APP + 0x32;
constexpr UINT kCrtFrameReadyMessage = WM_APP + 0x33;
constexpr UINT kFrameClockMessage = WM_APP + 0x34;
constexpr UINT kArtworkInvalidatedMessage = WM_APP + 0x35;
constexpr UINT kThemeTransitionMessage = WM_APP + 0x36;
constexpr UINT_PTR kAnalysisDwellTimer = 0x4e4b;
constexpr UINT kAnalysisDwellWakeMessage = WM_APP + 0x4d;
constexpr UINT_PTR kMusicMapCursorTimer = 0x4e4c;
constexpr UINT_PTR kCrtTailTimer = 0x4e4d;
constexpr UINT kCrtTailMilliseconds = 500;

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

UINT listenCaptureMessage() noexcept
{
    static const UINT message = RegisterWindowMessageW(
        nekorox::kListenCaptureMessageV1);
    return message;
}

UINT rootPresentationVisibilityMessage() noexcept
{
    static const auto message = RegisterWindowMessageW(
        L"Goden.RootPresentationVisibility.V1");
    return message;
}

UINT listenCrtStatsMessage() noexcept
{
    static const UINT message = RegisterWindowMessageW(
        nekorox::kListenCrtStatsMessageV1);
    return message;
}

UINT listenPresentationStateMessage() noexcept
{
    static const auto message = RegisterWindowMessageW(L"NekoRoX.ListenPresentationState.V1");
    return message;
}

UINT listenAnalysisStateMessage() noexcept
{
    static const UINT message = RegisterWindowMessageW(
        nekorox::kListenAnalysisStateMessageV1);
    return message;
}

UINT listenArtworkStateMessage() noexcept
{
    static const UINT message = RegisterWindowMessageW(
        nekorox::kListenArtworkStateMessageV1);
    return message;
}

UINT listenScopeStateMessage() noexcept
{
    static const UINT message = RegisterWindowMessageW(
        L"GodenListenScopeStateR2");
    return message;
}

const GUID kWindowX = {0x7419391c, 0x29da, 0x49ce, {0x9d, 0x20, 0x49, 0xfb, 0xbc, 0x30, 0xb0, 0x01}};
const GUID kWindowY = {0x3f3de8cf, 0x08e7, 0x40fb, {0x80, 0xa6, 0x19, 0xbd, 0x61, 0x1a, 0x5e, 0x72}};
const GUID kWindowW = {0x309cb7be, 0xc55c, 0x40d3, {0x8f, 0xf8, 0x63, 0x46, 0x08, 0xa4, 0x94, 0x13}};
const GUID kWindowH = {0x6afcf46d, 0x700c, 0x4982, {0xb3, 0xc6, 0x4e, 0x78, 0xb6, 0x13, 0x41, 0x17}};
const GUID kWindowDpi = {0x70c0a7e5, 0xf4d4, 0x479f, {0xb2, 0xd4, 0x63, 0xf0, 0xe4, 0x73, 0xef, 0x17}};
const GUID kLastSpace = {0x4de8fbc8, 0x276e, 0x4e30, {0xa7, 0xdf, 0x77, 0x34, 0xee, 0x23, 0x12, 0x8a}};
const GUID kLiveStereoMode = {0x2f06870a, 0xc911, 0x4a74, {0xa0, 0xac, 0x0e, 0x7f, 0x7c, 0x2f, 0xa6, 0x5b}};

cfg_int g_windowX(kWindowX, 140);
cfg_int g_windowY(kWindowY, 100);
cfg_int g_windowW(kWindowW, 1180);
cfg_int g_windowH(kWindowH, 760);
// Zero identifies a legacy physical-pixel rectangle. Its first load remains
// unchanged; subsequent saves carry the source DPI and can be rescaled.
cfg_int g_windowDpi(kWindowDpi, 0);
cfg_int g_lastSpace(kLastSpace, 1);
cfg_int g_liveStereoMode(kLiveStereoMode, 1);

std::uint64_t qpcNow() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
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

bool pointIn(const D2D1_RECT_F& rectangle, float x, float y) noexcept
{
    return x >= rectangle.left && x < rectangle.right &&
        y >= rectangle.top && y < rectangle.bottom;
}

UINT effectiveMonitorDpi(HMONITOR monitor, UINT fallback) noexcept
{
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    const auto shcore = LoadLibraryW(L"shcore.dll");
    if (shcore == nullptr) return (std::max)(1U, fallback);
    const auto getDpiForMonitor = reinterpret_cast<GetDpiForMonitorFn>(
        GetProcAddress(shcore, "GetDpiForMonitor"));
    UINT dpiX = fallback;
    UINT dpiY = fallback;
    if (getDpiForMonitor == nullptr ||
        FAILED(getDpiForMonitor(monitor, 0, &dpiX, &dpiY))) {
        dpiX = fallback;
    }
    FreeLibrary(shcore);
    return (std::max)(1U, dpiX);
}

std::wstring formatField(
    const metadb_handle_ptr& handle,
    const char* expression)
{
    if (!handle.is_valid()) return {};
    titleformat_object::ptr script;
    titleformat_compiler::get()->compile_safe(script, expression);
    pfc::string8_fast output;
    if (!script.is_valid() ||
        !handle->format_title(nullptr, output, script, nullptr)) {
        return {};
    }
    return trimText(utf8ToWide(output.c_str()));
}

LibraryTrack makeFallbackTrack(const metadb_handle_ptr& handle)
{
    LibraryTrack track;
    track.handle = handle;
    if (!handle.is_valid()) return track;
    track.path = utf8ToWide(handle->get_path());
    track.durationSeconds = handle->get_length();
    track.genre = formatField(handle, "$if2(%genre%,Unknown)");
    track.artist = formatField(handle, "$if2(%artist%,Unknown artist)");
    track.albumArtist = formatField(
        handle, "$if2(%album artist%,$if2(%artist%,Unknown artist))");
    track.album = formatField(handle, "$if2(%album%,Unknown album)");
    track.title = formatField(handle, "$if2(%title%,%filename%)");
    track.year = formatField(handle, "$left($if2(%date%,),4)");
    track.codec = formatField(handle, "$upper($if2(%codec%,))");
    const auto sampleRate = formatField(handle, "$if2(%samplerate%,0)");
    const auto bits = formatField(handle, "$if2(%__bitspersample%,0)");
    track.sampleRate = sampleRate.empty() ? 0 : _wtoi(sampleRate.c_str());
    track.bitsPerSample = bits.empty() ? 0 : _wtoi(bits.c_str());
    return track;
}

float interpolate(float left, float right, float amount) noexcept
{
    return left + (right - left) * amount;
}

} // namespace

ListenWindow::ListenWindow()
    : artCache_(kArtworkReadyMessage)
{
    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
        qpcFrequency_ = static_cast<double>(frequency.QuadPart);
    }
    framePeriodTicks_ = qpcFrequency_ / 120.0;
    analysisDwellWake_ = CreateThreadpoolTimer(
        &ListenWindow::analysisDwellWake, this, nullptr);
}

ListenWindow::~ListenWindow()
{
    analysisDwellWindow_.store(nullptr, std::memory_order_release);
    if (analysisDwellWake_ != nullptr) {
        SetThreadpoolTimer(analysisDwellWake_, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(analysisDwellWake_, TRUE);
        CloseThreadpoolTimer(analysisDwellWake_);
        analysisDwellWake_ = nullptr;
    }
}

void CALLBACK ListenWindow::analysisDwellWake(
    PTP_CALLBACK_INSTANCE,
    void* context,
    PTP_TIMER) noexcept
{
    auto* self = static_cast<ListenWindow*>(context);
    if (self == nullptr) return;
    const auto window = self->analysisDwellWindow_.load(
        std::memory_order_acquire);
    if (window != nullptr) {
        PostMessageW(window, kAnalysisDwellWakeMessage, 0, 0);
    }
}

ListenWindow& ListenWindow::instance()
{
    static ListenWindow listen;
    return listen;
}

void ListenWindow::show()
{
    auto& telemetry = PerformanceTelemetry::instance();
    const auto showSpan = telemetry.beginSpan(TelemetrySurface::listen, 20U);
    const auto background = qaBackgroundRequested();
    if (window_ != nullptr) {
        ShowWindow(window_, background ? SW_SHOWNOACTIVATE : SW_RESTORE);
        updateVisibilityLifecycle(true);
        if (!background) SetForegroundWindow(window_);
        telemetry.endSpan(TelemetrySurface::listen, 20U, showSpan, 1U);
        return;
    }

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC | CS_DBLCLKS;
    windowClass.lpfnWndProc = &ListenWindow::windowProc;
    windowClass.hInstance = core_api::get_my_instance();
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);

    // v1 persisted four pages. Removed pages 2/3 and invalid values migrate
    // deterministically to Live; Overview remains Overview.
    activeSpace_ = static_cast<int>(g_lastSpace) == 0 ? 0 : 1;
    crtDirectOrientation_ = static_cast<int>(g_liveStereoMode) != 0;
    crtXySurface_.setOrientation(
        crtDirectOrientation_
            ? CrtXyOrientation::directLeftRight
            : CrtXyOrientation::midSide);
    auto x = static_cast<int>(g_windowX);
    auto y = static_cast<int>(g_windowY);
    const auto storedWidth = (std::max)(1, static_cast<int>(g_windowW));
    const auto storedHeight = (std::max)(1, static_cast<int>(g_windowH));
    auto width = storedWidth;
    auto height = storedHeight;
    RECT requested{x, y, x + width, y + height};
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    const auto monitor = MonitorFromRect(
        &requested, MONITOR_DEFAULTTONEAREST);
    const auto owner = static_cast<HWND>(core_api::get_main_window());
    const auto fallbackDpi = owner != nullptr
        ? GetDpiForWindow(owner) : 96U;
    const auto targetDpi = effectiveMonitorDpi(monitor, fallbackDpi);
    const auto storedDpi = static_cast<int>(g_windowDpi) > 0
        ? static_cast<UINT>(g_windowDpi) : targetDpi;
    width = (std::max)(
        MulDiv(840, static_cast<int>(targetDpi), 96),
        MulDiv(storedWidth, static_cast<int>(targetDpi),
            static_cast<int>((std::max)(1U, storedDpi))));
    height = (std::max)(
        MulDiv(560, static_cast<int>(targetDpi), 96),
        MulDiv(storedHeight, static_cast<int>(targetDpi),
            static_cast<int>((std::max)(1U, storedDpi))));
    if (monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo)) {
        const auto& work = monitorInfo.rcWork;
        const auto workLeft = static_cast<int>(work.left);
        const auto workTop = static_cast<int>(work.top);
        const auto workRight = static_cast<int>(work.right);
        const auto workBottom = static_cast<int>(work.bottom);
        width = std::min(width, workRight - workLeft);
        height = std::min(height, workBottom - workTop);
        x = std::clamp(x, workLeft, (std::max)(workLeft, workRight - width));
        y = std::clamp(y, workTop, (std::max)(workTop, workBottom - height));
    }
    const auto createSpan = telemetry.beginSpan(TelemetrySurface::listen, 21U);
    window_ = CreateWindowExW(
        WS_EX_APPWINDOW | (background ? WS_EX_NOACTIVATE : 0U),
        kWindowClass,
        L"GODENplayer Listen",
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU |
            WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
            WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        x,
        y,
        width,
        height,
        nullptr,
        nullptr,
        core_api::get_my_instance(),
        this);
    telemetry.endSpan(TelemetrySurface::listen, 21U, createSpan);
    if (window_ == nullptr) {
        telemetry.endSpan(TelemetrySurface::listen, 20U, showSpan, 2U);
        return;
    }
    analysisDwellWindow_.store(window_, std::memory_order_release);
    enableFramelessWindow(window_);
    const auto trackSpan = telemetry.beginSpan(TelemetrySurface::listen, 22U);
    PacketCostController::instance().onTrackChanged(nativeTrackPath());
    telemetry.endSpan(TelemetrySurface::listen, 22U, trackSpan);
    const auto visibleSpan = telemetry.beginSpan(TelemetrySurface::listen, 23U);
    ShowWindow(window_, background ? SW_SHOWNOACTIVATE : SW_SHOW);
    updateVisibilityLifecycle(true);
    telemetry.endSpan(TelemetrySurface::listen, 23U, visibleSpan);
    // Do not make the caller of Listen pay for the first D2D/DXGI present.
    // WM_PAINT is naturally queued after ShowWindow; forcing it here can hold
    // the host's transport and every other child panel inside the driver.
    InvalidateRect(window_, nullptr, FALSE);
    telemetry.endSpan(TelemetrySurface::listen, 20U, showSpan);
}

void ListenWindow::close()
{
    if (window_ != nullptr) DestroyWindow(window_);
}

bool ListenWindow::occludes(HWND target) const noexcept
{
    occlusionTarget_ = target;
    targetOccluded_ = false;
    if (window_ == nullptr || target == nullptr ||
        !IsWindowVisible(window_) || IsIconic(window_)) return false;
    const auto root = GetAncestor(target, GA_ROOT);
    bool aboveTarget = false;
    for (auto below = GetWindow(window_, GW_HWNDNEXT);
            below != nullptr; below = GetWindow(below, GW_HWNDNEXT)) {
        if (below == root) {
            aboveTarget = true;
            break;
        }
    }
    if (!aboveTarget) return false;
    RECT listen{};
    RECT covered{};
    if (!GetWindowRect(window_, &listen) ||
        !GetWindowRect(target, &covered)) return false;
    const auto width = std::max<std::int64_t>(0,
        static_cast<std::int64_t>(covered.right) - covered.left);
    const auto height = std::max<std::int64_t>(0,
        static_cast<std::int64_t>(covered.bottom) - covered.top);
    const auto overlapWidth = std::max<std::int64_t>(0,
        static_cast<std::int64_t>(std::min(listen.right, covered.right)) -
        std::max(listen.left, covered.left));
    const auto overlapHeight = std::max<std::int64_t>(0,
        static_cast<std::int64_t>(std::min(listen.bottom, covered.bottom)) -
        std::max(listen.top, covered.top));
    // Listen is narrower than a maximized shell, yet still covers the
    // album's central canvas. Do not keep two live visual surfaces competing
    // for the UI thread merely because the transport/spectrum remain exposed.
    targetOccluded_ = width != 0 && height != 0 &&
        overlapWidth * overlapHeight * 3 >= width * height;
    return targetOccluded_;
}

void ListenWindow::requestRefresh()
{
    if (window_ != nullptr) {
        UiInvalidationBus::instance().publish(
            UiInvalidationSurface::listen,
            UiDirty::semantic);
    }
}

void ListenWindow::notifyArtworkChanged()
{
    if (window_ != nullptr) {
        PostMessageW(window_, kArtworkInvalidatedMessage, 0, 0);
    }
}

void ListenWindow::notifyPlaybackTrackTransition(
    const metadb_handle_ptr&)
{
    if (window_ == nullptr) return;
    // The invalidation bus deliberately coalesces semantic refreshes. Keep a
    // separate edge so a rapid A -> B -> A round trip cannot look unchanged
    // to refreshTrack() after the running analysis was already cancelled.
    trackTransitionPending_ = true;
    cancelAnalysisDwell(true);
    UiInvalidationBus::instance().publish(
        UiInvalidationSurface::listen,
        UiDirty::semantic);
}

LRESULT CALLBACK ListenWindow::windowProc(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    ListenWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<ListenWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<ListenWindow*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    return self != nullptr
        ? self->handleMessage(window, message, wParam, lParam)
        : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT ListenWindow::handleMessage(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    if (message == rootPresentationVisibilityMessage()) {
        updateVisibilityLifecycle(
            wParam != 0 &&
            DisplayEnvironmentService::instance().presentationVisible(window));
        return 0;
    }
    if (message == listenAnalysisStateMessage()) {
        const auto analysis = AnalysisController::instance().snapshot();
        auto state = static_cast<std::uint32_t>(analysis.status)
            << nekorox::kListenAnalysisStatusShiftV1;
        if (lifecycle_.visible()) {
            state |= nekorox::kListenAnalysisVisibleBitV1;
        }
        if (lifecycle_.analysisPending()) {
            state |= nekorox::kListenAnalysisPendingBitV1;
        }
        return static_cast<LRESULT>(state);
    }
    if (message == listenScopeStateMessage()) {
        const auto scope = RealtimeAnalysisHub::instance().scopeParameters();
        const auto gain = scope.gain == 0.5f ? 0U : scope.gain == 2.0f ? 2U : 1U;
        return static_cast<LRESULT>(
            (scope.windowMilliseconds & 0xffffU) |
            (gain << 16U) |
            (static_cast<std::uint32_t>(scope.mode) << 18U) |
            (static_cast<std::uint32_t>(liveFocus_) << 21U));
    }
    if (message == listenArtworkStateMessage()) {
        std::uint32_t state = 0;
        if (track_.is_valid()) {
            state |= nekorox::kListenArtworkTrackValidBitV1;
        }
        if (hasTrackInfo_) {
            state |= nekorox::kListenArtworkTrackInfoBitV1;
        }
        if (artwork_ && !artwork_->bgra.empty()) {
            state |= nekorox::kListenArtworkPixelsReadyBitV1;
        }
        if (renderer_.artworkReady()) {
            state |= nekorox::kListenArtworkRenderReadyBitV1;
        }
        if (renderer_.artworkTransitionActive()) {
            state |= nekorox::kListenArtworkTransitionActiveBitV1;
        }
        if (hasTrackInfo_) {
            switch (artCache_.status(artworkIdentity_, 720)) {
            case ArtCacheStatus::loading:
                state |= nekorox::kListenArtworkCacheLoadingBitV1;
                break;
            case ArtCacheStatus::missing:
                state |= nekorox::kListenArtworkCacheMissingBitV1;
                break;
            case ArtCacheStatus::ready:
                state |= nekorox::kListenArtworkCacheReadyBitV1;
                break;
            case ArtCacheStatus::absent:
                break;
            }
        }
        state |= static_cast<std::uint32_t>(motionMode())
            << nekorox::kListenArtworkMotionModeShiftV1;
        return static_cast<LRESULT>(state);
    }
    if (message == listenPresentationStateMessage()) {
        if (wParam >= 1001U && wParam <= 1003U) {
            if (!qa::hooksAllowed()) return E_ACCESSDENIED;
            wchar_t hooks[4]{}, profile[4]{};
            if (GetEnvironmentVariableW(L"NEKOROX_QA_HOOKS", hooks, 4) != 1 ||
                hooks[0] != L'1' ||
                GetEnvironmentVariableW(L"NEKOROX_QA_PROFILE", profile, 4) != 1 ||
                profile[0] != L'1') return 0;
            if (wParam == 1003U) {
                renderer_.queuePreparationFaultsForQa(0);
                return 1;
            }
            if (lParam < 1 || lParam > 16) return 0;
            renderer_.queuePreparationFaultsForQa(static_cast<std::uint32_t>(lParam));
            // 1002 only arms: a subsequent native DPI/paint event must drive
            // preparation. Neither selector starts the clock or rescues it.
            if (wParam == 1001U) InvalidateRect(window, nullptr, FALSE);
            return 1;
        }
        switch (wParam) {
        case 0U: return static_cast<LRESULT>(presentationRevision_.desired());
        case 1U: return static_cast<LRESULT>(presentationRevision_.accepted());
        case 2U: return presentationRevision_.pending();
        case 3U: return static_cast<LRESULT>(presentationRevision_.epoch());
        case 4U: return static_cast<LRESULT>(frameClock_.deliveredPulses());
        case 5U: return static_cast<LRESULT>(frameClock_.diagnosticDeliveryState());
        case 6U: return static_cast<LRESULT>(renderer_.bindingKey().swapchainEpoch);
        case 7U: return renderer_.bindingKey().valid();
        case 8U: return static_cast<LRESULT>(preparationAttempts_);
        case 9U: return static_cast<LRESULT>(preparationFailures_);
        case 10U: return static_cast<LRESULT>(renderer_.qaPreparationFaultCount());
        case 11U: return renderer_.qaPreparationFailuresRemaining();
        case 12U: return ThemeService::instance().transitionActive();
        case 13U: return renderer_.artworkTransitionActive();
        case 14U: return lifecycle_.visible();
        case 15U: return lifecycle_.playing();
        case 16U: return activeSpace_;
        case 17U: return renderer_.glyphTransitionActive();
        case 18U: return GlyphFlowService::instance().active(motionMode());
        default: return 0;
        }
    }
    if (message == listenCrtStatsMessage()) {
        std::filesystem::path profile;
        try {
            const auto native = filesystem::g_get_native_path(
                core_api::get_profile_path());
            profile = utf8ToWide(native.c_str());
        } catch (...) {
            return FALSE;
        }
        const auto output =
            profile / nekorox::kListenCrtStatsRelativePathV1;
        std::error_code error;
        std::filesystem::create_directories(output.parent_path(), error);
        if (error) return FALSE;
        const auto& surface = crtXySurface_.stats();
        const auto& model = crtXyModel_.stats();
        const auto listenTelemetry = PerformanceTelemetry::instance().snapshot(
            TelemetrySurface::listen);
        const auto crtTelemetry = PerformanceTelemetry::instance().snapshot(
            TelemetrySurface::listenCrt);
        std::ofstream stream(output, std::ios::trunc);
        if (!stream) return FALSE;
        stream << "{\n"
            << "  \"schema\": \"nekorox.listen-crt-stats.v1\",\n"
            << "  \"active\": "
            << (crtXySurface_.active() ? "true" : "false") << ",\n"
            << "  \"modelSamples\": " << model.samples << ",\n"
            << "  \"modelSegments\": " << model.segments << ",\n"
            << "  \"brokenPaths\": " << model.brokenPaths << ",\n"
            << "  \"submittedSegments\": "
            << surface.submittedSegments << ",\n"
            << "  \"droppedSegments\": "
            << surface.droppedSegments << ",\n"
            << "  \"presentedFrames\": "
            << surface.presentedFrames << ",\n"
            << "  \"skippedPresents\": "
            << surface.skippedPresents << ",\n"
            << "  \"pacedWaits\": "
            << surface.pacedWaits << ",\n"
            << "  \"deviceLosses\": "
            << surface.deviceLosses << ",\n"
            << "  \"settledStops\": "
            << surface.settledStops << ",\n"
            << "  \"softwareRasterizer\": "
            << (surface.softwareRasterizer ? "true" : "false") << ",\n"
            << "  \"frameClockPulses\": "
            << frameClock_.deliveredPulses() << ",\n"
            << "  \"frameClockSkippedDeadlines\": "
            << frameClock_.skippedDeadlines() << ",\n"
            << "  \"frameClockTargetRefreshMilliHz\": "
            << frameClock_.targetRefreshMilliHz() << ",\n"
            << "  \"scopeSequence\": "
            << (analysisSnapshot_
                    ? analysisSnapshot_->scope.sourceSequence
                    : 0) << ",\n"
            << "  \"scopeRenderRevision\": "
            << (analysisSnapshot_
                    ? analysisSnapshot_->scope.revision
                    : 0) << ",\n"
            << "  \"scopeLocked\": "
            << (analysisSnapshot_ && analysisSnapshot_->scope.locked
                    ? "true"
                    : "false") << ",\n"
            << "  \"scopeLockConfidence\": "
            << (analysisSnapshot_
                    ? analysisSnapshot_->scope.lockConfidence
                    : 0.0f) << ",\n"
            << "  \"scopePeriodHz\": "
            << (analysisSnapshot_
                    ? analysisSnapshot_->scope.periodHz
                    : 0.0f) << ",\n"
            << "  \"analysisProcessedFrames\": "
            << RealtimeAnalysisHub::instance().processedFrames() << ",\n"
            << "  \"analysisDroppedPublications\": "
            << RealtimeAnalysisHub::instance().droppedPublications() << ",\n"
            << "  \"listenPaints\": "
            << listenTelemetry.paints << ",\n"
            << "  \"listenUploadBytes\": "
            << listenTelemetry.uploadBytes << ",\n"
            << "  \"listenPresents\": "
            << listenTelemetry.presents << ",\n"
            << "  \"listenRefusedFrames\": "
            << listenTelemetry.refusedFrames << ",\n"
            << "  \"listenInputToPresentMaximumUs\": "
            << listenTelemetry.inputToPresentMaximumMicroseconds << ",\n"
            << "  \"crtPresentsTelemetry\": "
            << crtTelemetry.presents << ",\n"
            << "  \"crtRefusedFramesTelemetry\": "
            << crtTelemetry.refusedFrames << ",\n"
            << "  \"targetRefreshMilliHz\": "
            << surface.targetRefreshMilliHz << ",\n"
            << "  \"outputWidth\": " << surface.outputWidth << ",\n"
            << "  \"outputHeight\": " << surface.outputHeight << ",\n"
            << "  \"renderWidth\": " << surface.renderWidth << ",\n"
            << "  \"renderHeight\": " << surface.renderHeight << "\n"
            << "}\n";
        return stream.good() ? TRUE : FALSE;
    }
    if (message == listenCaptureMessage()) {
        RECT client{};
        if (!GetClientRect(window, &client)) return FALSE;
        std::filesystem::path profile;
        try {
            const auto native = filesystem::g_get_native_path(
                core_api::get_profile_path());
            profile = utf8ToWide(native.c_str());
        } catch (...) {
            return FALSE;
        }
        const auto output = profile / nekorox::kListenCaptureRelativePathV1;
        std::error_code error;
        std::filesystem::create_directories(output.parent_path(), error);
        if (error) return FALSE;
        CrtSurfaceCapture crtCapture;
        ListenBgraOverlay crtOverlay;
        const ListenBgraOverlay* overlay = nullptr;
        if (activeSpace_ == 1 &&
            crtXySurface_.captureBgra(crtCapture)) {
            crtOverlay.pixelRect = crtCapture.pixelRect;
            crtOverlay.width = crtCapture.width;
            crtOverlay.height = crtCapture.height;
            crtOverlay.bgra = crtCapture.bgra;
            overlay = &crtOverlay;
        }
        return renderer_.captureBmp(
            output,
            static_cast<std::uint32_t>(client.right - client.left),
            static_cast<std::uint32_t>(client.bottom - client.top),
            static_cast<float>(GetDpiForWindow(window)),
            renderState(),
            overlay) ? TRUE : FALSE;
    }
    switch (message) {
    case WM_NCCALCSIZE:
        return 0;
    case WM_NCHITTEST: {
        const auto resize = framelessResizeHitTest(window, lParam);
        return resize != HTCLIENT ? resize : HTCLIENT;
    }
    case WM_NCACTIVATE:
        return TRUE;
    case WM_NCPAINT:
        return 0;
    case WM_CREATE:
        window_ = window;
        monitorIdentity_.reset();
        monitorIdentity_.update(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
            GetDpiForWindow(window));
        DisplayEnvironmentService::instance().registerPersistentRoot(window);
        UiInvalidationBus::instance().registerWindow(
            UiInvalidationSurface::listen,
            window,
            kRefreshMessage);
        frameClock_.attach(window, kFrameClockMessage);
        frameClock_.setTelemetrySurface(TelemetrySurface::listen);
        ThemeService::instance().subscribe(
            window, kThemeTransitionMessage);
        GlyphFlowService::instance().subscribe(
            window, kThemeTransitionMessage);
        DragAcceptFiles(window, TRUE);
        artCache_.attach(window);
        crtXySurface_.attach(window, kCrtFrameReadyMessage);
        lifecycle_.setPlaying(
            Runtime::instance().playbackSnapshot().state ==
                PlaybackState::playing,
            qpcNow(),
            static_cast<std::uint64_t>(qpcFrequency_));
        {
            auto& telemetry = PerformanceTelemetry::instance();
            const auto started = telemetry.beginSpan(
                TelemetrySurface::listen, 25U);
            refreshTrack(true);
            telemetry.endSpan(TelemetrySurface::listen, 25U, started);
        }
        return 0;
    case WM_SHOWWINDOW:
        updateVisibilityLifecycle(wParam != FALSE && !IsIconic(window));
        UiInvalidationBus::instance().publish(
            UiInvalidationSurface::nativePanels, UiDirty::semantic);
        return 0;
    case WM_ACTIVATE:
        UiInvalidationBus::instance().publish(
            UiInvalidationSurface::nativePanels, UiDirty::semantic);
        break;
    case WM_DROPFILES:
        openDroppedFiles(window, reinterpret_cast<HDROP>(wParam));
        return 0;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT:
        paint();
        return 0;
    case WM_PRINTCLIENT: {
        RECT client{};
        GetClientRect(window, &client);
        renderer_.drawToDc(
            reinterpret_cast<HDC>(wParam),
            client,
            static_cast<float>(GetDpiForWindow(window)),
            renderState());
        return 0;
    }
    case WM_SIZE:
        renderer_.resize(LOWORD(lParam), HIWORD(lParam));
        frameClock_.setReadinessBinding(renderer_.bindingKey());
        presentationRevision_.invalidate();
        if (wParam == SIZE_MINIMIZED) {
            updateVisibilityLifecycle(false);
        } else {
            updateVisibilityLifecycle(IsWindowVisible(window) != FALSE);
            syncCrtSurface();
            syncFrameClock();
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_WINDOWPOSCHANGED:
        DisplayEnvironmentService::instance().notifyWindowChanged(window);
        refreshMonitorIdentity(window);
        crtXySurface_.displayEnvironmentChanged();
        updateVisibilityLifecycle(
            IsWindowVisible(window) != FALSE && IsIconic(window) == FALSE);
        // Wake Album only when its occlusion actually changes. A long drag
        // must not rebuild every native panel at pointer cadence.
        if (IsWindow(occlusionTarget_)) {
            const bool wasOccluded = targetOccluded_;
            if (occludes(occlusionTarget_) != wasOccluded)
                UiInvalidationBus::instance().publish(
                    UiInvalidationSurface::nativePanels, UiDirty::semantic);
        }
        break;
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(window, nullptr,
            suggested->left, suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        frameClock_.setReadinessBinding({});
        renderer_.discard();
        presentationRevision_.deviceEpochChanged();
        DisplayEnvironmentService::instance().notifyWindowChanged();
        crtXySurface_.setActive(false);
        crtXySurface_.displayEnvironmentChanged(true);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_DISPLAYCHANGE:
    case WM_DEVICECHANGE:
    case WM_SETTINGCHANGE:
        DisplayEnvironmentService::instance().notifyWindowChanged();
        renderer_.displayEnvironmentChanged();
        crtXySurface_.displayEnvironmentChanged(true);
        frameClock_.requestImmediate();
        InvalidateRect(window, nullptr, FALSE);
        break;
    case WM_GETMINMAXINFO: {
        applyFramelessMinMax(window, lParam);
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        applyDpiAwareMinimum(window, limits, 760, 520);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        PerformanceTelemetry::instance().input(TelemetrySurface::listen);
        SetFocus(window);
        const auto point = pointInDip(lParam);
        if (pointIn(renderer_.minimizeRect(), point.x, point.y)) {
            ShowWindow(window, SW_MINIMIZE);
        } else if (pointIn(renderer_.maximizeRect(), point.x, point.y)) {
            toggleFramelessMaximize(window);
        } else if (pointIn(renderer_.closeRect(), point.x, point.y)) {
            PostMessageW(window, WM_CLOSE, 0, 0);
        } else if (pointIn(renderer_.captionRect(), point.x, point.y)) {
            beginFramelessMove(window);
        } else if (pointIn(renderer_.playPauseRect(), point.x, point.y)) {
            requestPlaybackPauseToggle();
        } else if (pointIn(renderer_.transportSeekRect(), point.x, point.y)) {
            const auto session = ScrubCoordinator::instance().begin(normalizedSeekPoint(point.x));
            if (session != 0) {
                scrubSession_ = session;
                seeking_ = true;
                scrubDragging_ = false;
                scrubPointerDownX_ = point.x;
                SetCapture(window);
                syncFrameClock();
                InvalidateRect(window, nullptr, FALSE);
            }
        } else if (point.y >= 30.0f && point.y < 72.0f) {
            RECT client{};
            GetClientRect(window, &client);
            const auto widthDip = static_cast<float>(client.right) *
                96.0f / static_cast<float>(std::max<UINT>(1, GetDpiForWindow(window)));
            const auto nextSpace = std::clamp(
                static_cast<int>(point.x * 2.0f / std::max(1.0f, widthDip)),
                0, 1);
            ListenCommandV1 command;
            command.kind = ListenCommandKind::navigate;
            command.index = static_cast<std::uint32_t>(nextSpace);
            dispatchCommand(command);
        } else if (activeSpace_ == 1) {
            const auto layout = currentLiveLayout();
            const std::array<std::pair<ListenLiveFocus, const ListenPaneLayout*>, 4> panes{{
                {ListenLiveFocus::scope, &layout.scope},
                {ListenLiveFocus::crt, &layout.crt},
                {ListenLiveFocus::dynamics, &layout.dynamics},
                {ListenLiveFocus::spectrogram, &layout.spectrogram},
            }};
            for (const auto& [focus, pane] : panes) {
                if (!pane->visible || !pointIn(pane->focusHit, point.x, point.y)) continue;
                setLiveFocus(
                    liveFocus_ == focus ? ListenLiveFocus::none : focus);
                return 0;
            }
            if (pointIn(renderer_.dynamicsHoldRect(), point.x, point.y)) {
                setDynamicsHold(!dynamicsHold_);
                return 0;
            }
            if (pointIn(renderer_.liveModeRect(), point.x, point.y)) {
                setCrtDirectOrientation(!crtDirectOrientation_);
                return 0;
            }
        } else if (activeSpace_ == 0 &&
                   pointIn(renderer_.favoriteRect(), point.x, point.y)) {
            ListenCommandV1 command;
            command.kind = ListenCommandKind::toggleFavorite;
            dispatchCommand(command);
        } else if (activeSpace_ == 0 &&
                   pointIn(renderer_.analyzeRect(), point.x, point.y)) {
            startOrCancelAnalysis();
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        const auto point = pointInDip(lParam);
        if (pointIn(renderer_.captionRect(), point.x, point.y)) {
            toggleFramelessMaximize(window);
        } else if (activeSpace_ == 1) {
            const auto layout = currentLiveLayout();
            if (layout.scope.visible && pointIn(layout.scope.frame, point.x, point.y))
                setLiveFocus(ListenLiveFocus::scope);
            else if (layout.crt.visible && pointIn(layout.crt.frame, point.x, point.y))
                setLiveFocus(ListenLiveFocus::crt);
            else if (layout.dynamics.visible && pointIn(layout.dynamics.frame, point.x, point.y))
                setLiveFocus(ListenLiveFocus::dynamics);
            else if (layout.spectrogram.visible && pointIn(layout.spectrogram.frame, point.x, point.y))
                setLiveFocus(ListenLiveFocus::spectrogram);
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        const auto point = pointInDip(lParam);
        if (pointIn(renderer_.captionRect(), point.x, point.y)) {
            POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(window, &screenPoint);
            showFramelessSystemMenu(window, screenPoint);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (seeking_) {
            const auto point = pointInDip(lParam);
            ScrubCoordinator::instance().end(
                scrubSession_, normalizedSeekPoint(point.x));
            seeking_ = false;
            scrubDragging_ = false;
            if (GetCapture() == window) ReleaseCapture();
            syncFrameClock();
        }
        return 0;
    case WM_MOUSEMOVE: {
        const auto point = pointInDip(lParam);
        const auto wasInCaption = pointerVisible_ && pointerY_ < 31.0f;
        pointerX_ = point.x;
        pointerY_ = point.y;
        pointerVisible_ = true;
        if (!trackingPointer_) {
            TRACKMOUSEEVENT tracking{
                sizeof(tracking), TME_LEAVE, window, HOVER_DEFAULT};
            TrackMouseEvent(&tracking);
            trackingPointer_ = true;
        }
        if (seeking_) {
            if (!scrubDragging_ &&
                std::abs(point.x - scrubPointerDownX_) >= 3.0f) {
                scrubDragging_ = true;
            }
            if (scrubDragging_) {
                ScrubCoordinator::instance().update(
                    scrubSession_, normalizedSeekPoint(point.x));
            }
        }
        if (activeSpace_ == 0 || seeking_ ||
            point.y < 31.0f || wasInCaption) {
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        pointerVisible_ = false;
        trackingPointer_ = false;
        if (activeSpace_ == 0) {
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (seeking_) { ScrubCoordinator::instance().cancel(scrubSession_); scrubSession_ = 0; }
        seeking_ = false;
        scrubDragging_ = false;
        return 0;
    case WM_CANCELMODE:
        if (seeking_) { ScrubCoordinator::instance().cancel(scrubSession_); scrubSession_ = 0; }
        seeking_ = false;
        scrubDragging_ = false;
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_SPACE && !(GetKeyState(VK_CONTROL) & 0x8000) &&
                !(GetKeyState(VK_MENU) & 0x8000)) {
            if (!(lParam & (1LL << 30))) requestPlaybackPauseToggle();
            return 0;
        }
        if (wParam >= '1' && wParam <= '2') {
            ListenCommandV1 command;
            command.kind = ListenCommandKind::navigate;
            command.index = static_cast<std::uint32_t>(wParam - '1');
            dispatchCommand(command);
            return 0;
        }
        if (wParam == 'X' && activeSpace_ == 1) {
            setCrtDirectOrientation(!crtDirectOrientation_);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            if (liveFocus_ != ListenLiveFocus::none) {
                setLiveFocus(ListenLiveFocus::none);
                return 0;
            }
            if (seeking_) {
                ScrubCoordinator::instance().cancel(scrubSession_);
                scrubSession_ = 0;
                seeking_ = false;
                scrubDragging_ = false;
                if (GetCapture() == window) ReleaseCapture();
                syncFrameClock();
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            DestroyWindow(window);
            return 0;
        }
        break;
    case kFrameClockMessage:
        if (frameClock_.consume(wParam)) {
            tick();
            // A synchronous UpdateWindow here turns every Listen frame into
            // a nested paint inside the posted clock callback. Let the UI
            // queue dispatch paint after transport and pointer messages.
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_TIMER:
        if (wParam == kAnalysisDwellTimer) {
            handleAnalysisDwellTimer();
            return 0;
        }
        if (wParam == kCrtTailTimer) {
            KillTimer(window, kCrtTailTimer);
            crtTailPending_ = false;
            if (!realtimeConsumerActive_) {
                crtXySurface_.setActive(false);
            }
            return 0;
        }
        break;
    case kAnalysisDwellWakeMessage:
        handleAnalysisDwellTimer();
        return 0;
    case kCrtFrameReadyMessage:
        crtXySurface_.present(
            static_cast<std::uint32_t>(wParam),
            static_cast<std::uint32_t>(lParam));
        return 0;
    case kRefreshMessage:
        {
        const auto dirty = UiInvalidationBus::instance().consume(
            UiInvalidationSurface::listen,
            window);
        if (!any(dirty) ||
            any(dirty & (UiDirty::semantic | UiDirty::artwork))) {
            refreshTrack();
        }
        syncPlaybackLifecycle();
        if (any(dirty & UiDirty::analysis)) scheduleAnalysisDwell();
        refreshPresentationModel();
        syncRealtimeDemand();
        syncFrameClock();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
        }
    case kArtworkReadyMessage:
        refreshArtwork();
        InvalidateRect(window, nullptr, FALSE);
        syncFrameClock();
        return 0;
    case kArtworkInvalidatedMessage:
        if (hasTrackInfo_) {
            artwork_.reset();
            themedArtwork_ = nullptr;
            ThemeService::instance().resetForTrack(artworkIdentity_);
            refreshArtwork();
            syncFrameClock();
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case kThemeTransitionMessage:
        syncFrameClock();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY: {
        monitorIdentity_.reset();
        ScrubCoordinator::instance().cancel(scrubSession_);
        scrubSession_ = 0;
        analysisDwellWindow_.store(nullptr, std::memory_order_release);
        if (analysisDwellWake_ != nullptr) {
            SetThreadpoolTimer(analysisDwellWake_, nullptr, 0, 0);
        }
        DisplayEnvironmentService::instance().unregisterPersistentRoot(window);
        DragAcceptFiles(window, FALSE);
        UiInvalidationBus::instance().unregisterWindow(
            UiInvalidationSurface::listen,
            window);
        updateVisibilityLifecycle(false);
        KillTimer(window, kAnalysisDwellTimer);
        KillTimer(window, kMusicMapCursorTimer);
        KillTimer(window, kCrtTailTimer);
        ThemeService::instance().unsubscribe(window);
        GlyphFlowService::instance().unsubscribe(window);
        frameClock_.detach();
        if (GetCapture() == window) ReleaseCapture();
        RECT bounds{};
        if (!IsIconic(window) && GetWindowRect(window, &bounds)) {
            g_windowX = bounds.left;
            g_windowY = bounds.top;
            g_windowW = bounds.right - bounds.left;
            g_windowH = bounds.bottom - bounds.top;
            g_windowDpi = (std::max)(1U, GetDpiForWindow(window));
        }
        crtXySurface_.shutdown();
        AnalysisController::instance().cancelViewRequest();
        // Packet cost belongs to the playing track, not this window. Its
        // result/worker also serves WaveSeek and Transport after Listen closes;
        // Runtime track changes and component shutdown own its cancellation.
        artCache_.detach();
        artwork_.reset();
        renderer_.discard();
        track_.release();
        hasTrackInfo_ = false;
        resetRealtimePresentation();
        window_ = nullptr;
        UiInvalidationBus::instance().publish(
            UiInvalidationSurface::nativePanels, UiDirty::semantic);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ListenWindow::paint()
{
    PAINTSTRUCT paintState{};
    BeginPaint(window_, &paintState);
    if (!presentationRevision_.pending()) presentationRevision_.invalidate();
    if (preparationRetryQpc_ != 0 && qpcNow() < preparationRetryQpc_) {
        // Input, resize and expose paints must not bypass resource backoff.
        // Keep the latest revision pending and retain the original deadline.
        EndPaint(window_, &paintState);
        syncFrameClock();
        return;
    }
    ++preparationAttempts_;
    if (!renderer_.prepareSurface(window_)) {
        ++preparationFailures_;
        // Preparation can retire the surface before a presentation opportunity
        // exists. Drop the old binding and explicitly schedule another attempt,
        // including when a paused Overview had already stopped its clock.
        frameClock_.setReadinessBinding(renderer_.bindingKey());
        frameClock_.stop();
        preparationRetryQpc_ = qpcNow() + (std::max)(
            std::uint64_t{1}, static_cast<std::uint64_t>(qpcFrequency_ / 4.0));
        EndPaint(window_, &paintState);
        syncFrameClock();
        return;
    }
    preparationRetryQpc_ = 0;
    frameClock_.setReadinessBinding(renderer_.bindingKey());
    if (!frameClock_.beginPresentationOpportunity()) {
        EndPaint(window_, &paintState);
        return;
    }
    const auto frame = presentationRevision_.capture();
    auto& telemetry = PerformanceTelemetry::instance();
    const auto renderSpan = telemetry.beginSpan(
        TelemetrySurface::listen, 26U);
    const auto result = renderer_.draw(window_, renderState());
    telemetry.endSpan(TelemetrySurface::listen, 26U, renderSpan);
    const auto resultCode = [&]() -> HRESULT {
        switch (result) {
        case FrameResult::accepted: return S_OK;
        case FrameResult::notReady: return DXGI_ERROR_WAS_STILL_DRAWING;
        case FrameResult::occluded: return DXGI_STATUS_OCCLUDED;
        case FrameResult::deviceLost: return DXGI_ERROR_DEVICE_RESET;
        case FrameResult::failed: return E_FAIL;
        }
        return E_FAIL;
    }();
    const auto visible = result == FrameResult::accepted &&
        DisplayEnvironmentService::instance().presentationVisible(window_);
    presentationRevision_.presented(frame, resultCode, false);
    if (result == FrameResult::deviceLost) {
        presentationRevision_.deviceEpochChanged();
    }
    // A device-loss path may have retired the swap-chain HANDLE. Rebind only
    // after the attempt, while the clock still owns its safe duplicate.
    frameClock_.setReadinessBinding(renderer_.bindingKey());
    EndPaint(window_, &paintState);
    // CRT demand/lifetime is independent from a transient refusal by the
    // parent scene. Preserve its swap chain and phosphor across notReady.
    const auto crtSyncSpan = telemetry.beginSpan(
        TelemetrySurface::listen, 27U);
    syncCrtSurface();
    telemetry.endSpan(TelemetrySurface::listen, 27U, crtSyncSpan);
    // ArtworkTransitionRenderer starts its semantic clock after accepting the
    // first decoded frame. The ready-message path necessarily runs before
    // that paint and therefore cannot see the transition yet. Re-evaluate
    // after presentation so static Listen views request the intermediate
    // GlyphFlow frames instead of freezing on frame one until another input.
    const auto playback = Runtime::instance().playbackSnapshot();
    const auto playbackDemand = realtimeConsumerActive_ ||
        scopePresentation_.active() ||
        ScrubCoordinator::instance().active() ||
        (Runtime::instance().dawPreviewActive() && activeSpace_ == 1) ||
        lifecycle_.analysisPending();
    const auto needsAnother = presentationRevision_.pending() ||
        playbackDemand || ThemeService::instance().transitionActive() ||
        renderer_.glyphTransitionActive() ||
        (activeSpace_ == 0 && renderer_.artworkTransitionActive());
    frameClock_.completePresentationOpportunity(needsAnother);
}

ListenRenderState ListenWindow::renderState() const
{
    const auto playback = Runtime::instance().playbackSnapshot();
    ListenRenderState state;
    state.activeSpace = activeSpace_;
    state.crtDirectOrientation = crtDirectOrientation_;
    state.liveFocus = liveFocus_;
    state.playback = playback;
    state.playback.positionSeconds =
        ScrubCoordinator::instance().effectivePosition(
            Runtime::instance().presentationPlaybackPosition());
    state.track = hasTrackInfo_ ? &trackInfo_ : nullptr;
    state.artworkIdentity = artworkIdentity_;
    state.realtime = hasVisualSnapshot_ ? &displaySnapshot_ : nullptr;
    if (scopePresentation_.ready()) {
        state.scopeSamples = &scopePresentation_.samples();
        state.scopeMinimum = &scopePresentation_.minimumSamples();
        state.scopeMaximum = &scopePresentation_.maximumSamples();
        state.scopeVisualRevision = scopePresentationRevision_;
    }
    state.model = analysisSnapshot_.get();
    state.dynamicsHold = dynamicsHold_;
    state.presentation = presentationModel_.get();
    state.artwork = artwork_;
    state.theme = ThemeService::instance().snapshot();
    state.analysis = AnalysisController::instance().snapshot();
    const auto currentPath = nativeTrackPath();
    if (state.analysis.trackPath != currentPath) {
        // A cache completion or terminal worker frame can race the foobar
        // track callback by one presentation. Never let a portrait, waveform
        // or metric from that older generation enter the new track's frame.
        state.analysis = {};
        state.analysis.trackPath = currentPath;
    }
    const auto userState = DatabaseService::instance().userStateSnapshot();
    state.favoriteAvailable = userState->available;
    state.favorite = userState->favorite;
    state.maximized = IsZoomed(window_) != FALSE;
    state.pointerVisible = pointerVisible_;
    state.pointerX = pointerX_;
    state.pointerY = pointerY_;
    return state;
}

void ListenWindow::tick()
{
    if (!lifecycle_.visible()) {
        frameClock_.stop();
        return;
    }
    if (lifecycle_.analysisPending() &&
            lifecycle_.remainingMilliseconds(
                qpcNow(),
                static_cast<std::uint64_t>(qpcFrequency_)) <= 1U) {
        handleAnalysisDwellTimer();
        syncFrameClock();
    }
    if (realtimeConsumerActive_) {
        Runtime::instance().pumpVisualisation();
    }
    if (ScrubCoordinator::instance().active()) {
        ScrubCoordinator::instance().pump();
    }
    bool changed = false;
    const auto crtLive = activeSpace_ == 1 &&
        (liveFocus_ == ListenLiveFocus::none ||
         liveFocus_ == ListenLiveFocus::crt) &&
        window_ != nullptr && lifecycle_.visible() &&
        DisplayEnvironmentService::instance().presentationVisible(window_);
    if (!crtLive && hasCrtStream_) {
        crtXyModel_.reset();
        hasCrtStream_ = false;
    }

    for (std::size_t drained = 0; crtLive && drained < 32; ++drained) {
        // Catch up aggressively while idle, but never make input wait behind
        // a full PCM backlog. Remaining frames stay available for the next
        // coordinated pulse and the multicast cursor still tracks skips.
        if (drained >= 2 &&
            InteractionLatencyGuard::instance().shouldYieldRender()) {
            break;
        }
        std::uint64_t skipped = 0;
        if (!Runtime::instance().consumeNextFrame(
                RealtimeConsumer::listen,
                frame_,
                frameSequence_,
                &skipped)) {
            break;
        }

        if (crtLive) {
            const auto ingestQpc = qpcNow();
            const auto frameAgeSeconds =
                frame_.qpcTicks != 0 && ingestQpc > frame_.qpcTicks
                ? std::clamp(
                    static_cast<double>(ingestQpc - frame_.qpcTicks) /
                        qpcFrequency_,
                    0.0,
                    10.0)
                : 0.0;
            const auto explicitBreak =
                (frame_.flags & kRealtimeFrameFlagDiscontinuity) != 0;
            const auto discontinuity =
                !hasCrtStream_ ||
                skipped != 0 ||
                explicitBreak ||
                frame_.streamGeneration != crtStreamGeneration_ ||
                frame_.sampleRate != crtSampleRate_ ||
                frame_.firstFrameIndex != crtExpectedFrameIndex_;
            const auto segments =
                crtXyModel_.ingest(
                    frame_, discontinuity, frameAgeSeconds);
            crtXySurface_.submit(segments);
            hasCrtStream_ = frame_.channels >= 2;
            crtStreamGeneration_ = frame_.streamGeneration;
            crtSampleRate_ = frame_.sampleRate;
            crtExpectedFrameIndex_ =
                frame_.firstFrameIndex + frame_.frameCount;
        }

    }

    const auto analysis = realtimeConsumerActive_
        ? RealtimeAnalysisHub::instance().snapshot()
        : std::shared_ptr<const RealtimeAnalysisSnapshot>{};
    if (analysis && analysis->generation != analysisGeneration_) {
        if (hasVisualSnapshot_ && !analysis->discontinuity) {
            previousSnapshot_ = currentSnapshot_;
            hasPreviousSnapshot_ = true;
        } else {
            hasPreviousSnapshot_ = false;
        }
        currentSnapshot_ = analysis->visual;
        if (!hasPreviousSnapshot_) previousSnapshot_ = currentSnapshot_;
        if (previousAudioQpc_ != 0 &&
            analysis->sourceQpc > previousAudioQpc_ &&
            !analysis->discontinuity) {
            const auto observed = static_cast<double>(
                analysis->sourceQpc - previousAudioQpc_);
            const auto minimum = qpcFrequency_ / 240.0;
            const auto maximum = qpcFrequency_ / 30.0;
            framePeriodTicks_ = framePeriodTicks_ * 0.82 +
                std::clamp(observed, minimum, maximum) * 0.18;
        }
        previousAudioQpc_ = analysis->sourceQpc;
        snapshotArrivalQpc_ = qpcNow();
        analysisGeneration_ = analysis->generation;
        analysisSnapshot_ = analysis;
        hasVisualSnapshot_ = true;
        if (scopePresentation_.syncTarget(analysis->scope)) {
            ++scopePresentationRevision_;
            changed = true;
        }
        changed = true;
    }
    if (hasVisualSnapshot_) {
        const auto elapsed = presentationElapsedSeconds(
            scopePresentationQpc_, frameClock_.intervalSeconds());
        if (scopePresentation_.advance(elapsed)) {
            ++scopePresentationRevision_;
            changed = true;
        }
        updateInterpolatedSnapshot();
        if (scopePresentation_.ready()) {
            displaySnapshot_.scope = scopePresentation_.samples();
        }
    }
    auto playback = Runtime::instance().playbackSnapshot();
    playback.positionSeconds =
        ScrubCoordinator::instance().effectivePosition(
            playback.positionSeconds);
    if (activeSpace_ == 0 &&
        playback.state == PlaybackState::playing &&
        playback.durationSeconds > 0.0) {
        RECT client{};
        GetClientRect(window_, &client);
        const auto width = std::max(1L, client.right - client.left);
        const auto step = static_cast<std::int64_t>(std::llround(
            std::clamp(
                playback.positionSeconds / playback.durationSeconds,
                0.0,
                1.0) * static_cast<double>(width) * 4.0));
        if (step != lastPlaybackCursorStep_) {
            lastPlaybackCursorStep_ = step;
            changed = true;
        }
    }
    // Only Overview draws the cover. A transition left pending when entering
    // Live must not keep paused Live repainting; Overview will finish it on
    // return, when its pixels can actually be presented.
    const auto semanticMotion =
        ThemeService::instance().transitionActive() ||
        renderer_.glyphTransitionActive() ||
        (activeSpace_ == 0 && renderer_.artworkTransitionActive());
    if (!IsIconic(window_) && (changed || semanticMotion)) {
        InvalidateRect(window_, nullptr, FALSE);
    }
    // Re-evaluate after every presentation pulse. When the final semantic
    // frame has painted, the following pulse stops the clock instead of
    // leaving an otherwise idle Listen window ticking indefinitely.
    syncFrameClock();
}

void ListenWindow::refreshTrack(bool force)
{
    const auto next = Runtime::instance().currentTrackHandle();
    if (!force && next == track_ && !trackTransitionPending_) return;
    trackTransitionPending_ = false;
    track_ = next;
    artworkIdentity_ = artworkTrackIdentity(track_);
    dynamicsHold_ = false;
    artwork_.reset();
    themedArtwork_ = nullptr;
    hasTrackInfo_ = false;
    trackInfo_ = {};

    if (track_.is_valid()) {
        // Keep the index and records from the same immutable publication.
        // The worker already indexes path + subsong; a track switch need not
        // scan the entire Library on the window thread.
        const auto snapshot = LibraryModel::instance().sharedSnapshot();
        const LibraryTrack* cached = nullptr;
        if (snapshot && snapshot->tracks) {
            std::string key(track_->get_path());
            key.push_back('\x1f');
            key += std::to_string(track_->get_subsong_index());
            const auto found = snapshot->handleIndex.find(key);
            if (found != snapshot->handleIndex.end() &&
                    found->second < snapshot->tracks->size()) {
                const auto& candidate = (*snapshot->tracks)[found->second];
                if (candidate.handle == track_) cached = &candidate;
            }
        }
        trackInfo_ = cached ? *cached : makeFallbackTrack(track_);
        hasTrackInfo_ = true;
        const auto title = L"GODENplayer Listen  \u00B7  " +
            trackInfo_.artist + L" - " + trackInfo_.title;
        SetWindowTextW(window_, title.c_str());
    } else {
        SetWindowTextW(window_, L"GODENplayer Listen");
    }

    const auto path = nativeTrackPath();
    AnalysisController::instance().onTrackChanged(path);
    lifecycle_.onTrackChanged(
        !path.empty(),
        qpcNow(),
        static_cast<std::uint64_t>(qpcFrequency_));
    scheduleAnalysisDwell();
    refreshPresentationModel();

    ThemeService::instance().resetForTrack(
        artworkIdentity_);
    if (realtimeConsumerActive_) {
        RealtimeAnalysisHub::instance().reset(
            hasTrackInfo_
                ? trackInfo_.durationSeconds
                : Runtime::instance().playbackSnapshot().durationSeconds,
            Runtime::instance().latestRealtimeSequence());
    }
    scopePresentation_.reset();
    ++scopePresentationRevision_;
    scopePresentationQpc_.QuadPart = 0;
    previousSnapshot_ = {};
    currentSnapshot_ = {};
    displaySnapshot_ = {};
    hasVisualSnapshot_ = false;
    hasPreviousSnapshot_ = false;
    analysisSnapshot_.reset();
    analysisGeneration_ = 0;
    previousAudioQpc_ = 0;
    refreshArtwork();
}

void ListenWindow::refreshPresentationModel()
{
    auto analysis = AnalysisController::instance().snapshot();
    const auto currentPath = nativeTrackPath();
    if (analysis.trackPath != currentPath) {
        analysis = {};
        analysis.trackPath = currentPath;
    }
    const auto* identity = analysis.resultJson.get();
    const auto libraryGeneration = LibraryModel::instance().generation();
    const auto progressPercent = static_cast<int>(std::lround(
        std::clamp(analysis.overallProgress, 0.0f, 1.0f) * 100.0f));
    if (presentationModel_ &&
        presentationAnalysisIdentity_ == identity &&
        presentationLibraryGeneration_ == libraryGeneration &&
        presentationRequestId_ == analysis.requestId &&
        presentationStatus_ == analysis.status &&
        presentationStage_ == analysis.stage &&
        presentationProgressPercent_ == progressPercent &&
        presentationModel_->trackPath == currentPath) {
        return;
    }

    const auto& library = LibraryModel::instance().tracks();
    presentationModel_ = buildListenPresentationModelV3(
        hasTrackInfo_ ? &trackInfo_ : nullptr,
        analysis,
        library,
        LibraryModel::instance().generation());
    presentationAnalysisIdentity_ = identity;
    presentationLibraryGeneration_ = LibraryModel::instance().generation();
    presentationRequestId_ = analysis.requestId;
    presentationStatus_ = analysis.status;
    presentationStage_ = analysis.stage;
    presentationProgressPercent_ = progressPercent;
}

void ListenWindow::refreshArtwork()
{
    if (!track_.is_valid() || !hasTrackInfo_) {
        // Foobar can briefly report no current track while switching items.
        // Keep the renderer's presented bitmap alive so the next decoded image
        // has a real source frame to transition from.
        return;
    }
    artwork_ = artCache_.getOrRequest(
        artworkIdentity_, track_, 720, ArtRequestPriority::visibleMain);
    if (!artwork_ && artCache_.status(
            artworkIdentity_, 720) == ArtCacheStatus::missing) {
        renderer_.clearArtwork();
        if (ThemeService::instance().resolveWithoutArtwork(
                artworkIdentity_)) {
            Runtime::instance().refreshPresentationState();
            syncFrameClock();
        }
    }
    if (artwork_ && artwork_.get() != themedArtwork_) {
        themedArtwork_ = artwork_.get();
        if (ThemeService::instance().updateFromArtwork(
                artworkIdentity_, *artwork_)) {
            Runtime::instance().refreshPresentationState();
            syncFrameClock();
        }
    }
}

void ListenWindow::refreshMonitorIdentity(HWND window) noexcept
{
    if (!monitorIdentity_.update(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
            GetDpiForWindow(window))) return;
    renderer_.displayEnvironmentChanged();
    presentationRevision_.invalidate();
    frameClock_.requestImmediate();
    InvalidateRect(window, nullptr, FALSE);
}

void ListenWindow::updateVisibilityLifecycle(bool visible)
{
    visible = visible &&
        window_ != nullptr &&
        DisplayEnvironmentService::instance().presentationVisible(window_);
    const auto changed = lifecycle_.setVisible(
        visible,
        qpcNow(),
        static_cast<std::uint64_t>(qpcFrequency_));

    if (!visible) {
        preparationRetryQpc_ = 0;
        ScrubCoordinator::instance().cancel(scrubSession_);
        scrubSession_ = 0;
        seeking_ = false;
        scrubDragging_ = false;
        if (window_ != nullptr && GetCapture() == window_) ReleaseCapture();
        cancelAnalysisDwell(true);
        KillTimer(window_, kMusicMapCursorTimer);
        KillTimer(window_, kCrtTailTimer);
        crtTailPending_ = false;
        frameClock_.stop();
        crtXySurface_.setActive(false);
        crtXySurface_.reset();
        if (realtimeConsumerActive_) {
            RealtimeAnalysisHub::instance().deactivate();
            Runtime::instance().setRealtimeConsumerActive(
                RealtimeConsumer::listen, false);
            realtimeConsumerActive_ = false;
        }
        if (changed) resetRealtimePresentation();
        return;
    }

    syncPlaybackLifecycle();
    syncRealtimeDemand();
    if (changed) scheduleAnalysisDwell();
    syncFrameClock();
    syncCrtSurface();
}

void ListenWindow::syncPlaybackLifecycle()
{
    if (window_ == nullptr) return;
    const auto playbackState = Runtime::instance().playbackSnapshot().state;
    const auto playing = playbackState == PlaybackState::playing;
    const auto changed = lifecycle_.setPlaying(
        playing,
        qpcNow(),
        static_cast<std::uint64_t>(qpcFrequency_));
    if (!playing) {
        cancelAnalysisDwell(true);
    } else if (changed) {
        scheduleAnalysisDwell();
    }
    if (playbackState != presentedPlaybackState_) {
        if (playbackState == PlaybackState::paused ||
                playbackState == PlaybackState::stopped) {
            if (scopePresentation_.beginRelease())
                ++scopePresentationRevision_;
        }
        presentedPlaybackState_ = playbackState;
    }
    scheduleMusicMapCursor();
    syncFrameClock();
}

void ListenWindow::syncRealtimeDemand()
{
    const auto playback = Runtime::instance().playbackSnapshot();
    const auto audioActive =
        playback.state == PlaybackState::playing ||
        ScrubCoordinator::instance().active() ||
        Runtime::instance().dawPreviewActive();
    const auto demandV2 = rolloutFlagEnabled(RolloutFlag::listenDemandV2);
    const auto shouldRun = lifecycle_.visible() && activeSpace_ == 1 &&
        (demandV2 ? audioActive : true);
    std::uint32_t featureMask = 0;
    if (shouldRun) {
        if (liveFocus_ == ListenLiveFocus::none) {
            featureMask = kAnalysisFeatureCompactScope |
                kAnalysisFeatureSpectralHistory |
                (dynamicsHold_ ? 0U : kAnalysisFeatureDynamics);
        } else if (liveFocus_ == ListenLiveFocus::scope) {
            featureMask = kAnalysisFeatureCompactScope;
        } else if (liveFocus_ == ListenLiveFocus::dynamics &&
                !dynamicsHold_) {
            featureMask = kAnalysisFeatureDynamics;
        } else if (liveFocus_ == ListenLiveFocus::spectrogram) {
            featureMask = kAnalysisFeatureSpectralHistory;
        }
    }
    const auto shouldConsume = shouldRun &&
        (featureMask != 0 || liveFocus_ == ListenLiveFocus::none ||
         liveFocus_ == ListenLiveFocus::crt);
    if (shouldConsume && !realtimeConsumerActive_) {
        if (window_ != nullptr) KillTimer(window_, kCrtTailTimer);
        crtTailPending_ = false;
        resetRealtimePresentation();
        (void)RealtimeAnalysisHub::instance().setDemand(
            AnalysisOwner::listen, 1U, featureMask,
            hasTrackInfo_
                ? trackInfo_.durationSeconds
                : Runtime::instance().playbackSnapshot().durationSeconds,
            Runtime::instance().latestRealtimeSequence());
        Runtime::instance().setRealtimeConsumerActive(
            RealtimeConsumer::listen, true);
        realtimeConsumerActive_ = true;
    } else if (shouldConsume && realtimeConsumerActive_) {
        (void)RealtimeAnalysisHub::instance().setDemand(
            AnalysisOwner::listen, 1U, featureMask,
            hasTrackInfo_ ? trackInfo_.durationSeconds
                : playback.durationSeconds,
            Runtime::instance().latestRealtimeSequence());
    } else if (!shouldConsume && realtimeConsumerActive_) {
        RealtimeAnalysisHub::instance().deactivate();
        Runtime::instance().setRealtimeConsumerActive(
            RealtimeConsumer::listen, false);
        realtimeConsumerActive_ = false;
        if (lifecycle_.visible() && activeSpace_ == 1 && window_ != nullptr) {
            crtTailPending_ = true;
            SetTimer(window_, kCrtTailTimer, kCrtTailMilliseconds, nullptr);
        } else {
            crtTailPending_ = false;
            resetRealtimePresentation();
        }
    }
}

void ListenWindow::resetRealtimePresentation()
{
    frameSequence_ = Runtime::instance().discardRealtimeFrames(
        RealtimeConsumer::listen);
    frame_ = {};
    if (realtimeConsumerActive_) {
        RealtimeAnalysisHub::instance().reset(
            hasTrackInfo_
                ? trackInfo_.durationSeconds
                : Runtime::instance().playbackSnapshot().durationSeconds,
            frameSequence_);
    }
    scopePresentation_.reset();
    ++scopePresentationRevision_;
    renderer_.clearLiveHistory();
    scopePresentationQpc_.QuadPart = 0;
    previousSnapshot_ = {};
    currentSnapshot_ = {};
    displaySnapshot_ = {};
    hasVisualSnapshot_ = false;
    hasPreviousSnapshot_ = false;
    analysisSnapshot_.reset();
    analysisGeneration_ = 0;
    snapshotArrivalQpc_ = 0;
    previousAudioQpc_ = 0;
    crtXyModel_.reset();
    crtXySurface_.reset();
    crtStreamGeneration_ = 0;
    crtExpectedFrameIndex_ = 0;
    crtSampleRate_ = 0;
    hasCrtStream_ = false;
}

void ListenWindow::scheduleAnalysisDwell()
{
    if (window_ == nullptr) return;
    KillTimer(window_, kAnalysisDwellTimer);
    if (!lifecycle_.visible() || !lifecycle_.playing() ||
        !lifecycle_.analysisPending()) return;

    const auto path = nativeTrackPath();
    if (path.empty()) {
        lifecycle_.cancelPendingAnalysis();
        return;
    }
    const auto analysis = AnalysisController::instance().snapshot();
    if (analysis.trackPath == path &&
        (analysis.status == DeepAnalysisStatus::running ||
            analysis.status == DeepAnalysisStatus::complete)) {
        lifecycle_.cancelPendingAnalysis();
        return;
    }

    const auto milliseconds = lifecycle_.remainingMilliseconds(
        qpcNow(),
        static_cast<std::uint64_t>(qpcFrequency_));
    if (milliseconds == 0) return;
    if (SetTimer(window_, kAnalysisDwellTimer, milliseconds, nullptr) == 0) {
        console::print("GODENplayer Listen could not arm deep-analysis dwell.");
        lifecycle_.cancelPendingAnalysis();
        return;
    }
    if (analysisDwellWake_ != nullptr) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(
            std::max<std::uint32_t>(1U, milliseconds)) * 10000LL;
        SetThreadpoolTimer(analysisDwellWake_,
            reinterpret_cast<FILETIME*>(&due), 0, 0);
    }
}

void ListenWindow::cancelAnalysisDwell(bool cancelRunning)
{
    lifecycle_.cancelPendingAnalysis();
    if (window_ != nullptr) KillTimer(window_, kAnalysisDwellTimer);
    if (analysisDwellWake_ != nullptr) {
        SetThreadpoolTimer(analysisDwellWake_, nullptr, 0, 0);
    }
    if (cancelRunning) AnalysisController::instance().cancel();
}

void ListenWindow::handleAnalysisDwellTimer()
{
    if (window_ == nullptr) return;
    KillTimer(window_, kAnalysisDwellTimer);
    if (analysisDwellWake_ != nullptr) {
        SetThreadpoolTimer(analysisDwellWake_, nullptr, 0, 0);
    }
    const auto now = qpcNow();
    if (!lifecycle_.consumeAnalysisDue(now)) {
        scheduleAnalysisDwell();
        return;
    }

    const auto path = nativeTrackPath();
    if (path.empty() || !lifecycle_.visible() || !lifecycle_.playing()) return;
    auto analysis = AnalysisController::instance().snapshot();
    if (analysis.trackPath != path) {
        AnalysisController::instance().onTrackChanged(path);
        analysis = AnalysisController::instance().snapshot();
    }
    if (analysis.status == DeepAnalysisStatus::running ||
        analysis.status == DeepAnalysisStatus::complete) {
        return;
    }
    AnalysisController::instance().start(path);
}

void ListenWindow::syncFrameClock()
{
    if (window_ == nullptr) return;
    if (scrubSession_ != 0 && !ScrubCoordinator::instance().owns(scrubSession_)) {
        scrubSession_ = 0;
        seeking_ = false;
        scrubDragging_ = false;
        if (GetCapture() == window_) ReleaseCapture();
    }
    if (!lifecycle_.visible() && scrubSession_ != 0) {
        ScrubCoordinator::instance().cancel(scrubSession_);
        scrubSession_ = 0;
        seeking_ = false;
        scrubDragging_ = false;
        if (GetCapture() == window_) ReleaseCapture();
    }
    const auto playback = Runtime::instance().playbackSnapshot();
    // Overview is fully revision-driven. Music Map owns only a raster-gated
    // playback cursor: the full-track profile is retained, so display pulses
    // redraw solely when that cursor crosses a quarter physical pixel.
    const auto playbackDemand = realtimeConsumerActive_ ||
        scopePresentation_.active() ||
        ScrubCoordinator::instance().active() ||
        (Runtime::instance().dawPreviewActive() && activeSpace_ == 1);
    scheduleMusicMapCursor();
    if ((presentationRevision_.pending() || playbackDemand ||
            ThemeService::instance().transitionActive() ||
            renderer_.glyphTransitionActive() ||
            (activeSpace_ == 0 && renderer_.artworkTransitionActive())) &&
        lifecycle_.visible() &&
        DisplayEnvironmentService::instance().presentationVisible(window_)) {
        if (preparationRetryQpc_ != 0 && qpcNow() < preparationRetryQpc_) {
            // An unrelated immediate request may have cleared the logical
            // deadline or queued an early pulse. Retire it once, not on every
            // state notification, so repeated updates cannot postpone recovery.
            if (frameClock_.logicalNotBeforeQpc() != preparationRetryQpc_)
                frameClock_.stop();
            frameClock_.requestAtOrAfter(
                preparationRetryQpc_, kFrameReasonAmbient);
        } else {
            frameClock_.start();
        }
    } else {
        preparationRetryQpc_ = 0;
        frameClock_.stop();
    }
}

void ListenWindow::syncCrtSurface()
{
    const auto shouldBeActive =
        window_ != nullptr &&
        lifecycle_.visible() &&
        activeSpace_ == 1 &&
        (liveFocus_ == ListenLiveFocus::none ||
         liveFocus_ == ListenLiveFocus::crt) &&
        (realtimeConsumerActive_ || crtTailPending_) &&
        DisplayEnvironmentService::instance().presentationVisible(window_);
    if (!shouldBeActive) {
        crtXySurface_.setActive(false);
        return;
    }

    const auto layout = currentLiveLayout();
    const auto rectangle = squareWithinListenRect(layout.crt.plot);
    if (rectangle.right <= rectangle.left ||
        rectangle.bottom <= rectangle.top) {
        crtXySurface_.setActive(false);
        return;
    }
    const auto dpi = std::max<UINT>(1, GetDpiForWindow(window_));
    const auto scale = static_cast<float>(dpi) / 96.0f;
    const RECT pixelRect{
        static_cast<LONG>(std::floor(rectangle.left * scale)),
        static_cast<LONG>(std::floor(rectangle.top * scale)),
        static_cast<LONG>(std::ceil(rectangle.right * scale)),
        static_cast<LONG>(std::ceil(rectangle.bottom * scale)),
    };
    crtXySurface_.setViewport(pixelRect);
    crtXySurface_.setActive(true);
}

ListenLiveLayout ListenWindow::currentLiveLayout() const noexcept
{
    if (window_ == nullptr) return {};
    RECT client{};
    GetClientRect(window_, &client);
    const auto dpi = static_cast<float>(
        std::max<UINT>(1, GetDpiForWindow(window_)));
    const auto widthDip = static_cast<float>(client.right) * 96.0f / dpi;
    const auto heightDip = static_cast<float>(client.bottom) * 96.0f / dpi;
    const auto content = D2D1::RectF(
        14.0f, 113.0f,
        std::max(14.0f, widthDip - 14.0f),
        std::max(113.0f, heightDip - 14.0f));
    return computeListenLayout(content, liveFocus_, dpi);
}

void ListenWindow::setLiveFocus(ListenLiveFocus focus)
{
    if (liveFocus_ == focus) return;
    liveFocus_ = focus;
    syncRealtimeDemand();
    // Geometry is derived immediately from the current client/DPI and is
    // shared by CRT and the next render; it never reads a previous paint.
    syncCrtSurface();
    presentationRevision_.invalidate();
    InvalidateRect(window_, nullptr, FALSE);
}

void ListenWindow::setDynamicsHold(bool hold)
{
    if (dynamicsHold_ == hold) return;
    dynamicsHold_ = hold;
    syncRealtimeDemand();
    presentationRevision_.invalidate();
    syncFrameClock();
    if (window_ != nullptr) InvalidateRect(window_, nullptr, FALSE);
}

void ListenWindow::scheduleMusicMapCursor()
{
    if (window_ == nullptr) return;
    KillTimer(window_, kMusicMapCursorTimer);
}

void ListenWindow::setCrtDirectOrientation(bool direct)
{
    if (crtDirectOrientation_ == direct) return;
    crtDirectOrientation_ = direct;
    g_liveStereoMode = direct ? 1 : 0;
    crtXySurface_.setOrientation(
        direct
            ? CrtXyOrientation::directLeftRight
            : CrtXyOrientation::midSide);
    crtXyModel_.reset();
    hasCrtStream_ = false;
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
        syncCrtSurface();
        syncFrameClock();
    }
}

void ListenWindow::updateInterpolatedSnapshot()
{
    displaySnapshot_ = currentSnapshot_;
    if (!hasPreviousSnapshot_) return;
    const auto elapsed = static_cast<double>(
        qpcNow() - snapshotArrivalQpc_);
    const auto amount = static_cast<float>(std::clamp(
        elapsed / std::max(1.0, framePeriodTicks_), 0.0, 1.0));
    displaySnapshot_.rms = interpolate(
        previousSnapshot_.rms, currentSnapshot_.rms, amount);
    displaySnapshot_.peak = interpolate(
        previousSnapshot_.peak, currentSnapshot_.peak, amount);
    displaySnapshot_.correlation = interpolate(
        previousSnapshot_.correlation, currentSnapshot_.correlation, amount);
    displaySnapshot_.width = interpolate(
        previousSnapshot_.width, currentSnapshot_.width, amount);
    displaySnapshot_.brightness = interpolate(
        previousSnapshot_.brightness, currentSnapshot_.brightness, amount);
    displaySnapshot_.pulse = interpolate(
        previousSnapshot_.pulse, currentSnapshot_.pulse, amount);
    displaySnapshot_.spectralCentroidHz = interpolate(
        previousSnapshot_.spectralCentroidHz,
        currentSnapshot_.spectralCentroidHz,
        amount);
    displaySnapshot_.spectralRolloff85Hz = interpolate(
        previousSnapshot_.spectralRolloff85Hz,
        currentSnapshot_.spectralRolloff85Hz,
        amount);
    displaySnapshot_.spectralConfidence = interpolate(
        previousSnapshot_.spectralConfidence,
        currentSnapshot_.spectralConfidence,
        amount);
    // CompactScopePresenter owns the phase-coherent scope interpolation and
    // writes its result after this function. Interpolating all 512 samples a
    // second time here was both invisible and paid at display cadence.
    displaySnapshot_.vectorscope = currentSnapshot_.vectorscope;
    for (std::size_t index = 0; index < kSpectrumBins; ++index) {
        displaySnapshot_.spectrum[index] = interpolate(
            previousSnapshot_.spectrum[index],
            currentSnapshot_.spectrum[index], amount);
    }
    for (std::size_t index = 0; index < kSpectrumFftBins; ++index) {
        displaySnapshot_.spectrumFft[index] = interpolate(
            previousSnapshot_.spectrumFft[index],
            currentSnapshot_.spectrumFft[index], amount);
    }
}

double ListenWindow::normalizedSeekPoint(float x) const noexcept
{
    const auto map = renderer_.transportSeekRect();
    if (map.right <= map.left) return 0.0;
    return std::clamp(
        (x - map.left) / (map.right - map.left), 0.0f, 1.0f);
}

void ListenWindow::startOrCancelAnalysis()
{
    const auto state = AnalysisController::instance().snapshot();
    if (state.status == DeepAnalysisStatus::running) {
        cancelAnalysisDwell(true);
        return;
    }
    const auto path = nativeTrackPath();
    if (path.empty() || !lifecycle_.visible() || !lifecycle_.playing()) return;
    // An explicit RETRY/ANALYZE is immediate. The dwell exists solely to
    // avoid speculative automatic work while the listener browses tracks.
    lifecycle_.cancelPendingAnalysis();
    if (window_ != nullptr) KillTimer(window_, kAnalysisDwellTimer);
    AnalysisController::instance().start(path);
    syncFrameClock();
}

bool ListenWindow::dispatchCommand(const ListenCommandV1& command)
{
    if (!validListenCommand(command)) return false;
    switch (command.kind) {
    case ListenCommandKind::seek:
        if (!std::isfinite(command.value) || command.value < 0.0) return false;
        playback_control::get()->playback_seek(command.value);
        return true;
    case ListenCommandKind::analyze: {
        const auto path = nativeTrackPath();
        if (path.empty() || !lifecycle_.visible() ||
            !lifecycle_.playing()) return false;
        lifecycle_.cancelPendingAnalysis();
        if (window_ != nullptr) KillTimer(window_, kAnalysisDwellTimer);
        return AnalysisController::instance().start(path);
    }
    case ListenCommandKind::cancelAnalysis:
        cancelAnalysisDwell(true);
        return true;
    case ListenCommandKind::toggleFavorite:
        return DatabaseService::instance().toggleCurrentFavorite();
    case ListenCommandKind::navigate:
        const auto requestedSpace = command.index == 0U ? 0 : 1;
        if (requestedSpace != 1) liveFocus_ = ListenLiveFocus::none;
        if (activeSpace_ != requestedSpace && requestedSpace == 1) {
            frameSequence_ = Runtime::instance().latestRealtimeSequence();
            crtXyModel_.reset();
            crtXySurface_.reset();
            hasCrtStream_ = false;
        }
        activeSpace_ = requestedSpace;
        lastPlaybackCursorStep_ =
            (std::numeric_limits<std::int64_t>::min)();
        g_lastSpace = activeSpace_;
        if (window_ != nullptr) {
            if (activeSpace_ != 1) crtXySurface_.setActive(false);
            syncRealtimeDemand();
            scheduleMusicMapCursor();
            syncFrameClock();
            InvalidateRect(window_, nullptr, FALSE);
        }
        return true;
    }
    return false;
}

std::filesystem::path ListenWindow::nativeTrackPath() const
{
    if (!track_.is_valid()) return {};
    try {
        const auto native = filesystem::g_get_native_path(track_->get_path());
        return utf8ToWide(native.c_str());
    } catch (...) {
        return {};
    }
}

D2D1_POINT_2F ListenWindow::pointInDip(LPARAM lParam) const noexcept
{
    const auto dpi = window_ != nullptr
        ? std::max<UINT>(1, GetDpiForWindow(window_))
        : 96U;
    const auto scale = 96.0f / static_cast<float>(dpi);
    return D2D1::Point2F(
        static_cast<float>(GET_X_LPARAM(lParam)) * scale,
        static_cast<float>(GET_Y_LPARAM(lParam)) * scale);
}

} // namespace nekorox::component
