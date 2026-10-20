#include "qa_hooks.h"
#include "wave_seek_panel.h"
#include "frame_clock.h"
#include "display_environment.h"
#include "glyph_flow_renderer.h"
#include "motion_coordinator.h"
#include "packet_cost_controller.h"
#include "panel_d2d.h"
#include "runtime.h"
#include "scrub_coordinator.h"
#include "theme_service.h"
#include "ui_invalidation_bus.h"
#include "ui_preferences.h"
#include "waveform_cache_service.h"
#include "waveform_persistence.h"
#include "telemetry_span.h"

#include "nekorox/qa_contracts.h"
#include "nekorox/playback_presentation_gate.h"

#include <columns_ui-sdk/ui_extension.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nekorox::component {
namespace {

constexpr UINT kWaveSeekRefreshMessage = WM_APP + 0x520;
constexpr UINT kWaveSeekSnapshotMessage = WM_APP + 0x521;
constexpr UINT kWaveSeekFrameMessage = WM_APP + 0x522;
constexpr UINT kThemeTransitionMessage = WM_APP + 0x525;
constexpr MotionKey kWaveSeekReconstructMotion = 0x523;
constexpr MotionKey kWaveSeekCoverageMotion = 0x524;
constexpr unsigned kWaveSeekReconstructTiles = 36;
constexpr float kWaveSeekLaneGapDip = 4.0f;
constexpr std::uint64_t kWaveSeekDetailRasterPhase = 0x101;

UINT waveSeekStateMessage() noexcept
{
    static const auto message =
        RegisterWindowMessageW(nekorox::kWaveSeekStateMessageV1);
    return message;
}

const GUID kWaveSeekPanelGuid = {
    0x369b09d4,
    0x58a7,
    0x4730,
    {0xa2, 0xce, 0xdd, 0xd8, 0x24, 0xe8, 0x32, 0x13},
};

double qpcSeconds() noexcept
{
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart > 0
        ? static_cast<double>(counter.QuadPart) /
            static_cast<double>(frequency.QuadPart)
        : 0.0;
}

std::uint64_t waveSeekQpcNow() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

std::uint64_t waveSeekRetryDeadline() noexcept
{
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return waveSeekQpcNow() + std::max(std::uint64_t{1},
        static_cast<std::uint64_t>(frequency.QuadPart) / 4U);
}

std::wstring hoverTime(double seconds)
{
    seconds = std::max(0.0, seconds);
    const auto total = static_cast<std::uint64_t>(std::llround(seconds));
    wchar_t value[32]{};
    swprintf_s(value, L"%llu:%02llu",
        static_cast<unsigned long long>(total / 60U),
        static_cast<unsigned long long>(total % 60U));
    return value;
}

bool pointIn(
    const D2D1_RECT_F& rectangle,
    float x,
    float y) noexcept
{
    return x >= rectangle.left && x < rectangle.right &&
        y >= rectangle.top && y < rectangle.bottom;
}

bool canPresent(HWND window) noexcept
{
    return DisplayEnvironmentService::instance().presentationVisible(window);
}

// A linearly advancing playback cursor does not need display-rate updates.
// Use the monitor divisor nearest 10 Hz so presentation remains evenly paced:
// a 165 Hz display, for example, advances once every 17 refreshes (9.706 Hz).
// Scrub and semantic animation bypass this cap in syncClock().
double playbackCursorRefreshHz(std::uint32_t refreshMilliHz) noexcept
{
    const auto refreshHz = std::max(
        1.0, static_cast<double>(refreshMilliHz) / 1000.0);
    const auto divisor = std::max(1.0, std::round(refreshHz / 10.0));
    return refreshHz / divisor;
}

std::uint32_t mixBits(std::uint64_t value) noexcept
{
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return static_cast<std::uint32_t>(value ^ (value >> 32U));
}

float smoothStep(float value) noexcept
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}

float waveformCoverage(const WaveformPyramidV1& pyramid) noexcept
{
    if (pyramid.complete()) return 1.0f;
    if (pyramid.totalFrames == 0) return 0.0f;
    return std::clamp(
        static_cast<float>(pyramid.scannedFrames) /
            static_cast<float>(pyramid.totalFrames),
        0.0f,
        1.0f);
}

D2D1_RECT_F intersectRect(
    const D2D1_RECT_F& first,
    const D2D1_RECT_F& second) noexcept
{
    return D2D1::RectF(
        std::max(first.left, second.left),
        std::max(first.top, second.top),
        std::min(first.right, second.right),
        std::min(first.bottom, second.bottom));
}

std::pair<float, float> pointDip(
    HWND window,
    LPARAM lParam) noexcept
{
    const auto scale = 96.0f /
        static_cast<float>(
            std::max<UINT>(1U, GetDpiForWindow(window)));
    return {
        static_cast<float>(GET_X_LPARAM(lParam)) * scale,
        static_cast<float>(GET_Y_LPARAM(lParam)) * scale,
    };
}

const WaveformLevelV1* chooseLevel(
    const WaveformPyramidV1& pyramid,
    double durationSeconds,
    float width) noexcept
{
    if (width <= 0.0f || durationSeconds <= 0.0) return nullptr;
    const auto millisecondsPerDip =
        durationSeconds * 1000.0 / static_cast<double>(width);
    const WaveformLevelV1* selected = nullptr;
    for (const auto& level : pyramid.levels) {
        if (level.bins.empty()) continue;
        if (selected == nullptr ||
            level.resolutionMs <=
                std::max(10.0, millisecondsPerDip * 1.35)) {
            selected = &level;
        }
    }
    return selected;
}

const WaveformLevelV1* chooseDetailedLevel(
    const WaveformPyramidV1& pyramid,
    unsigned physicalColumns,
    double durationSeconds) noexcept
{
    if (physicalColumns == 0) return nullptr;
    constexpr std::size_t kMaximumSourceBinsPerPixel = 96;
    for (const auto& level : pyramid.levels) {
        if (level.bins.empty() || level.resolutionMs == 0) continue;
        const auto expectedBins = durationSeconds > 0.0
            ? static_cast<std::size_t>(std::ceil(
                durationSeconds * 1000.0 /
                static_cast<double>(level.resolutionMs)))
            : level.bins.size();
        if (expectedBins <=
                static_cast<std::size_t>(physicalColumns) *
                    kMaximumSourceBinsPerPixel) {
            return &level;
        }
    }
    for (auto iterator = pyramid.levels.rbegin();
         iterator != pyramid.levels.rend(); ++iterator) {
        if (!iterator->bins.empty()) return &*iterator;
    }
    return nullptr;
}

float detailU16(std::uint16_t value) noexcept
{
    return static_cast<float>(value) / 65535.0f;
}

float detailU8(std::uint8_t value) noexcept
{
    return static_cast<float>(value) / 255.0f;
}

struct WaveformColumnV2 {
    float left = 0.0f;
    float right = 0.0f;
    float leftPeak = 0.0f;
    float rightPeak = 0.0f;
    float leftShoulder = 0.0f;
    float rightShoulder = 0.0f;
    float leftBody = 0.0f;
    float rightBody = 0.0f;
    float leftCore = 0.0f;
    float rightCore = 0.0f;
    float low = 0.0f;
    float mid = 0.0f;
    float high = 0.0f;
    float transient = 0.0f;
};

void appendCostRectangle(
    ID2D1GeometrySink* sink,
    float left,
    float top,
    float right,
    float bottom)
{
    if (sink == nullptr || right <= left || bottom <= top) return;
    sink->BeginFigure(
        D2D1::Point2F(left, top),
        D2D1_FIGURE_BEGIN_FILLED);
    const std::array points{
        D2D1::Point2F(right, top),
        D2D1::Point2F(right, bottom),
        D2D1::Point2F(left, bottom),
    };
    sink->AddLines(points.data(), static_cast<UINT32>(points.size()));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
}

class WaveSeekPanel
    : public uie::container_uie_window_v3 {
public:
    const GUID& get_extension_guid() const override
    {
        return kWaveSeekPanelGuid;
    }

    void get_name(pfc::string_base& output) const override
    {
        output = "GODENplayer WaveSeek";
    }

    void get_category(pfc::string_base& output) const override
    {
        output = "Panels";
    }

    unsigned get_type() const override
    {
        return uie::type_panel;
    }

private:
    uie::container_window_v3_config get_window_config() override
    {
        return {L"NekoRoXWaveSeekV3", false};
    }

    LRESULT on_message(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam) override
    {
        if (message == waveSeekStateMessage()) {
            if (wParam == 1001U && lParam == 1) {
                if (!qa::hooksAllowed()) return E_ACCESSDENIED;
                wchar_t hooks[4]{}, profile[4]{};
                if (GetEnvironmentVariableW(L"NEKOROX_QA_HOOKS", hooks, 4) != 1 ||
                    hooks[0] != L'1' ||
                    GetEnvironmentVariableW(L"NEKOROX_QA_PROFILE", profile, 4) != 1 ||
                    profile[0] != L'1') return 0;
                qaRejectNextDraw_ = true;
                geometryFramePending_ = true;
                syncClock();
                (void)frameClock_.requestImmediate();
                InvalidateRect(window, nullptr, FALSE);
                return 1;
            }
            return qaState(wParam);
        }
        switch (message) {
        case WM_GETMINMAXINFO:
            if (auto* limits = reinterpret_cast<MINMAXINFO*>(lParam)) {
                const auto dpi = std::max<UINT>(1U, GetDpiForWindow(window));
                const auto height = static_cast<LONG>(fixedBandHeightPixels(
                    UiFixedBand::waveSeek, dpi));
                limits->ptMinTrackSize.y = height;
                limits->ptMaxTrackSize.y = height;
            }
            return 0;
        case WM_CREATE:
            window_ = window;
            frameClock_.attach(window_, kWaveSeekFrameMessage);
            frameClock_.setTelemetrySurface(TelemetrySurface::waveSeek);
            frameClock_.setLatencyCritical(true);
            ThemeService::instance().subscribe(
                window_, kThemeTransitionMessage);
            persistence_ = createWaveformPersistenceHook();
            cache_.setPersistenceHook(persistence_);
            cache_.start();
            UiInvalidationBus::instance().registerWindow(
                UiInvalidationSurface::waveSeek,
                window_,
                kWaveSeekRefreshMessage);
            refreshTrack();
            syncClock();
            return 0;
        case WM_DESTROY:
            cancelPointer(true);
            UiInvalidationBus::instance().unregisterWindow(
                UiInvalidationSurface::waveSeek,
                window_);
            frameClock_.detach();
            ThemeService::instance().unsubscribe(window_);
            cache_.stop();
            cache_.setPersistenceHook({});
            persistence_.reset();
            track_.release();
            resetWaveformGeometry();
            waveGlyphRenderer_.reset();
            resetCostGeometry();
            render_.reset();
            window_ = nullptr;
            return 0;
        case WM_ERASEBKGND:
            return TRUE;
        case WM_SHOWWINDOW:
            if (!wParam) cancelPointer(true);
            if (wParam) contentFramePending_ = true;
            syncClock();
            break;
        case WM_SIZE:
            geometryFramePending_ = true;
            resetWaveformGeometry();
            render_.resize();
            syncClock();
            (void)frameClock_.requestImmediate();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED:
            if (get_host().is_valid()) {
                get_host()->on_size_limit_change(
                    window,
                    uie::size_limit_minimum_height |
                        uie::size_limit_maximum_height);
            }
            // The host can repaint reentrantly while updating layout. Assert
            // damage after it returns and retire readiness before its target.
            frameClock_.setReadinessBinding({});
            resetWaveformGeometry();
            render_.reset();
            geometryFramePending_ = true;
            clockPaintSkipped_ = false;
            syncClock();
            (void)frameClock_.requestImmediate();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_PAINT:
            paint();
            return 0;
        case WM_LBUTTONDOWN:
            PerformanceTelemetry::instance().input(
                TelemetrySurface::waveSeek);
            pointerDown(lParam);
            return 0;
        case WM_MOUSEMOVE:
            if (pointerCaptured_ && (wParam & MK_LBUTTON) != 0) {
                pointerMove(lParam);
            } else {
                updateHover(lParam);
            }
            return 0;
        case WM_LBUTTONUP:
            pointerUp(lParam);
            return 0;
        case WM_MOUSELEAVE:
            trackingMouse_ = false;
            hover_.reset();
            requestPointerFrame();
            return 0;
        case WM_CAPTURECHANGED:
            // Losing capture is cancellation, not an implicit mouse-up. A
            // popup, Alt+Tab or another window must not commit a stray seek.
            cancelPointer();
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE && pointerCaptured_) {
                cancelPointer();
                return 0;
            }
            break;
        case WM_CANCELMODE:
            cancelPointer();
            return 0;
        case kWaveSeekRefreshMessage:
            contentFramePending_ = true;
            if (const auto dirty = UiInvalidationBus::instance().consume(
                    UiInvalidationSurface::waveSeek,
                    window);
                !any(dirty) ||
                any(dirty & (UiDirty::semantic | UiDirty::artwork))) {
                refreshTrack();
            }
            syncClock();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case kWaveSeekSnapshotMessage:
            if (static_cast<std::uint64_t>(wParam) ==
                requestGeneration_) {
                contentFramePending_ = true;
                InvalidateRect(window_, nullptr, FALSE);
                syncClock();
                (void)frameClock_.requestImmediate();
            }
            return 0;
        case kWaveSeekFrameMessage:
            if (frameClock_.consume(wParam)) {
                ScrubCoordinator::instance().pump();
                const auto skipped = frameClock_.skippedDeadlines();
                const bool recoveringDeadline =
                    skipped != observedSkippedDeadlines_;
                observedSkippedDeadlines_ = skipped;
                if (recoveringDeadline || shouldPresentFrame()) {
                    // The notice now arrives within WM_PAINT, after input;
                    // its normal handler commits this gate-approved frame.
                    InvalidateRect(window_, nullptr, FALSE);
                } else {
                    // A ready buffer is not a reason to redraw an unchanged
                    // playback pixel. Return the unused opportunity intact
                    // and ask for the next logical cursor deadline.
                    frameClock_.stop();
                    frameClock_.requestAtOrAfter(nextCursorDeadline(), kFrameReasonAmbient);
                    clockPaintSkipped_ = true;
                }
                syncClock();
                return 0;
            }
            break;
        case kThemeTransitionMessage:
            contentFramePending_ = true;
            syncClock();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    D2D1_RECT_F timelineRect() const noexcept
    {
        const auto size = render_.sizeDip();
        return D2D1::RectF(
            4.0f,
            2.0f,
            std::max(4.0f, size.width - 4.0f),
            std::max(2.0f, size.height - 2.0f));
    }

    D2D1_RECT_F costRect() const noexcept
    {
        const auto timeline = timelineRect();
        const auto height = std::max(0.0f, timeline.bottom - timeline.top);
        const auto costHeight = std::clamp(height * 0.20f, 4.0f, 6.0f);
        return D2D1::RectF(
            timeline.left, timeline.top,
            timeline.right, std::min(timeline.bottom, timeline.top + costHeight));
    }

    D2D1_RECT_F waveformRect() const noexcept
    {
        const auto timeline = timelineRect();
        const auto cost = costRect();
        const auto height = std::max(0.0f, timeline.bottom - timeline.top);
        const auto gap = std::min(
            kWaveSeekLaneGapDip,
            std::max(2.0f, height * 0.125f));
        return D2D1::RectF(
            timeline.left,
            std::min(timeline.bottom, cost.bottom + gap),
            timeline.right,
            timeline.bottom);
    }

    LRESULT qaState(WPARAM query) const noexcept
    {
        if (query == 1U) {
            return static_cast<LRESULT>(firstVisualLatencyUs_);
        }
        if (query == 2U) {
            return static_cast<LRESULT>(std::llround(
                std::clamp(geometryCoverage_, 0.0f, 1.0f) *
                1'000'000.0f));
        }
        if (query == 3U) {
            return static_cast<LRESULT>(requestGeneration_);
        }
        if (query == 4U) {
            return static_cast<LRESULT>(playbackPaints_);
        }
        if (query == 5U) {
            return static_cast<LRESULT>(frameClock_.deliveredPulses());
        }
        if (query == 6U) {
            return static_cast<LRESULT>(frameClock_.skippedDeadlines());
        }
        if (query == 7U) {
            return static_cast<LRESULT>(frameClock_.targetRefreshMilliHz());
        }
        if (query == 8U || query == 9U) {
            const auto value = query == 8U ? hover_ : presentedHover_;
            return value ? static_cast<LRESULT>(std::lround(*value * 1000.f)) : -1;
        }
        if (query == 10U) return pointerFramePending_;
        if (query == 11U) return pointerCaptured_;
        if (query == 12U) return static_cast<LRESULT>(scrubSession_);
        if (query == 13U) return ScrubCoordinator::instance().owns(scrubSession_);
        if (query == 14U) return ScrubCoordinator::instance().active();
        if (query == 15U) return static_cast<LRESULT>(
            ScrubCoordinator::instance().snapshot().generation);
        if (query == 16U) return ScrubCoordinator::audioSmoothingActive();
        if (query == 17U) return static_cast<LRESULT>(
            ScrubCoordinator::instance().snapshot().state);
        if (query == 18U) return static_cast<LRESULT>(std::llround(
            ScrubCoordinator::instance().snapshot().visualSeconds * 1000.0));
        if (query == 19U) return render_.lastFrameOutcome().drawHr;
        if (query == 20U) return render_.lastFrameOutcome().presentHr;
        if (query == 21U) return static_cast<LRESULT>(render_.lastFrameResult());
        if (query == 22U) return static_cast<LRESULT>(render_.resourceEpoch());
        if (query == 23U) return geometryFramePending_;
        if (query == 24U) return clockPaintSkipped_;
        if (query == 25U) return qaRejectedDrawHr_;
        if (query == 26U) return qaRejectedDraws_;

        std::uint32_t state = 0;
        if (rmsGeometry_ != nullptr) {
            state |= nekorox::kWaveSeekGeometryReadyBitV1;
        }
        if (geometryComplete_) {
            state |= nekorox::kWaveSeekCompleteBitV1;
        }
        const auto transition = motion_.sample(kWaveSeekReconstructMotion);
        const auto coverageTransition =
            motion_.sample(kWaveSeekCoverageMotion);
        if (!geometryComplete_ || transition.active ||
            coverageTransition.active ||
            outgoingRmsGeometry_ != nullptr ||
            outgoingPeakGeometry_ != nullptr) {
            state |= nekorox::kWaveSeekReconstructingBitV1;
        }
        const auto snapshot = cache_.snapshot();
        if (snapshot) {
            state |= static_cast<std::uint32_t>(snapshot->state) << 8U;
        }
        return static_cast<LRESULT>(state);
    }

    void paint()
    {
        auto& telemetry = PerformanceTelemetry::instance();
        telemetry.paint(TelemetrySurface::waveSeek);
        telemetry.frameAttempt(TelemetrySurface::waveSeek);
        PAINTSTRUCT paintStruct{};
        BeginPaint(window_, &paintStruct);
        if (preparationRetryQpc_ != 0 && waveSeekQpcNow() < preparationRetryQpc_) {
            EndPaint(window_, &paintStruct);
            syncClock();
            return;
        }
        const auto clockPaintSkipped = std::exchange(clockPaintSkipped_, false);
        if (clockPaintSkipped && !shouldPresentFrame()) {
            // FrameNotification has already invalidated the owner to deliver
            // this notice. Validate its damage without promoting a deliberately
            // skipped cursor tick to new geometry demand.
            EndPaint(window_, &paintStruct);
            return;
        }
        if (!render_.prepare(window_)) {
            pointerFramePending_ = true;
            preparationRetryQpc_ = waveSeekRetryDeadline();
            frameClock_.stop();
            frameClock_.setReadinessBinding({});
            syncClock();
            EndPaint(window_, &paintStruct);
            return;
        }
        preparationRetryQpc_ = 0;
        frameClock_.setReadinessBinding(render_.bindingKey());
        if (!frameClock_.beginPresentationOpportunity()) {
            geometryFramePending_ = true;
            syncClock();
            (void)frameClock_.requestImmediate();
            EndPaint(window_, &paintStruct);
            return;
        }
        const auto prepareSpan = telemetry.beginSpan(
            TelemetrySurface::waveSeek, 1U);
        // Both waveform layers have their own raster caches. This owner
        // always paints a complete frame and never reuses PanelD2D's backing.
        if (!render_.beginDirect(window_, paintStruct.hdc)) {
            pointerFramePending_ = true;
            preparationRetryQpc_ = waveSeekRetryDeadline();
            frameClock_.stop();
            telemetry.endSpan(TelemetrySurface::waveSeek, 1U, prepareSpan, 0U);
            telemetry.frameResult(TelemetrySurface::waveSeek, E_FAIL, false);
            EndPaint(window_, &paintStruct);
            frameClock_.setReadinessBinding(render_.bindingKey());
            syncClock();
            frameClock_.completePresentationOpportunity(true);
            return;
        }

        const auto size = render_.sizeDip();
        const auto timeline = timelineRect();
        const auto cost = costRect();
        const auto viewport = waveformRect();
        render_.line(
            0.0f, 0.0f, size.width, 0.0f,
            PanelColor::border);

        const auto playback = Runtime::instance().playbackSnapshot();
        telemetry.inputIncorporated(TelemetrySurface::waveSeek);
        telemetry.endSpan(TelemetrySurface::waveSeek, 1U, prepareSpan, 1U);
        const auto hasTrack = playback.trackIdentityHash != 0;
        if (hasTrack) drawCostRibbon(cost);
        else resetCostGeometry();
        render_.line(
            0.0f, size.height - 1.0f,
            size.width, size.height - 1.0f,
            PanelColor::border);
        render_.line(
            0.0f, 0.0f,
            0.0f, size.height,
            PanelColor::border);
        render_.line(
            size.width - 1.0f, 0.0f,
            size.width - 1.0f, size.height,
            PanelColor::border);

        const auto snapshot = cache_.snapshot();
        const auto pyramid = snapshot ? snapshot->pyramid : nullptr;
        const auto control = playback_control::get();
        const auto duration = playback.durationSeconds;
        auto* target = render_.target();
        const auto center =
            (viewport.top + viewport.bottom) * 0.5f;
        render_.line(
            viewport.left,
            center,
            viewport.right,
            center,
            PanelColor::border,
            0.65f);

        const auto backendPosition =
            Runtime::instance().presentationPlaybackPosition();
        const auto position = ScrubCoordinator::instance().effectivePosition(
            backendPosition);
        const auto scrub = ScrubCoordinator::instance().snapshot();
        if (duration > 0.0) {
            if (pyramid) {
                drawWaveform(pyramid, duration, position, viewport);
            } else if (rmsGeometry_) {
                drawPresentedWaveform(duration, position, viewport);
            }
        }

        const auto playheadStep = playheadPresentationStep(
            position, duration);
        if (duration > 0.0) {
            const auto progress = std::clamp(
                position / duration, 0.0, 1.0);
            const auto unalignedX = viewport.left +
                static_cast<float>(progress) *
                    (viewport.right - viewport.left);
            const auto scale = static_cast<float>(
                std::max<UINT>(1U, GetDpiForWindow(window_))) / 96.0f;
            const auto firstPixel = std::floor(viewport.left * scale);
            const auto lastPixel = std::max(
                firstPixel, std::ceil(viewport.right * scale) - 1.0f);
            const auto pixel = std::clamp(
                std::floor(unalignedX * scale), firstPixel, lastPixel);
            const auto x = (pixel + 0.5f) / scale;
            render_.line(
                x, timeline.top, x, timeline.bottom,
                PanelColor::accent, 1.0f / scale);
        }
        if (hover_) {
            auto* brush = render_.brush(PanelColor::bright);
            const auto previous = brush->GetOpacity();
            brush->SetOpacity(0.38f);
            target->DrawLine(
                D2D1::Point2F(*hover_, timeline.top),
                D2D1::Point2F(*hover_, timeline.bottom),
                brush,
                0.75f);
            brush->SetOpacity(previous);
            if (hasTrack) {
                const auto hoverSeconds = duration > 0.0
                    ? normalizedPosition(*hover_) * duration : 0.0;
                const auto label = duration > 0.0
                    ? hoverTime(hoverSeconds)
                    : L"";
                if (!label.empty()) {
                    const auto labelWidth = std::clamp(
                        12.0f + static_cast<float>(label.size()) * 5.5f,
                        34.0f,
                        58.0f);
                    const auto labelLeft = std::clamp(
                        *hover_ - labelWidth * 0.5f,
                        timeline.left,
                        std::max(timeline.left, timeline.right - labelWidth));
                    const auto labelRect = D2D1::RectF(
                        labelLeft,
                        std::max(timeline.top, timeline.bottom - 17.0f),
                        std::min(timeline.right, labelLeft + labelWidth),
                        timeline.bottom);
                    render_.fill(labelRect, PanelColor::background, 0.64f);
                    render_.text(label, labelRect, PanelText::center,
                        PanelColor::bright);
                }
            }
        }
        if (scrub.auditionUnavailable) {
            const auto notice = D2D1::RectF(
                timeline.left + 6.0f, timeline.top + 2.0f,
                std::min(timeline.right, timeline.left + 190.0f),
                timeline.top + 17.0f);
            render_.fill(notice, PanelColor::background, 0.72f);
            render_.text(L"PREVIEW UNAVAILABLE \u00B7 SILENT SELECT",
                notice, PanelText::caption, PanelColor::muted);
        }
        if (snapshot &&
            snapshot->state == WaveformCacheState::decoding) {
            const auto progress = static_cast<float>(
                std::clamp(snapshot->progress, 0.0, 1.0));
            render_.line(
                viewport.left,
                viewport.bottom,
                viewport.left +
                    (viewport.right - viewport.left) * progress,
                viewport.bottom,
                PanelColor::accent,
                1.0f);
        }

        // The disposable QA profile can invalidate one real draw to exercise
        // recovery while paused, without resetting a driver or another app.
        const auto rejectDraw = std::exchange(qaRejectNextDraw_, false);
        if (rejectDraw) target->PopAxisAlignedClip();
        const auto result = render_.end();
        if (rejectDraw) {
            qaRejectedDrawHr_ = render_.lastFrameOutcome().drawHr;
            ++qaRejectedDraws_;
        }
        // EndDraw/Present can retire the surface. Never keep waiting on the
        // old swapchain's credit after it has been discarded on failure.
        frameClock_.setReadinessBinding(render_.bindingKey());
        telemetry.endDraw(TelemetrySurface::waveSeek, result);
        telemetry.present(TelemetrySurface::waveSeek, result);
        const bool accepted = render_.lastFrameResult() == FrameResult::accepted;
        telemetry.frameResult(
            TelemetrySurface::waveSeek, result, accepted);
        if (accepted) {
            geometryFramePending_ = false;
            contentFramePending_ = false;
            pointerFramePending_ = false;
            presentedHover_ = hover_;
            playheadPresentation_.markPresented(
                playheadStep,
                qpcSeconds(),
                frameClock_.targetRefreshMilliHz());
            if (control->is_playing() && !control->is_paused()) {
                ++playbackPaints_;
            }
        }
        else {
            pointerFramePending_ = true;
        }
        EndPaint(window_, &paintStruct);
        syncClock();
        const bool semantic = geometryFramePending_ || contentFramePending_ || pointerFramePending_ || pointerCaptured_ ||
            ScrubCoordinator::instance().active() || motion_.needsFrame() ||
            ThemeService::instance().transitionActive() ||
            (geometryGeneration_ != 0 && !geometryComplete_);
        frameClock_.completePresentationOpportunity(
            semantic || (control->is_playing() && !control->is_paused()),
            semantic ? 0 : nextCursorDeadline(),
            semantic ? 0 : kFrameReasonAmbient);
    }

    void drawCostRibbon(const D2D1_RECT_F& ribbon)
    {
        const auto cost = PacketCostController::instance().readyResult();
        render_.fill(ribbon, PanelColor::panel);
        if (!cost || cost->levels.empty() || cost->durationSeconds <= 0.0) {
            resetCostGeometry();
            render_.line(ribbon.left, (ribbon.top + ribbon.bottom) * 0.5f,
                ribbon.right, (ribbon.top + ribbon.bottom) * 0.5f,
                PanelColor::border, 0.65f);
            return;
        }
        const auto width = std::max(1.0f, ribbon.right - ribbon.left);
        const auto targetMilliseconds = cost->durationSeconds * 1000.0 / width;
        const PacketCostLevel* level = &cost->levels.front();
        auto bestDistance = std::numeric_limits<double>::infinity();
        for (const auto& candidate : cost->levels) {
            if (candidate.bins.empty() || candidate.resolutionMilliseconds == 0) continue;
            const auto distance = std::abs(std::log(
                std::max(1.0, static_cast<double>(candidate.resolutionMilliseconds)) /
                std::max(1.0, targetMilliseconds)));
            if (distance < bestDistance) {
                bestDistance = distance;
                level = &candidate;
            }
        }
        drawCostMap(cost, *level, ribbon);
    }

    bool buildCostGeometry(
        const std::shared_ptr<const PacketCostResult>& cost,
        const PacketCostLevel& level,
        const D2D1_RECT_F& ribbon)
    {
        resetCostGeometry();
        auto* target = render_.target();
        const auto width = ribbon.right - ribbon.left;
        const auto height = ribbon.bottom - ribbon.top;
        if (target == nullptr || level.bins.empty() || width <= 1.0f ||
                height <= 0.0f || cost->durationSeconds <= 0.0) {
            return false;
        }
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        target->GetFactory(factory.GetAddressOf());
        if (!factory) return false;

        std::array<Microsoft::WRL::ComPtr<ID2D1GeometrySink>, 6> sinks;
        for (std::size_t tier = 0; tier < costGeometry_.size(); ++tier) {
            if (FAILED(factory->CreatePathGeometry(
                    costGeometry_[tier].ReleaseAndGetAddressOf())) ||
                    FAILED(costGeometry_[tier]->Open(
                        sinks[tier].ReleaseAndGetAddressOf()))) {
                resetCostGeometry();
                return false;
            }
            sinks[tier]->SetFillMode(D2D1_FILL_MODE_WINDING);
        }

        const auto center = (ribbon.top + ribbon.bottom) * 0.5f;
        const auto halfHeight = std::min(2.0f, height * 0.5f);
        struct Interval { float left = 0.0f; float right = 0.0f; };
        std::array<Interval, 6> pending;
        const auto flush = [&](std::size_t tier) {
            appendCostRectangle(sinks[tier].Get(), pending[tier].left,
                center - halfHeight, pending[tier].right, center + halfHeight);
        };
        for (const auto& bin : level.bins) {
            const auto tier = static_cast<std::size_t>(std::clamp(
                static_cast<int>(bin.normalized * costGeometry_.size()),
                0,
                static_cast<int>(costGeometry_.size() - 1U)));
            const auto start = std::clamp(
                bin.startSeconds / cost->durationSeconds, 0.0, 1.0);
            const auto end = std::clamp(
                (bin.startSeconds + bin.durationSeconds) /
                    cost->durationSeconds,
                0.0,
                1.0);
            const auto left = ribbon.left + static_cast<float>(start) * width;
            auto right = ribbon.left + static_cast<float>(end) * width;
            right = std::min(ribbon.right, std::max(right, left + 0.5f));
            right = std::min(ribbon.right, right + 0.12f);
            if (right <= left) continue;
            auto& run = pending[tier];
            // Each tier is one winding fill with a common vertical extent.
            // Union touching/overlapping intervals after endpoint rounding so a
            // constant-cost file does not retain a contour for every bin.
            if (run.right > run.left && left <= run.right && right >= run.left) {
                run.left = std::min(run.left, left);
                run.right = std::max(run.right, right);
            }
            else {
                flush(tier);
                run = {left, right};
            }
        }
        for (std::size_t tier = 0; tier < sinks.size(); ++tier) {
            flush(tier);
            if (FAILED(sinks[tier]->Close())) {
                resetCostGeometry();
                return false;
            }
        }
        costGeometryResult_ = cost;
        costGeometryWidth_ = width;
        costGeometryTop_ = ribbon.top;
        costGeometryBottom_ = ribbon.bottom;
        costGeometryResolution_ = level.resolutionMilliseconds;
        return true;
    }

    bool drawCostMap(
        const std::shared_ptr<const PacketCostResult>& cost,
        const PacketCostLevel& level,
        const D2D1_RECT_F& ribbon)
    {
        const auto width = ribbon.right - ribbon.left;
        if (costGeometryResult_ != cost ||
                std::abs(costGeometryWidth_ - width) > 0.01f ||
                std::abs(costGeometryTop_ - ribbon.top) > 0.01f ||
                std::abs(costGeometryBottom_ - ribbon.bottom) > 0.01f ||
                costGeometryResolution_ != level.resolutionMilliseconds) {
            if (!buildCostGeometry(cost, level, ribbon)) return false;
        }

        UiColor accent = UiDesignSystemV1::color(
            UiColorToken::accentPrimary);
        if (const auto* accentBrush = render_.brush(PanelColor::accent)) {
            const auto color = accentBrush->GetColor();
            accent = {color.r, color.g, color.b, 1.0f};
        }
        auto tone = toOklch(accent);
        // The cost ribbon is supporting information. Keep all six storage
        // tiers readable, but never let its brightest tier overpower the
        // substantially larger waveform lane below it.
        tone.chroma = std::min(tone.chroma, 0.055f);
        tone.alpha = 0.72f;
        constexpr std::array lightness{
            0.26f,
            0.32f,
            0.38f,
            0.44f,
            0.51f,
            0.59f,
        };
        for (std::size_t tier = 0; tier < costGeometry_.size(); ++tier) {
            tone.lightness = lightness[tier];
            render_.fillGeometry(
                costGeometry_[tier].Get(),
                fromOklch(tone));
        }
        return true;
    }

    void resetCostGeometry() noexcept
    {
        for (auto& geometry : costGeometry_) geometry.Reset();
        costGeometryResult_.reset();
        costGeometryWidth_ = 0.0f;
        costGeometryTop_ = 0.0f;
        costGeometryBottom_ = 0.0f;
        costGeometryResolution_ = 0;
    }

    void drawWaveform(
        const std::shared_ptr<const WaveformPyramidV1>& pyramid,
        double duration,
        double position,
        const D2D1_RECT_F& viewport)
    {
        if (!pyramid) return;
        auto* target = render_.target();
        if (!ensureWaveformGeometry(pyramid, duration, viewport) ||
            target == nullptr) {
            return;
        }
        if (pyramid->generation == requestGeneration_ &&
            firstVisualLatencyUs_ == 0 && requestStartedSeconds_ > 0.0) {
            firstVisualLatencyUs_ = static_cast<std::uint64_t>(std::max(
                1.0,
                (qpcSeconds() - requestStartedSeconds_) * 1'000'000.0));
        }
        drawPresentedWaveform(duration, position, viewport);
    }

    void drawPresentedWaveform(
        double duration,
        double position,
        const D2D1_RECT_F& viewport)
    {
        auto* target = render_.target();
        if (target == nullptr || !rmsGeometry_) return;
        const auto previousMode = target->GetAntialiasMode();
        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        const auto played = std::clamp(
            position / duration, 0.0, 1.0);
        lastRenderedProgress_ = static_cast<float>(played);
        const auto playedX = viewport.left +
            (viewport.right - viewport.left) *
                static_cast<float>(played);

        auto fillGeometry = [&](ID2D1Geometry* geometry,
                                const D2D1_RECT_F& clip,
                                PanelColor color,
                                float opacity) {
            auto* brush = render_.brush(color);
            if (geometry == nullptr || brush == nullptr ||
                clip.right <= clip.left || clip.bottom <= clip.top ||
                opacity <= 0.001f) {
                return;
            }
            const auto previousOpacity = brush->GetOpacity();
            brush->SetOpacity(opacity);
            target->PushAxisAlignedClip(
                clip, D2D1_ANTIALIAS_MODE_ALIASED);
            target->FillGeometry(geometry, brush);
            target->PopAxisAlignedClip();
            brush->SetOpacity(previousOpacity);
        };
        const auto playedClip = D2D1::RectF(
            viewport.left, viewport.top, playedX, viewport.bottom);
        const auto futureClip = D2D1::RectF(
            playedX, viewport.top, viewport.right, viewport.bottom);
        const auto transition = motion_.sample(kWaveSeekReconstructMotion);
        const auto retainedReady = ensureRetainedWaveform();
        const auto outgoingPlayedX = viewport.left +
            (viewport.right - viewport.left) *
                std::clamp(outgoingPlayedProgress_, 0.0f, 1.0f);
        const auto outgoingPlayedClip = D2D1::RectF(
            viewport.left, viewport.top,
            outgoingPlayedX, viewport.bottom);
        const auto outgoingFutureClip = D2D1::RectF(
            outgoingPlayedX, viewport.top,
            viewport.right, viewport.bottom);

        auto drawLayers = [&](ID2D1Geometry* rms,
                              const D2D1_RECT_F& clip,
                              float opacity) {
            const auto played = intersectRect(clip, playedClip);
            const auto future = intersectRect(clip, futureClip);

            // Reconstruction uses the same raw dense envelope as the retained
            // bitmap. The old peak shell produced a faint wall on compressed
            // masters and a visible shape snap when reconstruction ended.
            fillGeometry(rms, future, PanelColor::secondary, 0.58f * opacity);
            fillGeometry(rms, played, PanelColor::accent, 0.88f * opacity);
        };

        const auto hasOutgoingRetained =
            outgoingFutureWaveform_ != nullptr &&
            outgoingPlayedWaveform_ != nullptr;
        const auto hasOutgoing =
            outgoingRmsGeometry_ != nullptr || hasOutgoingRetained;
        const auto coverageTransition =
            motion_.sample(kWaveSeekCoverageMotion);
        const auto presentedCoverage = std::clamp(
            static_cast<float>(coverageTransition.found
                ? coverageTransition.value
                : geometryCoverage_),
            0.0f,
            1.0f);
        const auto reconstructing =
            !geometryComplete_ || presentedCoverage < 0.9995f ||
            hasOutgoing || transition.active || coverageTransition.active ||
            (transition.found && transition.progress < 1.0);
        if (reconstructing) {
            const auto seed = geometryGeneration_ ^
                (geometryPyramid_
                    ? geometryPyramid_->scannedFrames
                    : 0ULL);
            const auto motionProgress = static_cast<float>(
                transition.found ? transition.value : 1.0);
            const auto coverage = presentedCoverage;
            auto drawConstructionBlock = [&](const D2D1_RECT_F& tile,
                                             float opacity) {
                auto* secondary = render_.brush(PanelColor::secondary);
                if (secondary == nullptr || opacity <= 0.001f) return;
                const auto tileWidth = tile.right - tile.left;
                const auto tileHeight = tile.bottom - tile.top;
                const auto block = D2D1::RectF(
                    tile.left + 0.75f,
                    tile.top + tileHeight * 0.18f,
                    std::max(tile.left + 0.75f, tile.right - 0.75f),
                    tile.bottom - tileHeight * 0.18f);
                const auto oldSecondaryOpacity = secondary->GetOpacity();
                secondary->SetOpacity(0.075f * opacity);
                target->FillRectangle(block, secondary);
                secondary->SetOpacity(oldSecondaryOpacity);
            };
            std::array<GlyphFlowOverlayCell,
                kWaveSeekReconstructTiles> glyphs{};
            std::size_t glyphCount = 0;
            for (unsigned tile = 0;
                 tile < kWaveSeekReconstructTiles;
                 ++tile) {
                const auto left = viewport.left +
                    (viewport.right - viewport.left) *
                    static_cast<float>(tile) /
                    static_cast<float>(kWaveSeekReconstructTiles);
                const auto right = viewport.left +
                    (viewport.right - viewport.left) *
                    static_cast<float>(tile + 1U) /
                    static_cast<float>(kWaveSeekReconstructTiles);
                const auto hash = mixBits(seed + tile * 0x9e3779b9U);
                const auto random = static_cast<float>(hash & 0xffffU) /
                    65535.0f;
                const auto order = 0.54f *
                    static_cast<float>(tile) /
                    static_cast<float>(kWaveSeekReconstructTiles - 1U) +
                    0.24f * random;
                const auto animatedLocal = smoothStep(
                    (motionProgress - order) / 0.22f);
                // Duration fixes every tile in its final time-domain slot.
                // Progressive snapshots reveal only material that has really
                // been decoded; a prefix is never stretched across the lane
                // and completion never needs a second whole-lane swap.
                const auto coverageLocal = smoothStep(std::clamp(
                    coverage * kWaveSeekReconstructTiles -
                        static_cast<float>(tile),
                    0.0f,
                    1.0f));
                const auto clip = D2D1::RectF(
                    left, viewport.top, right, viewport.bottom);
                const auto knownRight = left +
                    (right - left) * coverageLocal;
                const auto knownClip = D2D1::RectF(
                    left, viewport.top, knownRight, viewport.bottom);
                const auto unknownClip = D2D1::RectF(
                    knownRight, viewport.top, right, viewport.bottom);

                const auto outgoingDrawn = hasOutgoingRetained &&
                    drawRetainedPair(
                        outgoingFutureWaveform_.Get(),
                        outgoingPlayedWaveform_.Get(),
                        outgoingPlayedClip,
                        outgoingFutureClip,
                        clip,
                        1.0f - animatedLocal);
                if (!outgoingDrawn) {
                    drawLayers(
                        outgoingRmsGeometry_.Get(),
                        clip,
                        1.0f - animatedLocal);
                }
                const auto incomingDrawn = retainedReady &&
                    drawRetainedPair(
                        futureWaveform_.Get(),
                        playedWaveform_.Get(),
                        playedClip,
                        futureClip,
                        knownClip,
                        animatedLocal);
                if (!incomingDrawn) {
                    drawLayers(
                        rmsGeometry_.Get(),
                        knownClip,
                        animatedLocal);
                }
                if (unknownClip.right > unknownClip.left + 0.01f &&
                    animatedLocal > 0.001f) {
                    target->PushAxisAlignedClip(
                        unknownClip, D2D1_ANTIALIAS_MODE_ALIASED);
                    drawConstructionBlock(clip, animatedLocal);
                    target->PopAxisAlignedClip();
                }
                // The first pass reconstructs the complete static domain.
                // Later coverage updates animate only the block currently
                // receiving real samples, never the complete waveform again.
                const auto glyphPhase = transition.active
                    ? animatedLocal
                    : coverageLocal;
                if (motionMode() != MotionMode::off &&
                    glyphPhase > 0.035f && glyphPhase < 0.965f &&
                    glyphCount < glyphs.size()) {
                    auto& glyph = glyphs[glyphCount++];
                    glyph.xNormalized =
                        (static_cast<float>(tile) + 0.5f) /
                        static_cast<float>(kWaveSeekReconstructTiles);
                    glyph.yNormalized = 0.18f + 0.64f *
                        static_cast<float>((hash >> 16U) & 0xffU) /
                        255.0f;
                    glyph.sizeDip = 1.55f +
                        static_cast<float>((hash >> 24U) & 0x3U) *
                            0.36f;
                    glyph.opacity = std::clamp(
                        (1.0f - std::abs(glyphPhase * 2.0f - 1.0f)) *
                            0.82f,
                        0.0f,
                        0.82f);
                    glyph.glyphIndex = static_cast<std::uint8_t>(
                        (hash >> 8U) % 6U);
                }
            }
            if (glyphCount != 0) {
                const auto theme = ThemeService::instance().snapshot();
                waveGlyphRenderer_.drawOverlay(
                    target,
                    viewport,
                    std::span<const GlyphFlowOverlayCell>(
                        glyphs.data(), glyphCount),
                    D2D1::ColorF(
                        theme.highlight.red,
                        theme.highlight.green,
                        theme.highlight.blue,
                        0.94f));
            }

            if (geometryComplete_ && presentedCoverage >= 0.9995f &&
                !coverageTransition.active && !transition.active &&
                (!transition.found || transition.progress >= 1.0)) {
                outgoingRmsGeometry_.Reset();
                outgoingPeakGeometry_.Reset();
                outgoingFutureWaveform_.Reset();
                outgoingPlayedWaveform_.Reset();
                motion_.release(kWaveSeekReconstructMotion);
                motion_.release(kWaveSeekCoverageMotion);
            }
        } else {
            if (!drawRetainedWaveform(playedClip, futureClip)) {
                drawLayers(
                    rmsGeometry_.Get(),
                    viewport,
                    1.0f);
            }
            outgoingRmsGeometry_.Reset();
            outgoingPeakGeometry_.Reset();
            outgoingFutureWaveform_.Reset();
            outgoingPlayedWaveform_.Reset();
            motion_.release(kWaveSeekReconstructMotion);
        }
        target->SetAntialiasMode(previousMode);
    }

    static bool sameColor(
        const D2D1_COLOR_F& first,
        const D2D1_COLOR_F& second) noexcept
    {
        return std::abs(first.r - second.r) < 0.0001f &&
            std::abs(first.g - second.g) < 0.0001f &&
            std::abs(first.b - second.b) < 0.0001f &&
            std::abs(first.a - second.a) < 0.0001f;
    }

    void smoothWaveformColumns()
    {
        if (waveformColumns_.size() < 3U) return;
        const auto source = waveformColumns_;
        const auto blur = [&](float WaveformColumnV2::* member,
                              unsigned radius,
                              float rawMix) {
            for (std::size_t index = 0;
                 index < waveformColumns_.size(); ++index) {
                double weighted = 0.0;
                double weightSum = 0.0;
                for (int offset = -static_cast<int>(radius);
                     offset <= static_cast<int>(radius); ++offset) {
                    const auto sampleIndex = static_cast<std::size_t>(
                        std::clamp<std::int64_t>(
                            static_cast<std::int64_t>(index) + offset,
                            0,
                            static_cast<std::int64_t>(source.size() - 1U)));
                    const auto distance = static_cast<float>(std::abs(offset));
                    const auto weight = static_cast<double>(
                        radius + 1.0f - distance);
                    weighted += source[sampleIndex].*member * weight;
                    weightSum += weight;
                }
                const auto filtered = static_cast<float>(weighted / weightSum);
                waveformColumns_[index].*member =
                    filtered * (1.0f - rawMix) +
                    source[index].*member * rawMix;
            }
        };

        // One physical-column 1:2:1 kernels suppress quantization chatter
        // without erasing attacks. Peaks and transient strength deliberately
        // remain unfiltered; they are the high-frequency information this
        // overview must preserve. Spectral tint is slower than geometry, but
        // no longer smeared across thirteen columns.
        blur(&WaveformColumnV2::leftCore, 1U, 0.45f);
        blur(&WaveformColumnV2::rightCore, 1U, 0.45f);
        blur(&WaveformColumnV2::leftBody, 1U, 0.45f);
        blur(&WaveformColumnV2::rightBody, 1U, 0.45f);
        blur(&WaveformColumnV2::leftShoulder, 1U, 0.60f);
        blur(&WaveformColumnV2::rightShoulder, 1U, 0.60f);
        blur(&WaveformColumnV2::low, 2U, 0.55f);
        blur(&WaveformColumnV2::mid, 2U, 0.55f);
        blur(&WaveformColumnV2::high, 2U, 0.55f);

        for (auto& column : waveformColumns_) {
            column.leftCore = std::min(column.leftCore, column.leftBody);
            column.rightCore = std::min(column.rightCore, column.rightBody);
            column.leftShoulder = std::max(
                column.leftBody, column.leftShoulder);
            column.rightShoulder = std::max(
                column.rightBody, column.rightShoulder);
            column.leftPeak = std::max(
                column.leftShoulder, column.leftPeak);
            column.rightPeak = std::max(
                column.rightShoulder, column.rightPeak);
        }
    }

    bool createDetailedWaveformBitmaps(
        const std::array<D2D1_COLOR_F, 3>& colors,
        ID2D1Bitmap** futureOutput,
        ID2D1Bitmap** playedOutput)
    {
        auto* target = render_.target();
        if (target == nullptr || futureOutput == nullptr || playedOutput == nullptr ||
            waveformColumns_.empty() || geometryWidth_ <= 0.0f ||
            detailAmplitude_ <= 0.0f) {
            return false;
        }
        *futureOutput = nullptr;
        *playedOutput = nullptr;
        const TelemetrySpan rasterSpan(TelemetrySurface::waveSeek, kWaveSeekDetailRasterPhase);
        const auto pixelSize = target->GetPixelSize();
        if (pixelSize.width == 0 || pixelSize.height == 0) return false;
        float dpiX = 96.0f;
        float dpiY = 96.0f;
        target->GetDpi(&dpiX, &dpiY);
        const auto pixelCount = static_cast<std::size_t>(pixelSize.width) *
            static_cast<std::size_t>(pixelSize.height);
        std::array<std::vector<std::uint32_t>, 2> pixels;
        try {
            for (auto& layer : pixels) layer.assign(pixelCount, 0U);
        } catch (const std::bad_alloc&) {
            return false;
        }

        const auto& secondary = colors[0];
        const auto& highlight = colors[1];
        const auto& accent = colors[2];
        const auto mixColor = [](const D2D1_COLOR_F& from,
                                 const D2D1_COLOR_F& to,
                                 float amount) noexcept {
            amount = std::clamp(amount, 0.0f, 1.0f);
            return D2D1::ColorF(
                from.r + (to.r - from.r) * amount,
                from.g + (to.g - from.g) * amount,
                from.b + (to.b - from.b) * amount,
                1.0f);
        };
        // Both halves share the same material. Progress changes luminance and
        // opacity instead of switching to an unrelated second color.
        struct WaveformPalette {
            D2D1_COLOR_F body;
            D2D1_COLOR_F edge;
        };
        std::vector<float> lowDistribution;
        std::vector<float> midDistribution;
        std::vector<float> highDistribution;
        lowDistribution.reserve(waveformColumns_.size());
        midDistribution.reserve(waveformColumns_.size());
        highDistribution.reserve(waveformColumns_.size());
        for (const auto& column : waveformColumns_) {
            lowDistribution.push_back(column.low);
            midDistribution.push_back(column.mid);
            highDistribution.push_back(column.high);
        }
        const auto percentile = [](std::vector<float> values,
                                   float rank) noexcept {
            if (values.empty()) return 0.0f;
            const auto index = std::min(
                values.size() - 1U,
                static_cast<std::size_t>(std::lround(
                    std::clamp(rank, 0.0f, 1.0f) *
                    static_cast<float>(values.size() - 1U))));
            std::nth_element(
                values.begin(), values.begin() + index, values.end());
            return values[index];
        };
        // Each broad spectral band has a different natural RMS range. Use a
        // robust per-track reference before mixing them, otherwise mid/low
        // energy wins almost every pixel and the other hues disappear.
        const auto candidateLowReference = std::max(
            0.008f, percentile(std::move(lowDistribution), 0.72f));
        const auto candidateMidReference = std::max(
            0.008f, percentile(std::move(midDistribution), 0.72f));
        const auto candidateHighReference = std::max(
            0.008f, percentile(std::move(highDistribution), 0.72f));
        if (spectralCalibrationPyramid_.get() != geometryPyramid_.get()) {
            if (spectralCalibrationPyramid_ == nullptr) {
                waveformLowReference_ = candidateLowReference;
                waveformMidReference_ = candidateMidReference;
                waveformHighReference_ = candidateHighReference;
            } else if (!waveformCalibrationFrozen_) {
                constexpr float kCalibrationBlend = 0.24f;
                waveformLowReference_ +=
                    (candidateLowReference - waveformLowReference_) *
                    kCalibrationBlend;
                waveformMidReference_ +=
                    (candidateMidReference - waveformMidReference_) *
                    kCalibrationBlend;
                waveformHighReference_ +=
                    (candidateHighReference - waveformHighReference_) *
                    kCalibrationBlend;
            }
            spectralCalibrationPyramid_ = geometryPyramid_;
        }
        const auto lowReference = waveformLowReference_;
        const auto midReference = waveformMidReference_;
        const auto highReference = waveformHighReference_;
        std::array<std::vector<WaveformPalette>, 2> columnPalettes;
        for (unsigned variant = 0; variant < 2; ++variant) {
            const bool played = variant == 1;
            const auto base = played
                ? mixColor(accent, highlight, 0.06f)
                : mixColor(accent, secondary, 0.18f);
            const auto baseTone = toOklch({base.r, base.g, base.b, 1.0f});
            const auto spectralAnchor = [&](float hueOffset,
                                            float lightnessOffset,
                                            float chromaScale) noexcept {
                auto tone = baseTone;
                tone.hueDegrees += hueOffset;
                tone.lightness = std::clamp(
                    tone.lightness + lightnessOffset, 0.0f, 1.0f);
                tone.chroma = std::clamp(
                    tone.chroma * chromaScale, 0.035f, 0.22f);
                const auto converted = fromOklch(tone);
                return D2D1::ColorF(
                    converted.red, converted.green, converted.blue, 1.0f);
            };
            const auto lowAnchor = spectralAnchor(-28.0f, -0.012f, 0.94f);
            const auto midAnchor = spectralAnchor(0.0f, 0.006f, 1.00f);
            const auto highAnchor = spectralAnchor(34.0f, 0.026f, 1.08f);
            const auto blendBands = [&](float low,
                                        float mid,
                                        float high) noexcept {
                const auto sum = low + mid + high;
                if (sum <= 0.0001f) return base;
                const auto inverse = 1.0f / sum;
                return D2D1::ColorF(
                    (lowAnchor.r * low + midAnchor.r * mid +
                        highAnchor.r * high) * inverse,
                    (lowAnchor.g * low + midAnchor.g * mid +
                        highAnchor.g * high) * inverse,
                    (lowAnchor.b * low + midAnchor.b * mid +
                        highAnchor.b * high) * inverse,
                    1.0f);
            };
            auto& palettes = columnPalettes[variant];
            palettes.reserve(waveformColumns_.size());
            for (const auto& column : waveformColumns_) {
                // Root compression preserves real spectral differences while
                // preventing one broad band from becoming a winner-takes-all hue.
                const auto low = std::sqrt(std::clamp(
                    column.low / lowReference, 0.0f, 2.5f));
                const auto mid = std::sqrt(std::clamp(
                    column.mid / midReference, 0.0f, 2.5f));
                const auto high = std::sqrt(std::clamp(
                    column.high / highReference, 0.0f, 2.5f));
                auto body = blendBands(
                    low * 0.62f, mid * 0.31f, high * 0.07f);
                auto edge = blendBands(
                    low * 0.10f, mid * 0.35f, high * 0.55f);
                // Keep the signature color recognizable; the spectral anchors are
                // a material gradient around it, not a replacement palette.
                body = mixColor(base, body, 0.68f);
                edge = mixColor(base, edge, 0.80f);
                edge = mixColor(
                    edge,
                    highlight,
                    std::clamp(column.transient * 0.12f, 0.0f, 0.12f));
                palettes.push_back({body, edge});
            }

        }

        const auto smoothCoverage = [](float boundary,
                                       float position,
                                       float feather) noexcept {
            return smoothStep(
                (boundary - position + feather) /
                std::max(0.0001f, feather * 2.0f));
        };
        const auto bodyFeather = std::max(
            0.004f,
            (0.65f * 96.0f / std::max(1.0f, dpiY)) /
                detailAmplitude_);
        const auto edgeFeather = std::max(
            0.006f,
            (0.80f * 96.0f / std::max(1.0f, dpiY)) /
                detailAmplitude_);
        // Compute coverage once; keep each variant's palette, alpha clamp and
        // byte rounding independent to preserve the original raster exactly.
        const auto topDip = geometryTop_;
        const auto bottomDip = geometryTop_ + geometryHeight_;
        for (UINT y = 0; y < pixelSize.height; ++y) {
            const auto yDip =
                (static_cast<float>(y) + 0.5f) * 96.0f / dpiY;
            if (yDip < topDip || yDip >= bottomDip) continue;
            const auto upper = yDip < detailCenter_;
            const auto normalizedY = std::abs(yDip - detailCenter_) /
                detailAmplitude_;
            for (UINT x = 0; x < pixelSize.width; ++x) {
                const auto xDip =
                    (static_cast<float>(x) + 0.5f) * 96.0f / dpiX;
                const auto normalizedX =
                    (xDip - geometryLeft_) / geometryWidth_;
                if (normalizedX < 0.0f || normalizedX >= 1.0f) continue;
                // waveformColumns_ is a decoded prefix, not the horizontal
                // domain. Indexing by its current size makes that prefix get
                // resampled on every publish (most visible on long tracks).
                // Use the immutable final physical-column domain instead and
                // leave columns beyond the decoded prefix transparent.
                const auto columnIndex = static_cast<std::size_t>(
                    normalizedX *
                    static_cast<float>(geometryColumnCount_));
                if (columnIndex >= waveformColumns_.size()) continue;
                const auto& column = waveformColumns_[columnIndex];
                const auto shoulder = upper
                    ? column.leftShoulder : column.rightShoulder;
                const auto body = upper
                    ? column.leftBody : column.rightBody;
                const auto activity = smoothStep(
                    (shoulder - 0.003f) / 0.012f);
                const auto bodyAlpha = activity * 0.68f * smoothCoverage(
                    body, normalizedY, bodyFeather);
                const auto edgeAlpha = activity * 0.32f * smoothCoverage(
                    shoulder, normalizedY, edgeFeather);
                const auto edgeProgress = smoothStep(
                    normalizedY / std::max(0.001f, shoulder));
                for (unsigned variant = 0; variant < 2; ++variant) {
                    const auto opacityScale = variant == 1 ? 0.98f : 0.78f;
                    auto alpha = bodyAlpha + edgeAlpha;
                    alpha *= opacityScale *
                        (1.0f + column.transient * 0.04f);
                    alpha = std::clamp(alpha, 0.0f, 0.92f);
                    if (alpha <= 0.001f) continue;
                    const auto& palette = columnPalettes[variant][columnIndex];
                    // Continuous vertical material: body at the centre,
                    // dense envelope at the silhouette. True peaks deliberately
                    // do not participate in WaveSeek presentation.
                    const auto color = mixColor(
                        palette.body,
                        palette.edge,
                        edgeProgress);
                    const auto byte = [](float value) noexcept {
                        return static_cast<std::uint32_t>(std::lround(
                            std::clamp(value, 0.0f, 1.0f) * 255.0f));
                };
                const auto a = byte(alpha);
                const auto r = byte(color.r * alpha);
                const auto g = byte(color.g * alpha);
                const auto b = byte(color.b * alpha);
                pixels[variant][static_cast<std::size_t>(y) * pixelSize.width + x] =
                    b | (g << 8U) | (r << 16U) | (a << 24U);
                }
            }
        }

        const auto properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED),
            dpiX,
            dpiY);
        return SUCCEEDED(target->CreateBitmap(
            pixelSize, pixels[0].data(),
            pixelSize.width * sizeof(std::uint32_t), &properties, futureOutput)) &&
            SUCCEEDED(target->CreateBitmap(
                pixelSize, pixels[1].data(),
                pixelSize.width * sizeof(std::uint32_t), &properties, playedOutput));
    }

    void resetRetainedWaveform() noexcept
    {
        futureWaveform_.Reset();
        playedWaveform_.Reset();
        retainedTarget_.Reset();
        retainedColorsValid_ = false;
        retainedSize_ = {};
    }

    bool createLegacyWaveformLayer(
        bool played,
        D2D1_SIZE_F size,
        const std::array<D2D1_COLOR_F, 3>& colors,
        ID2D1Bitmap** output)
    {
        auto* target = render_.target();
        if (target == nullptr || output == nullptr || rmsGeometry_ == nullptr) {
            return false;
        }
        *output = nullptr;
        const auto pixelSize = target->GetPixelSize();
        const auto pixelFormat = target->GetPixelFormat();
        Microsoft::WRL::ComPtr<ID2D1BitmapRenderTarget> layer;
        if (FAILED(target->CreateCompatibleRenderTarget(
                &size,
                &pixelSize,
                &pixelFormat,
                D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE,
                layer.GetAddressOf()))) {
            return false;
        }

        const auto& secondary = colors[0];
        const auto& highlight = colors[1];
        const auto& accent = colors[2];
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> peakBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> rmsBrush;
        const auto peakColor = played ? highlight : secondary;
        const auto rmsColor = played ? accent : secondary;
        if (FAILED(layer->CreateSolidColorBrush(
                peakColor,
                D2D1::BrushProperties(played ? 0.24f : 0.13f),
                peakBrush.GetAddressOf())) ||
            FAILED(layer->CreateSolidColorBrush(
                rmsColor,
                D2D1::BrushProperties(played ? 0.88f : 0.32f),
                rmsBrush.GetAddressOf()))) {
            return false;
        }

        layer->BeginDraw();
        layer->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        layer->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        if (peakGeometry_ != nullptr) {
            layer->FillGeometry(peakGeometry_.Get(), peakBrush.Get());
        }
        layer->FillGeometry(rmsGeometry_.Get(), rmsBrush.Get());
        if (FAILED(layer->EndDraw())) return false;
        return SUCCEEDED(layer->GetBitmap(output));
    }

    bool ensureRetainedWaveform()
    {
        auto* target = render_.target();
        if (target == nullptr || rmsGeometry_ == nullptr) return false;
        const auto size = render_.sizeDip();
        auto* secondaryBrush = render_.brush(PanelColor::secondary);
        auto* highlightBrush = render_.brush(PanelColor::highlight);
        auto* accentBrush = render_.brush(PanelColor::accent);
        if (secondaryBrush == nullptr || highlightBrush == nullptr ||
            accentBrush == nullptr) {
            return false;
        }
        // Build the incoming waveform directly in the destination artwork
        // palette.  The outgoing retained bitmaps already own their original
        // colour; sampling intermediate theme colours here both recoloured the
        // wrong track and forced expensive full-width re-rasterisation.
        const auto themeTarget = ThemeService::instance().targetSnapshot();
        const std::array<D2D1_COLOR_F, 3> colors{
            D2D1_COLOR_F{themeTarget.secondary.red,
                themeTarget.secondary.green, themeTarget.secondary.blue, 1.0f},
            D2D1_COLOR_F{themeTarget.highlight.red,
                themeTarget.highlight.green, themeTarget.highlight.blue, 1.0f},
            D2D1_COLOR_F{themeTarget.accent.red,
                themeTarget.accent.green, themeTarget.accent.blue, 1.0f},
        };
        const bool colorsMatch = retainedColorsValid_ &&
            sameColor(retainedColors_[0], colors[0]) &&
            sameColor(retainedColors_[1], colors[1]) &&
            sameColor(retainedColors_[2], colors[2]);
        const bool retainedGeometryMatches =
            futureWaveform_ != nullptr && playedWaveform_ != nullptr &&
            retainedTarget_.Get() == target &&
            std::abs(retainedSize_.width - size.width) < 0.01f &&
            std::abs(retainedSize_.height - size.height) < 0.01f;
        if (retainedGeometryMatches) {
            if (colorsMatch) return true;
            // targetSnapshot is stable throughout the palette transition, so
            // a mismatch means the artwork destination genuinely changed.
        }

        resetRetainedWaveform();
        retainedTarget_ = target;
        const bool created = detailedWaveform_
            ? createDetailedWaveformBitmaps(colors,
                futureWaveform_.GetAddressOf(), playedWaveform_.GetAddressOf())
            : createLegacyWaveformLayer(false, size, colors, futureWaveform_.GetAddressOf()) &&
                createLegacyWaveformLayer(true, size, colors, playedWaveform_.GetAddressOf());
        if (!created) {
            resetRetainedWaveform();
            return false;
        }
        retainedColors_ = colors;
        retainedColorsValid_ = true;
        retainedSize_ = size;
        return true;
    }

    bool drawRetainedPair(
        ID2D1Bitmap* future,
        ID2D1Bitmap* played,
        const D2D1_RECT_F& playedClip,
        const D2D1_RECT_F& futureClip,
        const D2D1_RECT_F& revealClip,
        float opacity)
    {
        auto* target = render_.target();
        if (target == nullptr || future == nullptr || played == nullptr ||
            opacity <= 0.001f) {
            return false;
        }
        const auto size = render_.sizeDip();
        const auto destination = D2D1::RectF(
            0.0f, 0.0f, size.width, size.height);
        auto drawLayer = [&](ID2D1Bitmap* bitmap, const D2D1_RECT_F& clip) {
            const auto visible = intersectRect(clip, revealClip);
            if (bitmap == nullptr || visible.right <= visible.left ||
                visible.bottom <= visible.top) {
                return;
            }
            target->PushAxisAlignedClip(
                visible, D2D1_ANTIALIAS_MODE_ALIASED);
            target->DrawBitmap(
                bitmap,
                destination,
                opacity,
                D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            target->PopAxisAlignedClip();
        };
        drawLayer(future, futureClip);
        drawLayer(played, playedClip);
        return true;
    }

    bool drawRetainedWaveform(
        const D2D1_RECT_F& playedClip,
        const D2D1_RECT_F& futureClip)
    {
        if (!ensureRetainedWaveform()) return false;
        const auto size = render_.sizeDip();
        return drawRetainedPair(
            futureWaveform_.Get(),
            playedWaveform_.Get(),
            playedClip,
            futureClip,
            D2D1::RectF(0.0f, 0.0f, size.width, size.height),
            1.0f);
    }

    bool ensureWaveformGeometry(
        const std::shared_ptr<const WaveformPyramidV1>& pyramid,
        double duration,
        const D2D1_RECT_F& viewport)
    {
        if (!pyramid) return false;
        const auto width = viewport.right - viewport.left;
        const auto height = viewport.bottom - viewport.top;
        auto* target = render_.target();
        if (target == nullptr) return false;
        const auto dpi = static_cast<float>(
            std::max<UINT>(1U, GetDpiForWindow(window_)));
        const auto physicalColumns = std::max(
            1U,
            static_cast<unsigned>(std::ceil(width * dpi / 96.0f)));
        const auto useDetail =
            pyramid->version >= kWaveformPyramidVersion;
        const auto* level = useDetail
            ? chooseDetailedLevel(*pyramid, physicalColumns, duration)
            : chooseLevel(*pyramid, duration, width);
        if (level == nullptr || level->bins.empty() ||
            width <= 0.0f || height <= 0.0f) {
            resetWaveformGeometry();
            return false;
        }
        const auto incomingCoverage = waveformCoverage(*pyramid);
        if (geometryPyramid_.get() == pyramid.get() &&
            std::abs(geometryDuration_ - duration) < 0.0001 &&
            std::abs(geometryWidth_ - width) < 0.01f &&
            std::abs(geometryHeight_ - height) < 0.01f &&
            std::abs(geometryLeft_ - viewport.left) < 0.01f &&
            std::abs(geometryTop_ - viewport.top) < 0.01f) {
            return rmsGeometry_ != nullptr;
        }

        // Progressive snapshots are immutable, but rebuilding an entire path
        // for every decoder publish can monopolize the UI thread.  Rebuild
        // only when enough newly covered audio changes at least four visible
        // columns (or when the final complete snapshot arrives).
        const auto sameGeometryDomain =
            geometryGeneration_ == pyramid->generation &&
            std::abs(geometryDuration_ - duration) < 0.0001 &&
            std::abs(geometryWidth_ - width) < 0.01f &&
            std::abs(geometryHeight_ - height) < 0.01f &&
            std::abs(geometryLeft_ - viewport.left) < 0.01f &&
            std::abs(geometryTop_ - viewport.top) < 0.01f;
        const auto minimumCoverageStep =
            std::max(0.0025f, 4.0f / std::max(1.0f, width));
        if (sameGeometryDomain && rmsGeometry_ != nullptr &&
            !pyramid->complete() &&
            incomingCoverage < geometryCoverage_ + minimumCoverageStep) {
            return true;
        }

        const auto generationChanged =
            geometryGeneration_ != pyramid->generation;
        if (generationChanged) {
            outgoingRmsGeometry_ = rmsGeometry_;
            outgoingPeakGeometry_ = peakGeometry_;
            // Preserve the fully shaded material, not merely its fallback
            // geometry. GlyphFlow can then reconstruct gradient into gradient
            // without a flat-colour phase or a resolution snap at the end.
            outgoingFutureWaveform_ = futureWaveform_;
            outgoingPlayedWaveform_ = playedWaveform_;
            outgoingPlayedProgress_ = lastRenderedProgress_;
            waveformCalibrationGeneration_ = pyramid->generation;
            waveformCalibrationFrozen_ = false;
            spectralCalibrationPyramid_.reset();
        } else if (!waveformCalibrationFrozen_) {
            const auto calibrationMotion =
                motion_.sample(kWaveSeekReconstructMotion);
            if (calibrationMotion.found && !calibrationMotion.active &&
                calibrationMotion.progress >= 1.0) {
                // Once the initial reconstruction settles, already visible
                // columns become immutable. Later decoder publishes may add
                // columns, but cannot renormalize material the user has seen.
                waveformCalibrationFrozen_ = true;
            }
        }
        rmsGeometry_.Reset();
        peakGeometry_.Reset();
        waveformColumns_.clear();
        detailedWaveform_ = false;
        resetRetainedWaveform();
        geometryPyramid_.reset();
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        target->GetFactory(factory.GetAddressOf());
        if (!factory) return false;

        const auto center =
            (viewport.top + viewport.bottom) * 0.5f;
        const auto amplitude =
            std::max(1.0f, height * 0.47f);
        std::array<std::size_t, 128> bodyHistogram{};
        std::array<std::size_t, 128> truePeakHistogram{};
        for (const auto& bin : level->bins) {
            const auto displayReference = useDetail
                ? std::max(
                    detailU8(bin.detail.denseAmplitude),
                    detailU8(bin.detail.medianAmplitude))
                : bin.peak;
            const auto bodyBucket = static_cast<std::size_t>(std::clamp(
                std::floor(std::clamp(displayReference, 0.0f, 1.0f) *
                    static_cast<float>(bodyHistogram.size() - 1U)),
                0.0f,
                static_cast<float>(bodyHistogram.size() - 1U)));
            ++bodyHistogram[bodyBucket];
            const auto truePeak = useDetail
                ? std::max(
                    detailU16(bin.detail.peakLeft),
                    detailU16(bin.detail.peakRight))
                : bin.peak;
            const auto peakBucket = static_cast<std::size_t>(std::clamp(
                std::floor(std::clamp(truePeak, 0.0f, 1.0f) *
                    static_cast<float>(truePeakHistogram.size() - 1U)),
                0.0f,
                static_cast<float>(truePeakHistogram.size() - 1U)));
            ++truePeakHistogram[peakBucket];
        }
        const auto histogramPercentile = [count = level->bins.size()](
            const std::array<std::size_t, 128>& histogram,
            double percentile) noexcept {
            const auto rank = static_cast<std::size_t>(std::ceil(
                static_cast<double>(count) * percentile));
            std::size_t cumulative = 0;
            for (std::size_t bucket = 0;
                 bucket < histogram.size(); ++bucket) {
                cumulative += histogram[bucket];
                if (cumulative >= rank) {
                    return static_cast<float>(bucket + 1U) /
                        static_cast<float>(histogram.size());
                }
            }
            return 1.0f;
        };
        const auto bodyReference = std::max(
            useDetail ? 0.08f : 0.06f,
            histogramPercentile(
                bodyHistogram, useDetail ? 0.95 : 0.98));
        const auto peakReference = std::max(
            0.12f,
            histogramPercentile(truePeakHistogram, 0.98));
        const auto candidateBodyGain = useDetail
            ? std::clamp(0.82f / bodyReference, 0.80f, 2.8f)
            : std::clamp(0.92f / bodyReference, 1.0f, 4.0f);
        const auto candidatePeakGain = useDetail
            ? std::clamp(0.96f / peakReference, 0.80f, 1.9f)
            : std::min(candidateBodyGain, 2.8f);
        if (generationChanged ||
            waveformCalibrationGeneration_ != pyramid->generation) {
            waveformCalibrationGeneration_ = pyramid->generation;
            waveformBodyGain_ = candidateBodyGain;
            waveformPeakGain_ = candidatePeakGain;
        } else if (!waveformCalibrationFrozen_) {
            // Early snapshots refine calibration while GlyphFlow still masks
            // the construction. A conservative blend prevents pumping even
            // inside that short settling window.
            constexpr float kCalibrationBlend = 0.24f;
            waveformBodyGain_ +=
                (candidateBodyGain - waveformBodyGain_) *
                kCalibrationBlend;
            waveformPeakGain_ +=
                (candidatePeakGain - waveformPeakGain_) *
                kCalibrationBlend;
        }
        const auto bodyGain = waveformBodyGain_;
        const auto peakGain = waveformPeakGain_;
        const auto displayGain = bodyGain;
        const auto columns = std::max(
            1U,
            useDetail
                ? physicalColumns
                : static_cast<unsigned>(std::ceil(width)));
        std::vector<D2D1_POINT_2F> rmsTop;
        std::vector<D2D1_POINT_2F> rmsBottom;
        std::vector<D2D1_POINT_2F> peakTop;
        std::vector<D2D1_POINT_2F> peakBottom;
        rmsTop.reserve(columns);
        rmsBottom.reserve(columns);
        peakTop.reserve(columns);
        peakBottom.reserve(columns);

        for (unsigned column = 0; column < columns; ++column) {
            const auto normalizedStart =
                static_cast<double>(column) / columns;
            const auto normalizedEnd =
                static_cast<double>(column + 1U) / columns;
            const auto firstBinExact =
                normalizedStart * duration * 1000.0 /
                level->resolutionMs;
            const auto lastBinExact =
                normalizedEnd * duration * 1000.0 /
                level->resolutionMs;
            const auto firstBin = static_cast<std::size_t>(
                std::floor(firstBinExact));
            if (firstBin >= level->bins.size()) break;
            const auto lastBin = std::min(
                level->bins.size(),
                std::max(
                    firstBin + 1U,
                    static_cast<std::size_t>(std::ceil(lastBinExact))));

            float peak = 0.0f;
            double rmsSquares = 0.0;
            double leftSquares = 0.0;
            double rightSquares = 0.0;
            double medianSquares = 0.0;
            double denseSquares = 0.0;
            double lowSquares = 0.0;
            double midSquares = 0.0;
            double highSquares = 0.0;
            double transientSquares = 0.0;
            double totalWeight = 0.0;
            float trueLeftPeak = 0.0f;
            float trueRightPeak = 0.0f;
            float transientMaximum = 0.0f;
            std::array<double, 32> leftPeakHistogram{};
            std::array<double, 32> rightPeakHistogram{};
            for (auto index = firstBin; index < lastBin; ++index) {
                const auto overlap = std::max(
                    0.0,
                    std::min(lastBinExact,
                        static_cast<double>(index + 1U)) -
                    std::max(firstBinExact,
                        static_cast<double>(index)));
                if (overlap <= 0.0) continue;
                const auto& bin = level->bins[index];
                peak = std::max(peak, bin.peak);
                rmsSquares +=
                    static_cast<double>(bin.rms) * bin.rms * overlap;
                totalWeight += overlap;
                if (useDetail) {
                    const auto leftPeak = detailU16(bin.detail.peakLeft);
                    const auto rightPeak = detailU16(bin.detail.peakRight);
                    const auto leftRms = detailU16(bin.detail.rmsLeft);
                    const auto rightRms = detailU16(bin.detail.rmsRight);
                    const auto median = detailU8(
                        bin.detail.medianAmplitude);
                    const auto dense = detailU8(
                        bin.detail.denseAmplitude);
                    const auto low = detailU8(bin.detail.lowEnergy);
                    const auto mid = detailU8(bin.detail.midEnergy);
                    const auto high = detailU8(bin.detail.highEnergy);
                    const auto transient = detailU8(bin.detail.transient);
                    trueLeftPeak = std::max(trueLeftPeak, leftPeak);
                    trueRightPeak = std::max(trueRightPeak, rightPeak);
                    transientMaximum = std::max(
                        transientMaximum, transient);
                    leftSquares +=
                        static_cast<double>(leftRms) * leftRms * overlap;
                    rightSquares +=
                        static_cast<double>(rightRms) * rightRms * overlap;
                    medianSquares +=
                        static_cast<double>(median) * median * overlap;
                    denseSquares +=
                        static_cast<double>(dense) * dense * overlap;
                    lowSquares += static_cast<double>(low) * low * overlap;
                    midSquares += static_cast<double>(mid) * mid * overlap;
                    highSquares += static_cast<double>(high) * high * overlap;
                    transientSquares +=
                        static_cast<double>(transient) * transient * overlap;
                    leftPeakHistogram[static_cast<std::size_t>(std::clamp(
                        static_cast<int>(leftPeak * 31.0f), 0, 31))] += overlap;
                    rightPeakHistogram[static_cast<std::size_t>(std::clamp(
                        static_cast<int>(rightPeak * 31.0f), 0, 31))] += overlap;
                }
            }
            const auto effectiveWeight = std::max(0.000001, totalWeight);
            const auto rms = static_cast<float>(
                std::sqrt(rmsSquares / effectiveWeight));
            const auto rmsDisplay = std::pow(
                std::clamp(rms * displayGain, 0.0f, 1.0f),
                useDetail ? 0.78f : 0.68f);
            const auto peakDisplay = std::max(
                rmsDisplay,
                std::pow(
                    std::clamp(
                        peak * std::min(displayGain, 2.8f),
                        0.0f,
                        1.0f),
                    0.90f));
            const auto x = viewport.left +
                width * static_cast<float>(
                    (normalizedStart + normalizedEnd) * 0.5);
            if (useDetail) {
                const auto percentile = [effectiveWeight](
                    const std::array<double, 32>& histogram) noexcept {
                    const auto rank = effectiveWeight * 0.90;
                    double accumulated = 0.0;
                    for (std::size_t index = 0;
                         index < histogram.size(); ++index) {
                        accumulated += histogram[index];
                        if (accumulated >= rank) {
                            return static_cast<float>(index) /
                                static_cast<float>(histogram.size() - 1U);
                        }
                    }
                    return 0.0f;
                };
                const auto rootMeanSquare = [effectiveWeight](
                    double sum) noexcept {
                    return static_cast<float>(std::sqrt(
                        sum / effectiveWeight));
                };
                const auto shape = [](
                    float value, float gain, float gamma) noexcept {
                    return std::pow(
                        std::clamp(value * gain, 0.0f, 1.0f),
                        gamma);
                };
                const auto leftRms = rootMeanSquare(leftSquares);
                const auto rightRms = rootMeanSquare(rightSquares);
                const auto stereoReference = std::max(
                    0.0001f,
                    std::sqrt((leftRms * leftRms +
                        rightRms * rightRms) * 0.5f));
                const auto leftScale = std::clamp(
                    leftRms / stereoReference, 0.25f, 1.35f);
                const auto rightScale = std::clamp(
                    rightRms / stereoReference, 0.25f, 1.35f);
                const auto median = rootMeanSquare(medianSquares);
                const auto dense = rootMeanSquare(denseSquares);
                // Global body normalization is derived from the amplitude
                // distribution, so the drawn body must use the same domain.
                // Channel RMS alone is substantially lower and previously
                // left most of the lane visually empty.
                const auto bodyEnvelope =
                    median * 0.42f + dense * 0.58f;

                WaveformColumnV2 detail;
                detail.left = viewport.left +
                    width * static_cast<float>(normalizedStart);
                detail.right = viewport.left +
                    width * static_cast<float>(normalizedEnd);
                detail.leftBody = shape(
                    std::max(leftRms, bodyEnvelope * leftScale),
                    bodyGain,
                    0.76f);
                detail.rightBody = shape(
                    std::max(rightRms, bodyEnvelope * rightScale),
                    bodyGain,
                    0.76f);
                detail.leftCore = std::min(
                    detail.leftBody,
                    shape(median * leftScale, bodyGain, 0.90f));
                detail.rightCore = std::min(
                    detail.rightBody,
                    shape(median * rightScale, bodyGain, 0.90f));
                detail.leftShoulder = std::max(
                    detail.leftBody,
                    shape(dense * leftScale, bodyGain, 0.82f));
                detail.rightShoulder = std::max(
                    detail.rightBody,
                    shape(dense * rightScale, bodyGain, 0.82f));
                const auto attackBlend = smoothStep(
                    (transientMaximum - 0.15f) / 0.25f);
                const auto leftP90 = percentile(leftPeakHistogram);
                const auto rightP90 = percentile(rightPeakHistogram);
                detail.leftPeak = std::max(
                    detail.leftShoulder,
                    shape(
                        leftP90 + (trueLeftPeak - leftP90) * attackBlend,
                        peakGain,
                        1.0f));
                detail.rightPeak = std::max(
                    detail.rightShoulder,
                    shape(
                        rightP90 + (trueRightPeak - rightP90) * attackBlend,
                        peakGain,
                        1.0f));
                detail.low = rootMeanSquare(lowSquares);
                detail.mid = rootMeanSquare(midSquares);
                detail.high = rootMeanSquare(highSquares);
                detail.transient = std::max(
                    transientMaximum,
                    smoothStep(rootMeanSquare(transientSquares)));
                waveformColumns_.push_back(detail);

                rmsTop.push_back(D2D1::Point2F(
                    x, center - detail.leftShoulder * amplitude));
                rmsBottom.push_back(D2D1::Point2F(
                    x, center + detail.rightShoulder * amplitude));
                peakTop.push_back(D2D1::Point2F(
                    x, center - detail.leftPeak * amplitude));
                peakBottom.push_back(D2D1::Point2F(
                    x, center + detail.rightPeak * amplitude));
            } else {
                rmsTop.push_back(D2D1::Point2F(
                    x, center - rmsDisplay * amplitude));
                rmsBottom.push_back(D2D1::Point2F(
                    x, center + rmsDisplay * amplitude));
                peakTop.push_back(D2D1::Point2F(
                    x, center - peakDisplay * amplitude));
                peakBottom.push_back(D2D1::Point2F(
                    x, center + peakDisplay * amplitude));
            }
        }
        if (rmsTop.empty()) return false;
        if (useDetail) {
            smoothWaveformColumns();
            // Keep the transition geometry in the same smoothed shape as the
            // final density bitmap so there is no resolution/shape snap when
            // the retained layer takes over.
            for (std::size_t index = 0;
                 index < waveformColumns_.size() &&
                 index < rmsTop.size(); ++index) {
                const auto& column = waveformColumns_[index];
                rmsTop[index].y = center -
                    column.leftShoulder * amplitude;
                rmsBottom[index].y = center +
                    column.rightShoulder * amplitude;
                peakTop[index].y = center -
                    column.leftPeak * amplitude;
                peakBottom[index].y = center +
                    column.rightPeak * amplitude;
            }
        }

        if (FAILED(factory->CreatePathGeometry(
                rmsGeometry_.ReleaseAndGetAddressOf()))) {
            resetWaveformGeometry();
            return false;
        }
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> rmsSink;
        if (FAILED(rmsGeometry_->Open(
                rmsSink.ReleaseAndGetAddressOf()))) {
            resetWaveformGeometry();
            return false;
        }
        rmsSink->BeginFigure(
            rmsTop.front(), D2D1_FIGURE_BEGIN_FILLED);
        if (rmsTop.size() > 1) {
            rmsSink->AddLines(
                rmsTop.data() + 1,
                static_cast<UINT32>(rmsTop.size() - 1));
        }
        for (auto iterator = rmsBottom.rbegin();
             iterator != rmsBottom.rend();
             ++iterator) {
            rmsSink->AddLine(*iterator);
        }
        rmsSink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(rmsSink->Close())) {
            resetWaveformGeometry();
            return false;
        }

        if (SUCCEEDED(factory->CreatePathGeometry(
                peakGeometry_.ReleaseAndGetAddressOf()))) {
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> peakSink;
            if (SUCCEEDED(peakGeometry_->Open(
                    peakSink.ReleaseAndGetAddressOf()))) {
                peakSink->BeginFigure(
                    peakTop.front(),
                    D2D1_FIGURE_BEGIN_FILLED);
                if (peakTop.size() > 1) {
                    peakSink->AddLines(
                        peakTop.data() + 1,
                        static_cast<UINT32>(peakTop.size() - 1));
                }
                for (auto iterator = peakBottom.rbegin();
                     iterator != peakBottom.rend();
                     ++iterator) {
                    peakSink->AddLine(*iterator);
                }
                peakSink->EndFigure(D2D1_FIGURE_END_CLOSED);
                if (FAILED(peakSink->Close())) {
                    peakGeometry_.Reset();
                }
            } else {
                peakGeometry_.Reset();
            }
        }

        const auto nextCoverage = incomingCoverage;
        geometryPyramid_ = pyramid;
        geometryGeneration_ = pyramid->generation;
        geometryCoverage_ = generationChanged
            ? nextCoverage
            : std::max(geometryCoverage_, nextCoverage);
        geometryComplete_ = pyramid->complete();
        geometryDuration_ = duration;
        geometryWidth_ = width;
        geometryHeight_ = height;
        geometryLeft_ = viewport.left;
        geometryTop_ = viewport.top;
        geometryColumnCount_ = columns;
        detailedWaveform_ = useDetail && !waveformColumns_.empty();
        detailCenter_ = center;
        detailAmplitude_ = amplitude;
        if (generationChanged) {
            motion_.begin(
                kWaveSeekReconstructMotion,
                MotionToken::artwork,
                motionMode(),
                0.0,
                1.0,
                MotionEasing::easeInOutCubic);
            motion_.begin(
                kWaveSeekCoverageMotion,
                MotionToken::selection,
                motionMode(),
                0.0,
                geometryCoverage_,
                MotionEasing::easeOutCubic);
            if (canPresent(window_)) frameClock_.start();
        } else {
            motion_.retarget(
                kWaveSeekCoverageMotion,
                MotionToken::selection,
                motionMode(),
                geometryCoverage_,
                MotionEasing::easeOutCubic);
        }
        return true;
    }

    void resetWaveformGeometry() noexcept
    {
        rmsGeometry_.Reset();
        peakGeometry_.Reset();
        outgoingRmsGeometry_.Reset();
        outgoingPeakGeometry_.Reset();
        outgoingFutureWaveform_.Reset();
        outgoingPlayedWaveform_.Reset();
        resetRetainedWaveform();
        waveformColumns_.clear();
        detailedWaveform_ = false;
        detailCenter_ = 0.0f;
        detailAmplitude_ = 0.0f;
        geometryPyramid_.reset();
        geometryGeneration_ = 0;
        geometryCoverage_ = 0.0f;
        geometryComplete_ = false;
        geometryDuration_ = 0.0;
        geometryWidth_ = 0.0f;
        geometryHeight_ = 0.0f;
        geometryLeft_ = 0.0f;
        geometryTop_ = 0.0f;
        geometryColumnCount_ = 0;
        waveformCalibrationGeneration_ = 0;
        waveformCalibrationFrozen_ = false;
        waveformBodyGain_ = 1.0f;
        waveformPeakGain_ = 1.0f;
        waveformLowReference_ = 0.008f;
        waveformMidReference_ = 0.008f;
        waveformHighReference_ = 0.008f;
        spectralCalibrationPyramid_.reset();
        lastRenderedProgress_ = 0.0f;
        outgoingPlayedProgress_ = 0.0f;
        motion_.release(kWaveSeekReconstructMotion);
        motion_.release(kWaveSeekCoverageMotion);
    }

    void refreshTrack()
    {
        const auto next = Runtime::instance().currentTrackHandle();
        if (next == track_) return;
        track_ = next;
        requestGeneration_ = 0;
        requestStartedSeconds_ = qpcSeconds();
        firstVisualLatencyUs_ = 0;
        playheadPresentation_.reset();
        if (track_.is_empty()) {
            cache_.cancel();
            resetWaveformGeometry();
            return;
        }

        const auto target = window_;
        requestGeneration_ = cache_.request(
            track_,
            [target](WaveformCacheService::SnapshotPtr snapshot) {
                if (target == nullptr || !IsWindow(target) ||
                    !snapshot) {
                    return;
                }
                PostMessageW(
                    target,
                    kWaveSeekSnapshotMessage,
                    static_cast<WPARAM>(snapshot->generation),
                    0);
            });
    }

    void pointerDown(LPARAM lParam)
    {
        if (!playback_control::get()->playback_can_seek()) return;
        const auto [x, y] = pointDip(window_, lParam);
        const auto rectangle = timelineRect();
        if (!pointIn(rectangle, x, y)) return;
        const auto normalized = normalizedPosition(x);
        const auto paused = playback_control::get()->is_paused();
        const auto alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        const auto shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        // Playing drag auditions by default; Shift explicitly requests the
        // silent precision gesture.  A paused transport stays silent unless
        // Alt is held, so scrubbing never resumes it by surprise.
        const auto audition = paused ? alt : !shift;
        const auto session = ScrubCoordinator::instance().begin(normalized, audition);
        if (session == 0) return;
        scrubSession_ = session;
        pointerCaptured_ = true;
        dragging_ = false;
        pointerDownX_ = x;
        SetCapture(window_);
        requestPointerFrame();
    }

    void pointerMove(LPARAM lParam)
    {
        if (!pointerCaptured_) return;
        const auto [x, ignored] = pointDip(window_, lParam);
        if (!dragging_ && std::abs(x - pointerDownX_) >= 3.0f) {
            dragging_ = true;
        }
        if (dragging_) {
            ScrubCoordinator::instance().update(scrubSession_, normalizedPosition(x));
        }
        requestPointerFrame();
    }

    void pointerUp(LPARAM lParam)
    {
        if (!pointerCaptured_) return;
        const auto [x, ignored] = pointDip(window_, lParam);
        ScrubCoordinator::instance().end(scrubSession_, normalizedPosition(x));
        pointerCaptured_ = false;
        dragging_ = false;
        if (GetCapture() == window_) ReleaseCapture();
        requestPointerFrame();
    }

    void cancelPointer(bool cancelPending = false)
    {
        if (!pointerCaptured_ && (!cancelPending || scrubSession_ == 0)) return;
        ScrubCoordinator::instance().cancel(scrubSession_);
        scrubSession_ = 0;
        pointerCaptured_ = false;
        dragging_ = false;
        if (window_ != nullptr && GetCapture() == window_) {
            ReleaseCapture();
        }
        requestPointerFrame();
    }

    void commitPointer()
    {
        if (!pointerCaptured_) return;
        ScrubCoordinator::instance().commitCurrent(scrubSession_);
        pointerCaptured_ = false;
        dragging_ = false;
        if (window_ != nullptr && GetCapture() == window_) ReleaseCapture();
        syncClock();
        InvalidateRect(window_, nullptr, FALSE);
    }

    double normalizedPosition(float x) const noexcept
    {
        const auto rectangle = timelineRect();
        const auto width =
            std::max(1.0f, rectangle.right - rectangle.left);
        return std::clamp(
            static_cast<double>((x - rectangle.left) / width),
            0.0,
            1.0);
    }

    void updateHover(LPARAM lParam)
    {
        if (!trackingMouse_) {
            TRACKMOUSEEVENT tracking{
                sizeof(tracking),
                TME_LEAVE,
                window_,
                HOVER_DEFAULT,
            };
            TrackMouseEvent(&tracking);
            trackingMouse_ = true;
        }
        const auto [x, y] = pointDip(window_, lParam);
        const auto rectangle = timelineRect();
        const auto next = pointIn(rectangle, x, y)
            ? std::optional<float>{x}
            : std::nullopt;
        if (next == hover_) return;
        hover_ = next;
        requestPointerFrame();
    }

    void requestPointerFrame()
    {
        // Input only replaces the latest intent. Drawing here on every mouse
        // packet can outrun DXGI and block the input handler in EndDraw. Keep
        // the pending intent until a readiness-approved frame is accepted,
        // including the final move after the pointer stops or leaves.
        pointerFramePending_ = true;
        syncClock();
        frameClock_.requestImmediate();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void syncClock()
    {
        // A track switch or another surface can revoke this generation while
        // Windows still reports pointer capture. Reconcile even when hidden.
        if (scrubSession_ != 0 && !ScrubCoordinator::instance().owns(scrubSession_)) {
            scrubSession_ = 0;
            pointerCaptured_ = false;
            dragging_ = false;
            if (window_ != nullptr && GetCapture() == window_) ReleaseCapture();
        }
        if (!canPresent(window_)) {
            cancelPointer(true);
            preparationRetryQpc_ = 0;
            frameClock_.stop();
            return;
        }
        if (preparationRetryQpc_ != 0 && waveSeekQpcNow() < preparationRetryQpc_) {
            if (frameClock_.logicalNotBeforeQpc() != preparationRetryQpc_) frameClock_.stop();
            frameClock_.requestAtOrAfter(preparationRetryQpc_, kFrameReasonAmbient);
            return;
        }
        const auto control = playback_control::get();
        const auto semanticMotion =
            geometryFramePending_ ||
            contentFramePending_ ||
            pointerFramePending_ ||
            pointerCaptured_ ||
            ScrubCoordinator::instance().active() ||
            (geometryGeneration_ != 0 && !geometryComplete_) ||
            motion_.needsFrame();
        const auto themeActive = ThemeService::instance().transitionActive();
        frameClock_.setMaximumRefreshHz(
            semanticMotion
                ? 0.0
                : themeActive ? kThemeSampleHz : playbackCursorRefreshHz(
                    frameClock_.targetRefreshMilliHz()));
        const auto active =
            semanticMotion ||
            themeActive ||
            (control->is_playing() && !control->is_paused());
        if (active) frameClock_.start();
        else frameClock_.stop();
    }

    std::uint64_t nextCursorDeadline() const noexcept
    {
        LARGE_INTEGER now{}, frequency{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        // Skipped cursor frames retain a DXGI credit without presenting, so
        // they do not pass through completePresentationOpportunity's cap.
        // Recheck at the cursor cadence; intervalSeconds is the native display
        // period and would wake an unchanged seek at 165 Hz between pixels.
        const auto cursorMilliHz = static_cast<std::uint32_t>(std::lround(
            playbackCursorRefreshHz(frameClock_.targetRefreshMilliHz()) * 1000.0));
        return cappedPresentationDeadline(
            static_cast<std::uint64_t>(now.QuadPart),
            static_cast<std::uint64_t>(frequency.QuadPart), cursorMilliHz);
    }

    static constexpr std::int64_t kUnpresentedStep =
        (std::numeric_limits<std::int64_t>::min)();

    std::int64_t playheadPresentationStep(
        double position,
        double duration) const noexcept
    {
        if (duration <= 0.0 || window_ == nullptr) return kUnpresentedStep;
        const auto viewport = waveformRect();
        const auto scale = static_cast<double>(GetDpiForWindow(window_)) / 96.0;
        const auto pixelWidth = std::max<std::int64_t>(
            1,
            static_cast<std::int64_t>(std::llround(
                static_cast<double>(viewport.right - viewport.left) * scale)));
        const auto progress = std::clamp(position / duration, 0.0, 1.0);
        return std::min<std::int64_t>(
            pixelWidth - 1,
            static_cast<std::int64_t>(std::floor(
                progress * static_cast<double>(pixelWidth))));
    }

    bool shouldPresentFrame()
    {
        if (geometryFramePending_ || contentFramePending_ || pointerFramePending_ || pointerCaptured_ || ScrubCoordinator::instance().active() ||
            ThemeService::instance().transitionActive() ||
            (geometryGeneration_ != 0 && !geometryComplete_) ||
            motion_.needsFrame()) {
            return true;
        }
        const auto playback = Runtime::instance().playbackSnapshot();
        if (playback.state != PlaybackState::playing) return false;
        return playheadPresentation_.shouldPresent(
            playheadPresentationStep(
                ScrubCoordinator::instance().effectivePosition(
                    Runtime::instance().presentationPlaybackPosition()),
                playback.durationSeconds),
            qpcSeconds(),
            frameClock_.targetRefreshMilliHz());
    }

    HWND window_ = nullptr;
    PanelD2D render_;
    WindowFrameClock frameClock_;
    MotionCoordinator motion_;
    GlyphFlowRenderer waveGlyphRenderer_;
    bool qaRejectNextDraw_ = false;
    HRESULT qaRejectedDrawHr_ = S_OK;
    unsigned qaRejectedDraws_ = 0;
    WaveformCacheService cache_;
    std::shared_ptr<WaveformPersistenceHook> persistence_;
    metadb_handle_ptr track_;
    std::uint64_t requestGeneration_ = 0;
    std::optional<float> hover_;
    float pointerDownX_ = 0.0f;
    bool pointerCaptured_ = false;
    ScrubCoordinator::SessionToken scrubSession_ = 0;
    bool dragging_ = false;
    bool trackingMouse_ = false;
    bool pointerFramePending_ = true;
    std::uint64_t preparationRetryQpc_ = 0;
    bool geometryFramePending_ = true;
    bool contentFramePending_ = true;
    bool clockPaintSkipped_ = false;
    std::optional<float> presentedHover_;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> rmsGeometry_;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> peakGeometry_;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> outgoingRmsGeometry_;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> outgoingPeakGeometry_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> futureWaveform_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> playedWaveform_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> outgoingFutureWaveform_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> outgoingPlayedWaveform_;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> retainedTarget_;
    std::array<Microsoft::WRL::ComPtr<ID2D1PathGeometry>, 6> costGeometry_;
    std::shared_ptr<const PacketCostResult> costGeometryResult_;
    float costGeometryWidth_ = 0.0f;
    float costGeometryTop_ = 0.0f;
    float costGeometryBottom_ = 0.0f;
    std::uint32_t costGeometryResolution_ = 0;
    std::array<D2D1_COLOR_F, 3> retainedColors_{};
    D2D1_SIZE_F retainedSize_{};
    bool retainedColorsValid_ = false;
    float lastRenderedProgress_ = 0.0f;
    float outgoingPlayedProgress_ = 0.0f;
    std::vector<WaveformColumnV2> waveformColumns_;
    bool detailedWaveform_ = false;
    float detailCenter_ = 0.0f;
    float detailAmplitude_ = 0.0f;
    std::shared_ptr<const WaveformPyramidV1> geometryPyramid_;
    std::uint64_t geometryGeneration_ = 0;
    float geometryCoverage_ = 0.0f;
    bool geometryComplete_ = false;
    double geometryDuration_ = 0.0;
    float geometryWidth_ = 0.0f;
    float geometryHeight_ = 0.0f;
    float geometryLeft_ = 0.0f;
    float geometryTop_ = 0.0f;
    unsigned geometryColumnCount_ = 0;
    std::uint64_t waveformCalibrationGeneration_ = 0;
    bool waveformCalibrationFrozen_ = false;
    float waveformBodyGain_ = 1.0f;
    float waveformPeakGain_ = 1.0f;
    float waveformLowReference_ = 0.008f;
    float waveformMidReference_ = 0.008f;
    float waveformHighReference_ = 0.008f;
    std::shared_ptr<const WaveformPyramidV1>
        spectralCalibrationPyramid_;
    double requestStartedSeconds_ = 0.0;
    std::uint64_t firstVisualLatencyUs_ = 0;
    nekorox::PlaybackPresentationGate playheadPresentation_;
    std::uint64_t playbackPaints_ = 0;
    std::uint64_t observedSkippedDeadlines_ = 0;
};

uie::window_factory<WaveSeekPanel> g_waveSeekPanelFactory;

} // namespace

void notifyWaveSeekPlaybackChanged()
{
    UiInvalidationBus::instance().publish(
        UiInvalidationSurface::waveSeek,
        UiDirty::semantic);
}

} // namespace nekorox::component
