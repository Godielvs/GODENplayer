#include "album_mode_renderer.h"
#include "performance_telemetry.h"
#include "telemetry_span.h"
#include "curated/curated_preferences.h"
#include "a4/a4_preferences.h"
#include "a4/a4_trace.h"

#include "motion_coordinator.h"
#include "rollout_flags.h"
#include "text_util.h"
#include "theme_service.h"
#include "ui_preferences.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <d2d1effects.h>
#include <initializer_list>
#include <sstream>
#include <vector>

namespace nekorox::component {
namespace {

float width(const D2D1_RECT_F& value) noexcept { return value.right - value.left; }
float height(const D2D1_RECT_F& value) noexcept { return value.bottom - value.top; }

D2D1_RECT_F containRect(const D2D1_RECT_F& bounds,
    std::uint32_t imageWidth, std::uint32_t imageHeight) noexcept
{
    if (imageWidth == 0 || imageHeight == 0) return bounds;
    const auto scale = std::min(width(bounds) / imageWidth,
        height(bounds) / imageHeight);
    const auto w = imageWidth * scale;
    const auto h = imageHeight * scale;
    const auto x = (bounds.left + bounds.right - w) * 0.5f;
    const auto y = (bounds.top + bounds.bottom - h) * 0.5f;
    return D2D1::RectF(x, y, x + w, y + h);
}

D2D1_RECT_F interpolateRect(const D2D1_RECT_F& from,
    const D2D1_RECT_F& to, float amount) noexcept
{
    amount = std::clamp(amount, 0.0f, 1.0f);
    return D2D1::RectF(
        from.left + (to.left - from.left) * amount,
        from.top + (to.top - from.top) * amount,
        from.right + (to.right - from.right) * amount,
        from.bottom + (to.bottom - from.bottom) * amount);
}

std::uint64_t semanticRevision(
    std::initializer_list<std::uint64_t> values) noexcept
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto value : values) {
        auto remaining = value;
        for (unsigned byte = 0; byte < 8; ++byte) {
            hash ^= remaining & 0xffU;
            hash *= 1099511628211ULL;
            remaining >>= 8U;
        }
    }
    return hash;
}

std::uint64_t hashText(std::uint64_t hash, std::string_view text) noexcept
{
    for (const auto value : text) {
        hash ^= static_cast<unsigned char>(value);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint64_t lyricsContentRevision(
    const std::shared_ptr<const LyricsSnapshot>& snapshot) noexcept
{
    if (!snapshot || !snapshot->document) return 0;
    const auto& document = *snapshot->document;
    auto hash = 1469598103934665603ULL;
    hash = hashText(hash, document.documentId);
    hash = hashText(hash, document.textHash);
    hash = hashText(hash, document.timingHash);
    hash ^= static_cast<std::uint64_t>(document.timing);
    hash *= 1099511628211ULL;
    return hash;
}

double rendererClockSeconds() noexcept
{
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

float quinticSmooth(float value) noexcept
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * value *
        (value * (value * 6.0f - 15.0f) + 10.0f);
}

D2D1_RECT_F statusCard(const D2D1_RECT_F& column) noexcept
{
    const auto cardWidth = std::min(360.0f, std::max(0.0f, width(column) - 24.0f));
    const auto cardHeight = std::min(112.0f, std::max(0.0f, height(column) - 24.0f));
    const auto x = (column.left + column.right - cardWidth) * 0.5f;
    const auto y = (column.top + column.bottom - cardHeight) * 0.5f;
    return D2D1::RectF(x, y, x + cardWidth, y + cardHeight);
}

D2D1_RECT_F occupiedTrackRect(const D2D1_RECT_F& column,
    std::size_t trackCount) noexcept
{
    auto result = column;
    result.bottom = std::min(column.bottom,
        column.top + 38.0f + static_cast<float>(trackCount) * 28.0f + 8.0f);
    return result;
}

D2D1_RECT_F occupiedLyricsRect(const D2D1_RECT_F& column,
    std::size_t lineCount) noexcept
{
    auto result = column;
    result.bottom = std::min(column.bottom,
        column.top + 16.0f + static_cast<float>(lineCount) * 50.0f);
    return result;
}

} // namespace


bool AlbumModeRenderer::curatedSelected() const noexcept
{
    return curated::selectedStyle(static_cast<unsigned>(a4::style()));
}
void AlbumModeRenderer::setCuratedActivity(bool active) noexcept
{
    curatedActive_=active;
    curated_.setActivity(active,rendererClockSeconds());
}
void AlbumModeRenderer::suspendCurated() noexcept
{
    curatedActive_=false;
    curated_.suspend(rendererClockSeconds());
}
bool AlbumModeRenderer::curatedAnimationActive() const noexcept
{
    return curatedSelected() && curated_.needsFrame();
}
bool AlbumModeRenderer::curatedFrameDue() const noexcept
{
    return curatedSelected() && curated_.due(rendererClockSeconds());
}
std::uint64_t AlbumModeRenderer::curatedDeadlineQpc() const noexcept
{
    LARGE_INTEGER ticks{},frequency{};
    if(!QueryPerformanceCounter(&ticks)||!QueryPerformanceFrequency(&frequency)||frequency.QuadPart<=0)return 0;
    const auto delay=std::max(0.0,curated_.deadline()-rendererClockSeconds());
    return static_cast<std::uint64_t>(ticks.QuadPart)+
        static_cast<std::uint64_t>(delay*static_cast<double>(frequency.QuadPart));
}

void AlbumModeRenderer::ensureFormats()
{
    if (!writeFactory_) {
        (void)DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(writeFactory_.ReleaseAndGetAddressOf()));
    }
    if (!writeFactory_ || lyricActive_) return;
    (void)writeFactory_->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 22.0f, L"",
        lyricActive_.ReleaseAndGetAddressOf());
    (void)writeFactory_->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 22.0f, L"",
        lyricNeighbour_.ReleaseAndGetAddressOf());
    (void)writeFactory_->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 20.0f, L"",
        lyricPlain_.ReleaseAndGetAddressOf());
    (void)writeFactory_->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 20.0f, L"",
        bookletTitle_.ReleaseAndGetAddressOf());
    (void)writeFactory_->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"",
        bookletBody_.ReleaseAndGetAddressOf());
    for (auto* format : {lyricActive_.Get(), lyricNeighbour_.Get(), lyricPlain_.Get()}) {
        if (format) {
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    }
    for (auto* format : {bookletTitle_.Get(), bookletBody_.Get()}) {
        if (format) {
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
    }
}

void AlbumModeRenderer::onResourceEpochChanged(std::uint64_t epoch) noexcept
{
    if(epoch==0||epoch==observedResourceEpoch_)return;
    observedResourceEpoch_=epoch;
    initialUiPresented_=false;
    curated_.invalidateBitmap();
    ambientEffect_.Reset();ambientTexture_.Reset();previousAmbientTexture_.Reset();
    resetAmbientGpuTiming();alloy_.reset();alloySceneReady_=false;
    bookletBackdrop_.Reset();bookletBackdropBlur_.Reset();bookletOpen_=false;
    artworkTransition_.discardDeviceResources();artworkTarget_=0;lyricBrush_.Reset();
    a4::trace("graphics.epoch",epoch);
}

void AlbumModeRenderer::ensureArtwork(
    const std::shared_ptr<const ArtPixels>& pixels,
    std::wstring_view identity,float dpi,ArtCacheStatus status)
{
    const auto now=rendererClockSeconds();
    if(artworkRequestIdentity_!=identity){
        artworkRequestIdentity_.assign(identity);++artworkRequestSerial_;
    }
    const bool usable=pixels&&!pixels->bgra.empty()&&pixels->width&&pixels->height&&!identity.empty();
    const auto gateState=usable?a4::ArtworkState::ready:
        status==ArtCacheStatus::missing?a4::ArtworkState::missing:a4::ArtworkState::loading;
    if(!usable)artworkGate_.observe(artworkRequestSerial_,gateState,displayedArtwork_!=nullptr,
        now,motionMode()==MotionMode::normal);
    artworkOpacity_=artworkGate_.opacity(now);
    if(!usable){
        if(artworkGate_.shouldClear(now)||identity.empty()){
            artworkGate_.cleared();displayedArtwork_.reset();artworkTransition_.clear();
            artworkHash_=0;artworkWidth_=artworkHeight_=0;artworkTarget_=0;
            artworkIdentity_.clear();ambientPalette_={};ambientTexture_.Reset();
            previousAmbientTexture_.Reset();previousArtworkHash_=0;
            ++artworkVisualRevision_;artworkOpacity_=0;
        }
        return;
    }
    auto* target=render_.target();if(!target)return;
    artworkTransition_.update(target,dpi,identity,pixels);
    if(artworkTransition_.currentContentHash()!=pixels->contentHash||!artworkTransition_.currentBitmap()){
        artworkGate_.observe(artworkRequestSerial_,a4::ArtworkState::error,displayedArtwork_!=nullptr,now,motionMode()==MotionMode::normal);
        artworkOpacity_=artworkGate_.opacity(now);
        if(artworkGate_.shouldClear(now)){
            artworkGate_.cleared();displayedArtwork_.reset();artworkTransition_.clear();
            artworkHash_=0;artworkWidth_=artworkHeight_=0;artworkTarget_=0;
            artworkIdentity_.clear();ambientPalette_={};ambientTexture_.Reset();
            previousAmbientTexture_.Reset();previousArtworkHash_=0;++artworkVisualRevision_;
        }
        return;
    }
    artworkGate_.observe(artworkRequestSerial_,a4::ArtworkState::ready,displayedArtwork_!=nullptr,
        now,motionMode()==MotionMode::normal);
    const bool sameMaterial=artworkHash_==pixels->contentHash&&artworkHash_!=0;
    const auto targetIdentity=reinterpret_cast<std::uintptr_t>(target);
    const bool rasterChanged=artworkWidth_!=pixels->width||artworkHeight_!=pixels->height||artworkTarget_!=targetIdentity;
    displayedArtwork_=pixels;artworkIdentity_.assign(identity);artworkOpacity_=1;
    if(sameMaterial&&!rasterChanged)return;
    if(!sameMaterial){
        previousArtworkHash_=artworkHash_;previousAmbientTexture_=ambientTexture_;
        previousAmbientPalette_=ambientPalette_;ambientPalette_=pixels->palette;
        artworkChangedAt_=std::chrono::steady_clock::now();
    }
    // Same-content DPI variants never start a new material transition.
    ambientTexture_.Reset();artworkHash_=pixels->contentHash;
    artworkWidth_=pixels->width;artworkHeight_=pixels->height;artworkTarget_=targetIdentity;
    ++artworkVisualRevision_;
}

void AlbumModeRenderer::ensureLyrics(
    const std::shared_ptr<const LyricsSnapshot>& snapshot)
{
    const auto presentation = snapshot ? snapshot->presentationRevision : 0U;
    if (presentation == lyricsPresentationRevision_) return;
    lyricsPresentationRevision_ = presentation;
    const auto revision = lyricsContentRevision(snapshot);
    const auto recording = snapshot ? snapshot->request.recording : RecordingKey{};
    const bool recordingChanged = !(lyricsRecording_ == recording);
    if (revision == lyricsRevision_ && !recordingChanged) return;
    lyricsRecording_ = recording;
    ++plainLyricsOwner_;
    plainLyricsText_.cancel();plainLyricsFrame_.reset();plainLyricsBatch_.reset();pendingPlainWheel_=0.f;
    lyricsRevision_ = revision;
    lyrics_ = snapshot ? snapshot->document : nullptr;
    lyricLines_.clear();
    preparedLyrics_.clear();
    lyricVisuals_.clear();
    preparedLyricsRevision_ = 0;
    preparedLyricsWidth_ = 0.0f;
    preparedLyricsHeight_ = 0.0f;
    lyricsStaging_=false;
    ++lyricsGestureRevision_;
    if (lyrics_ && (lyrics_->timing == TimingStatus::catalogMatched ||
            lyrics_->timing == TimingStatus::audioVerified) &&
        !lyrics_->cues.empty()) {
        lyricLines_.reserve(lyrics_->cues.size());
        for (const auto& cue : lyrics_->cues) {
            std::wstring line;
            for (const auto& simultaneous : cue.simultaneousLines) {
                if (!line.empty()) line += L"\n";
                line += utf8ToWide(simultaneous);
            }
            lyricLines_.push_back(std::move(line));
        }
    } else if (lyrics_ && !lyrics_->plainTextUtf8.empty()) {
        std::istringstream lines(lyrics_->plainTextUtf8);
        std::string line;
        while (std::getline(lines, line)) lyricLines_.push_back(utf8ToWide(line));
    }
    lyricScrollDip_ = 0.0f;
    lyricMaximumScrollDip_ = 0.0f;
    lyricScroll_.setBounds(0.0, 0.0);
    lyricScroll_.setPosition(0.0);
    pendingCueIndex_.reset();
    pendingCueStarted_ = {};
    manualLyricsUntil_ = {};
}

void AlbumModeRenderer::ensureLyricsLayout(float textWidth)
{
    TelemetrySpan span(TelemetrySurface::album,107,lyricLines_.size());
    textWidth = std::max(1.0f, textWidth);
    if (preparedLyricsRevision_ != lyricsRevision_ ||
        std::abs(preparedLyricsWidth_ - textWidth) >= 0.25f) {
        preparedLyrics_.clear();
        preparedLyrics_.reserve(lyricLines_.size());
        preparedLyricsHeight_=10.f;
        preparedLyricsRevision_=lyricsRevision_;
        preparedLyricsWidth_=textWidth;
        lyricVisuals_.clear();
        ++lyricsGestureRevision_;
    }
    const auto began=std::chrono::steady_clock::now();
    const auto end=std::min(lyricLines_.size(),preparedLyrics_.size()+32U);
    const auto synced = lyrics_ &&
        (lyrics_->timing == TimingStatus::catalogMatched ||
         lyrics_->timing == TimingStatus::audioVerified);
    for (std::size_t index = preparedLyrics_.size(); index < end; ++index) {
        PreparedLyricsLine line;
        line.top = preparedLyricsHeight_;
        line.normal = textLayouts_.getOrCreate(writeFactory_.Get(),
            lyricsRevision_, index * 2U, synced ? 2U : 3U,
            lyricLines_[index],
            synced ? lyricNeighbour_.Get() : lyricPlain_.Get(), textWidth);
        const auto normalHeight = line.normal ? line.normal->height : 24.0f;
        line.height = normalHeight * 1.08f;
        preparedLyricsHeight_ += line.height + 12.0f;
        preparedLyrics_.push_back(std::move(line));
        if(std::chrono::steady_clock::now()-began>=std::chrono::milliseconds(2)) break;
    }
    lyricVisuals_.resize(preparedLyrics_.size());
    lyricsStaging_=preparedLyrics_.size()<lyricLines_.size();
}

bool AlbumModeRenderer::prepareAmbientEffect(
    Microsoft::WRL::ComPtr<ID2D1Effect>& effect)
{
    if (qaAlbumAmbientEffectFailure()) return false;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
    if (FAILED(render_.target()->QueryInterface(
            IID_PPV_ARGS(context.ReleaseAndGetAddressOf())))) return false;
    if (FAILED(AlbumAmbientEffect::create(
            context.Get(), effect.ReleaseAndGetAddressOf()))) {
        effect.Reset();
        return false;
    }
    return true;
}

AlbumModeRenderer::AmbientBuildResult AlbumModeRenderer::buildAmbientTexture(
    ID2D1Effect* effect, const ArtPalette& palette,
    const AlbumAmbientPose& pose,
    const D2D1_RECT_F& lyricsRect, const D2D1_RECT_F& tracksRect,
    std::uint64_t artworkHash,
    const D2D1_RECT_F& target,
    Microsoft::WRL::ComPtr<ID2D1Bitmap1>& output)
{
    if (target.right <= target.left ||
        target.bottom <= target.top ||
        ambientBudgetFallback_ || qaAlbumAmbientEffectFailure()) {
        return AmbientBuildResult::notRequested;
    }
    if (!effect) return AmbientBuildResult::effectUnavailable;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
    if (FAILED(render_.target()->QueryInterface(
        IID_PPV_ARGS(context.ReleaseAndGetAddressOf())))) {
        return AmbientBuildResult::effectUnavailable;
    }
    const auto wanted = fitAmbientTexture(
        static_cast<std::uint32_t>(std::ceil(width(target))),
        static_cast<std::uint32_t>(std::ceil(height(target))),
        ambientDegraded_);
    if (wanted.width == 0 || wanted.height == 0)
        return AmbientBuildResult::drawFailed;
    if (!output || output->GetPixelSize().width != wanted.width ||
        output->GetPixelSize().height != wanted.height) {
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT,
                D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> replacement;
        if (FAILED(context->CreateBitmap(D2D1::SizeU(wanted.width, wanted.height),
                nullptr, 0, &properties,
                replacement.ReleaseAndGetAddressOf()))) {
            return AmbientBuildResult::drawFailed;
        }
        output = std::move(replacement);
    }

    const auto targetWidth = std::max(1.0f, width(target));
    const auto targetHeight = std::max(1.0f, height(target));
    const auto normalize = [&](const D2D1_RECT_F& rectangle) {
        return D2D1::RectF(
            (rectangle.left - target.left) / targetWidth,
            (rectangle.top - target.top) / targetHeight,
            (rectangle.right - target.left) / targetWidth,
            (rectangle.bottom - target.top) / targetHeight);
    };
    const auto lyricsNormalized = normalize(lyricsRect);
    const auto tracksNormalized = normalize(tracksRect);

    const auto viewportSize = D2D1::SizeF(targetWidth, targetHeight);
    const auto constants = AlbumAmbientEffect::constants(viewportSize,
        D2D1::SizeU(wanted.width, wanted.height),
        lyricsNormalized, tracksNormalized, palette, pose,
        albumAmbientMode() == AlbumAmbientMode::reactive, artworkHash);
    if (FAILED(AlbumAmbientEffect::update(effect, constants))) {
        return AmbientBuildResult::constantsRejected;
    }

    Microsoft::WRL::ComPtr<ID2D1Image> previousTarget;
    context->GetTarget(previousTarget.ReleaseAndGetAddressOf());
    float previousDpiX = 96.0f;
    float previousDpiY = 96.0f;
    context->GetDpi(&previousDpiX, &previousDpiY);
    context->SetTarget(output.Get());
    context->SetDpi(96.0f, 96.0f);
    context->BeginDraw();
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    const auto timingSlot = beginAmbientGpuTiming();
    context->DrawImage(effect, D2D1::Point2F(0.0f, 0.0f));
    const auto result = context->EndDraw();
    endAmbientGpuTiming(timingSlot);
    context->SetTarget(previousTarget.Get());
    context->SetDpi(previousDpiX, previousDpiY);
    if (FAILED(result)) {
        output.Reset();
        return AmbientBuildResult::drawFailed;
    }
    return AmbientBuildResult::produced;
}

void AlbumModeRenderer::collectAmbientGpuTiming() noexcept
{
    auto* context = render_.immediateContext();
    if (!context) return;
    for (auto& slot : ambientTiming_) {
        if (!slot.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        UINT64 begin = 0;
        UINT64 end = 0;
        const auto disjointResult = context->GetData(slot.disjoint.Get(),
            &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        const auto beginResult = context->GetData(slot.begin.Get(), &begin,
            sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        const auto endResult = context->GetData(slot.end.Get(), &end,
            sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (disjointResult == S_FALSE || beginResult == S_FALSE ||
            endResult == S_FALSE) continue;
        slot.pending = false;
        if (FAILED(disjointResult) || FAILED(beginResult) || FAILED(endResult) ||
            disjoint.Disjoint || disjoint.Frequency == 0 || end < begin) continue;
        const auto microseconds = static_cast<double>(end - begin) * 1'000'000.0 /
            static_cast<double>(disjoint.Frequency);
        ambientGpuLatestUs_ = microseconds;
        ambientGpuSamplesUs_[ambientGpuSampleCursor_] = microseconds;
        ambientGpuSampleCursor_ =
            (ambientGpuSampleCursor_ + 1U) % ambientGpuSamplesUs_.size();
        ambientGpuSampleCount_ = std::min(
            ambientGpuSampleCount_ + 1U, ambientGpuSamplesUs_.size());
        ++ambientGpuSamplesSinceWindow_;
    }
    if (ambientGpuSamplesSinceWindow_ < 30U || ambientGpuSampleCount_ < 30U)
        return;
    ambientGpuSamplesSinceWindow_ = 0;
    std::vector<double> ordered(ambientGpuSamplesUs_.begin(),
        ambientGpuSamplesUs_.begin() + ambientGpuSampleCount_);
    std::sort(ordered.begin(), ordered.end());
    const auto p95 = ordered[std::min(ordered.size() - 1U,
        static_cast<std::size_t>(std::ceil(ordered.size() * 0.95)) - 1U)];
    ambientGpuP95Us_ = p95;
    if (p95 <= 500.0) {
        overBudgetWindows_ = 0;
        publishAmbientRenderDiagnostics();
        return;
    }
    if (++overBudgetWindows_ < 3U) {
        publishAmbientRenderDiagnostics();
        return;
    }
    overBudgetWindows_ = 0;
    if (!ambientDegraded_) {
        ambientDegraded_ = true;
        ambientTexture_.Reset();
        previousAmbientTexture_.Reset();
    } else {
        ambientBudgetFallback_ = true;
        ambientTexture_.Reset();
        previousAmbientTexture_.Reset();
    }
    publishAmbientRenderDiagnostics();
}

std::size_t AlbumModeRenderer::beginAmbientGpuTiming() noexcept
{
    collectAmbientGpuTiming();
    auto* device = render_.d3dDevice();
    auto* context = render_.immediateContext();
    if (!device || !context) return ambientTiming_.size();
    if (ambientTimingDevice_.Get() != device) {
        resetAmbientGpuTiming();
        ambientTimingDevice_ = device;
        const D3D11_QUERY_DESC disjointDesc{
            D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        const D3D11_QUERY_DESC timestampDesc{D3D11_QUERY_TIMESTAMP, 0};
        for (auto& slot : ambientTiming_) {
            if (FAILED(device->CreateQuery(&disjointDesc,
                    slot.disjoint.ReleaseAndGetAddressOf())) ||
                FAILED(device->CreateQuery(&timestampDesc,
                    slot.begin.ReleaseAndGetAddressOf())) ||
                FAILED(device->CreateQuery(&timestampDesc,
                    slot.end.ReleaseAndGetAddressOf()))) {
                resetAmbientGpuTiming();
                return ambientTiming_.size();
            }
        }
    }
    for (std::size_t scanned = 0; scanned < ambientTiming_.size(); ++scanned) {
        const auto index = (nextAmbientTimingSlot_ + scanned) % ambientTiming_.size();
        auto& slot = ambientTiming_[index];
        if (slot.pending || !slot.disjoint || !slot.begin || !slot.end) continue;
        context->Begin(slot.disjoint.Get());
        context->End(slot.begin.Get());
        nextAmbientTimingSlot_ = (index + 1U) % ambientTiming_.size();
        return index;
    }
    return ambientTiming_.size();
}

void AlbumModeRenderer::endAmbientGpuTiming(std::size_t slotIndex) noexcept
{
    auto* context = render_.immediateContext();
    if (!context || slotIndex >= ambientTiming_.size()) return;
    auto& slot = ambientTiming_[slotIndex];
    context->End(slot.end.Get());
    context->End(slot.disjoint.Get());
    slot.pending = true;
}

void AlbumModeRenderer::resetAmbientGpuTiming() noexcept
{
    for (auto& slot : ambientTiming_) {
        slot.pending = false;
        slot.end.Reset();
        slot.begin.Reset();
        slot.disjoint.Reset();
    }
    ambientTimingDevice_.Reset();
    ambientGpuSamplesUs_.fill(0.0);
    ambientGpuSampleCount_ = 0;
    ambientGpuSampleCursor_ = 0;
    ambientGpuSamplesSinceWindow_ = 0;
    nextAmbientTimingSlot_ = 0;
    overBudgetWindows_ = 0;
    ambientDegraded_ = qaAlbumAmbientForceDegraded();
    ambientBudgetFallback_ = false;
    ambientGpuLatestUs_ = 0.0;
    ambientGpuP95Us_ = 0.0;
    publishAmbientRenderDiagnostics();
}

void AlbumModeRenderer::publishAmbientRenderDiagnostics() const noexcept
{
    const auto bytes = [](ID2D1Bitmap* bitmap, std::uint64_t bytesPerPixel) {
        if (!bitmap) return std::uint64_t{0};
        const auto size = bitmap->GetPixelSize();
        return static_cast<std::uint64_t>(size.width) * size.height * bytesPerPixel;
    };
    const auto resourceBytes = bytes(ambientTexture_.Get(), 8U) +
        bytes(previousAmbientTexture_.Get(), 8U) +
        sizeof(AlbumAmbientConstants) * ambientTiming_.size();
    publishAlbumAmbientRenderDiagnostics(resourceBytes, ambientDegraded_,
        ambientGpuSampleCount_ != 0, ambientGpuLatestUs_, ambientGpuP95Us_);
}

bool AlbumModeRenderer::prepareBookletBackdrop(
    float topInsetDip, const D2D1_RECT_F& coverRect)
{
    auto* backing = render_.retainedBacking();
    if (!backing) return false;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
    if (FAILED(render_.target()->QueryInterface(
            IID_PPV_ARGS(context.ReleaseAndGetAddressOf())))) return false;
    const auto pixels = backing->GetPixelSize();
    if (pixels.width == 0 || pixels.height == 0) return false;
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
            D2D1_ALPHA_MODE_IGNORE));
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> frozen;
    if (FAILED(context->CreateBitmap(pixels, nullptr, 0, &properties,
            frozen.ReleaseAndGetAddressOf())) ||
        FAILED(frozen->CopyFromBitmap(nullptr, backing, nullptr))) return false;

    // The previous accepted central scene is the stable source. Remove the
    // sharp cover before blur so the morphing cover remains the sole artwork
    // instance. This edit happens once on booklet entry, never per animation.
    Microsoft::WRL::ComPtr<ID2D1Image> oldTarget;
    context->GetTarget(oldTarget.ReleaseAndGetAddressOf());
    context->SetTarget(frozen.Get());
    context->BeginDraw();
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> coverMask;
    if (SUCCEEDED(context->CreateSolidColorBrush(
            D2D1::ColorF(0.018f, 0.022f, 0.023f, 1.0f),
            coverMask.ReleaseAndGetAddressOf()))) {
        auto masked = coverRect;
        masked.top += topInsetDip;
        masked.bottom += topInsetDip;
        context->FillRectangle(masked, coverMask.Get());
    }
    const auto maskStatus = context->EndDraw();
    context->SetTarget(oldTarget.Get());
    if (FAILED(maskStatus)) return false;

    Microsoft::WRL::ComPtr<ID2D1Effect> blur;
    if (FAILED(context->CreateEffect(CLSID_D2D1GaussianBlur,
            blur.ReleaseAndGetAddressOf()))) return false;
    blur->SetInput(0U, frozen.Get());
    (void)blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 16.0f);
    (void)blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,
        D2D1_GAUSSIANBLUR_OPTIMIZATION_QUALITY);
    (void)blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,
        D2D1_BORDER_MODE_HARD);
    bookletBackdrop_ = std::move(frozen);
    bookletBackdropBlur_ = std::move(blur);
    return true;
}

void AlbumModeRenderer::drawBookletBackdrop(float topInsetDip,
    const D2D1_RECT_F& contentRect, float opacity)
{
    if (!bookletBackdropBlur_ || opacity <= 0.0f) return;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
    if (FAILED(render_.target()->QueryInterface(
            IID_PPV_ARGS(context.ReleaseAndGetAddressOf())))) return;
    const auto source = D2D1::RectF(contentRect.left,
        contentRect.top + topInsetDip, contentRect.right,
        contentRect.bottom + topInsetDip);
    const auto layer = D2D1::LayerParameters(contentRect, nullptr,
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
        std::clamp(opacity, 0.0f, 1.0f));
    render_.target()->PushLayer(layer, nullptr);
    context->DrawImage(bookletBackdropBlur_.Get(),
        D2D1::Point2F(contentRect.left, contentRect.top - topInsetDip),
        source, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
        D2D1_COMPOSITE_MODE_SOURCE_OVER);
    render_.target()->PopLayer();
}

void AlbumModeRenderer::drawArtwork(ID2D1Bitmap* bitmap,
    const D2D1_RECT_F& target, float opacity) const
{
    if (!bitmap || target.right <= target.left || target.bottom <= target.top) return;
    render_.target()->DrawBitmap(bitmap, target, opacity,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void AlbumModeRenderer::drawTracklist(const AlbumModeLayout& layout,
    const AlbumSessionSnapshot& session,float scroll)
{
    trackExtras_.draw(render_,layout,session,scroll,alloySceneReady_);
    if(focusedTrack_ && *focusedTrack_<session.tracks.size()){
        auto row=layout.tracks;row.top+=38+float(*focusedTrack_)*28-scroll;row.bottom=row.top+27;
        if(row.top>=layout.tracks.top+38 && row.bottom<=layout.tracks.bottom)
            render_.target()->DrawRectangle(row,render_.brush(PanelColor::accent),1.5f);
    }
}

void AlbumModeRenderer::preparePlainLyrics(HWND window, const AlbumModeLayout& layout)
{
    lyricsStaging_ = false; // Completion, rather than an animation timer, wakes this surface.
    if (!plainLyricsBatch_) {
        auto batch = std::make_shared<AsyncTextBatch>();
        batch->leading = 10.f;
        batch->styles.push_back(AsyncTextStyle::fromFormat(lyricPlain_.Get()));
        batch->blocks.reserve(lyricLines_.size());
        for (const auto& line : lyricLines_)
            batch->blocks.push_back({std::make_shared<const std::wstring>(line), 0U, 0.f, 1.08f, 12.f});
        plainLyricsBatch_ = std::move(batch);
    }
    const auto textWidth = std::max(1.f, width(layout.lyrics) - 28.f);
    const auto viewHeight = std::max(1.f, height(layout.lyrics) - 16.f);
    const auto now = rendererClockSeconds();
    lyricScroll_.setResponseScale(motionMode() == MotionMode::off ? 0.0 :
        motionMode() == MotionMode::reduced ? .55 : 1.0);
    if (lyricScroll_.advanceTo(now)) ++lyricsGestureRevision_;
    lyricScrollDip_ = static_cast<float>(lyricScroll_.position());
    plainLyricsFrame_ = plainLyricsText_.prepareBatch(plainLyricsOwner_, lyricsRevision_,
        plainLyricsBatch_, textWidth, viewHeight, lyricScrollDip_, window);
    if (plainLyricsFrame_ && plainLyricsFrame_->generation == plainLyricsText_.stats().generation) {
        preparedLyricsHeight_ = plainLyricsFrame_->geometry->height;
        lyricMaximumScrollDip_ = std::max(0.f, preparedLyricsHeight_ - std::max(0.f, height(layout.lyrics) - 10.f));
        lyricScroll_.setBounds(0.0, lyricMaximumScrollDip_);
        if (pendingPlainWheel_ != 0.f) {
            (void)lyricScroll_.addWheelImpulseAt(pendingPlainWheel_, now);
            pendingPlainWheel_ = 0.f;
        }
        lyricScrollDip_ = static_cast<float>(lyricScroll_.position());
        if (!plainLyricsFrame_->covers(lyricScrollDip_, viewHeight))
            plainLyricsFrame_ = plainLyricsText_.prepareBatch(plainLyricsOwner_, lyricsRevision_,
                plainLyricsBatch_, textWidth, viewHeight, lyricScrollDip_, window);
    }
}

void AlbumModeRenderer::drawPlainLyrics(const AlbumModeLayout& layout)
{
    auto* target = render_.target();
    const auto viewport = D2D1::RectF(layout.lyrics.left + 14.f, layout.lyrics.top + 8.f,
        layout.lyrics.right - 14.f, layout.lyrics.bottom - 8.f);
    auto scrim = layout.lyrics;
    if (plainLyricsFrame_) scrim.bottom = std::min(scrim.bottom,
        scrim.top + plainLyricsFrame_->geometry->height + 16.f);
    if (!alloySceneReady_) render_.fill(scrim, PanelColor::panel, .40f);
    render_.text(lyricsRetry_ ? L"RETRY" : L"FOLLOW", layout.follow, PanelText::center,
        std::chrono::steady_clock::now() < manualLyricsUntil_ ? PanelColor::accent : PanelColor::secondary);
    if (plainLyricsFrame_) {
        const auto& frame = *plainLyricsFrame_;
        const auto covered = frame.covers(lyricScrollDip_, height(viewport));
        const auto offset = covered ? lyricScrollDip_ : frame.scroll;
        auto clipped = viewport;
        clipped.right = std::min(clipped.right, clipped.left + frame.width);
        if (!covered) clipped.bottom = std::min(clipped.bottom, clipped.top + frame.viewportHeight);
        const auto first = std::lower_bound(frame.geometry->blocks.begin(), frame.geometry->blocks.end(), offset,
            [](const auto& block, float top){return block.top + block.height <= top;});
        D2D1_MATRIX_3X2_F original{};
        target->GetTransform(&original);
        const auto scale = motionMode() == MotionMode::normal ? .94f : 1.f;
        for (auto it = first; it != frame.geometry->blocks.end() && it->top < offset + height(clipped); ++it) {
            const auto centerY = albumPlainLyricsScaleCenter(
                clipped, it->top, it->height, offset);
            target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale,
                D2D1::Point2F(clipped.left, centerY)) * original);
            (void)frame.cache->drawPrepared(target, *it->document, clipped,
                render_.brush(PanelColor::text), clipped.top + it->top, offset);
        }
        target->SetTransform(original);
    }
    if (!plainLyricsFrame_ || plainLyricsText_.stats().status == AsyncTextStatus::failed) {
        auto status = viewport; status.bottom = std::min(status.bottom, status.top + 30.f);
        render_.text(plainLyricsText_.stats().status == AsyncTextStatus::failed
            ? L"Lyrics text unavailable. Reopen to retry." : L"Preparing lyrics…",
            status, PanelText::normal, PanelColor::muted);
    }
}

void AlbumModeRenderer::drawLyrics(
    const AlbumModeLayout& layout, double playbackSeconds)
{
    if (layout.lyrics.right <= layout.lyrics.left || lyricLines_.empty()) return;
    auto* target = render_.target();
    if (!target) return;
    const auto synced = lyrics_ &&
        (lyrics_->timing == TimingStatus::catalogMatched ||
         lyrics_->timing == TimingStatus::audioVerified);
    if (!synced) { drawPlainLyrics(layout); return; }
    auto activeLine = synced
        ? activeLyricsCueIndex(*lyrics_, static_cast<std::int64_t>(
            std::llround(playbackSeconds * 1000.0)))
        : std::optional<std::size_t>{};
    const auto now = std::chrono::steady_clock::now();
    if (pendingCueIndex_) {
        if (activeLine == pendingCueIndex_ &&
                now - pendingCueStarted_ >= std::chrono::milliseconds(20)) {
            pendingCueIndex_.reset();
        } else if (now - pendingCueStarted_ < std::chrono::milliseconds(750)) {
            activeLine = pendingCueIndex_;
        } else {
            pendingCueIndex_.reset();
        }
    }
    const auto textWidth = std::max(1.0f, width(layout.lyrics) - 28.0f);
    ensureLyricsLayout(textWidth);
    auto lyricsScrim = layout.lyrics;
    lyricsScrim.bottom = std::min(layout.lyrics.bottom,
        layout.lyrics.top + preparedLyricsHeight_ + 16.0f);
    if(!alloySceneReady_)render_.fill(lyricsScrim, PanelColor::panel, 0.40f);
    const auto viewportHeight = std::max(0.0f, height(layout.lyrics) - 10.0f);
    const auto maximumScroll = std::max(
        0.0f, preparedLyricsHeight_ - viewportHeight);
    lyricMaximumScrollDip_ = maximumScroll;
    lyricScroll_.setBounds(0.0, maximumScroll);
    lyricScroll_.setResponseScale(
        motionMode() == MotionMode::off ? 0.0 :
        motionMode() == MotionMode::reduced ? 0.55 : 1.0);
    const auto nowSeconds = rendererClockSeconds();
    if (activeLine && now >= manualLyricsUntil_ &&
        *activeLine < preparedLyrics_.size()) {
        const auto& active = preparedLyrics_[*activeLine];
        const auto wanted = std::clamp(
            active.top + active.height * 0.5f -
                viewportHeight * 0.5f,
            0.0f, maximumScroll);
        const auto delta = wanted - lyricScroll_.target();
        if (std::abs(delta) > 0.05) {
            (void)lyricScroll_.addWheelImpulseAt(delta, nowSeconds);
        }
    }
    if (lyricScroll_.advanceTo(nowSeconds)) ++lyricsGestureRevision_;
    lyricScrollDip_ = static_cast<float>(lyricScroll_.position());
    render_.text(lyricsRetry_?L"RETRY":L"FOLLOW", layout.follow, PanelText::center,
        now < manualLyricsUntil_ ? PanelColor::accent : PanelColor::secondary);

    const auto motion = motionMode();
    const auto colorDuration = motion == MotionMode::normal ? 0.160 :
        motion == MotionMode::reduced ? 0.100 : 0.0;
    if (lyricVisuals_.size() != preparedLyrics_.size()) {
        lyricVisuals_.assign(preparedLyrics_.size(), {});
    }
    for (std::size_t index = 0; index < lyricVisuals_.size(); ++index) {
        auto& visual = lyricVisuals_[index];
        const auto desired = activeLine && index == *activeLine;
        const auto elapsed = visual.started == std::chrono::steady_clock::time_point{}
            ? colorDuration : std::chrono::duration<double>(now - visual.started).count();
        const auto progress = colorDuration <= 0.0
            ? 1.0f : quinticSmooth(static_cast<float>(elapsed / colorDuration));
        const auto current = visual.from +
            ((visual.targetActive ? 1.0f : 0.0f) - visual.from) * progress;
        visual.value = current;
        if (desired != visual.targetActive) {
            visual.from = current;
            visual.targetActive = desired;
            visual.started = now;
        }
        if (colorDuration <= 0.0) visual.value = desired ? 1.0f : 0.0f;
    }

    const auto viewport = D2D1::RectF(layout.lyrics.left + 14.0f,
        layout.lyrics.top + 8.0f, layout.lyrics.right - 14.0f,
        layout.lyrics.bottom - 8.0f);
    const auto neutral = UiDesignSystemV1::color(UiColorToken::text);
    const auto bright = UiDesignSystemV1::color(UiColorToken::textBright);
    if(!lyricBrush_)(void)target->CreateSolidColorBrush(D2D1::ColorF(
        neutral.red, neutral.green, neutral.blue, neutral.alpha),lyricBrush_.GetAddressOf());
    auto& lyricBrush=lyricBrush_;
    D2D1_MATRIX_3X2_F baseTransform{};
    target->GetTransform(&baseTransform);
    auto first = std::lower_bound(preparedLyrics_.begin(),
        preparedLyrics_.end(), lyricScrollDip_,
        [](const PreparedLyricsLine& line, float top) {
            return line.top + line.height <= top;
        });
    for (auto iterator = first; iterator != preparedLyrics_.end(); ++iterator) {
        if (iterator->top > lyricScrollDip_ + height(viewport)) break;
        const auto index = static_cast<std::size_t>(
            std::distance(preparedLyrics_.begin(), iterator));
        const auto& document = iterator->normal;
        if (!document) continue;
        const auto envelope = index < lyricVisuals_.size()
            ? lyricVisuals_[index].value : 0.0f;
        const auto scale = motion == MotionMode::normal
            ? 0.94f + 0.12f * envelope : 1.0f;
        const auto centerY = viewport.top + iterator->top - lyricScrollDip_ +
            iterator->height * 0.5f;
        target->SetTransform(D2D1::Matrix3x2F::Scale(
            scale, scale, D2D1::Point2F(viewport.left, centerY)) * baseTransform);
        if (lyricBrush) lyricBrush->SetColor(D2D1::ColorF(
            neutral.red + (bright.red - neutral.red) * envelope,
            neutral.green + (bright.green - neutral.green) * envelope,
            neutral.blue + (bright.blue - neutral.blue) * envelope,
            neutral.alpha + (bright.alpha - neutral.alpha) * envelope));
        textLayouts_.drawVisible(target, *document, viewport,
            lyricBrush ? lyricBrush.Get() : render_.brush(PanelColor::text),
            viewport.top + iterator->top, lyricScrollDip_);
        target->SetTransform(baseTransform);
        if (envelope > 0.001f) {
            render_.fill(D2D1::RectF(viewport.left - 6.0f,
                centerY - iterator->height * 0.45f,
                viewport.left - 4.0f,
                centerY + iterator->height * 0.45f), bright, envelope);
        }
    }
    target->SetTransform(baseTransform);
}

void AlbumModeRenderer::drawNavigation(
    const D2D1_SIZE_F& size,
    float heightDip,
    bool albumActive)
{
    // Sample the pair once and draw both labels from that exact palette
    // instant. The general PanelD2D brush cache is ideal for a complete
    // surface, but this strip is repainted independently; using explicit
    // colours prevents an accepted partial present from retaining one cached
    // endpoint while the other label advances with a later surface frame.
    const auto theme = ThemeService::instance().snapshot();
    const UiColor accent{
        theme.accent.red, theme.accent.green, theme.accent.blue, 1.0f};
    const UiColor secondary{
        theme.secondary.red, theme.secondary.green, theme.secondary.blue, 1.0f};
    heightDip = std::clamp(heightDip, 0.0f, size.height);
    const auto libraryRight = std::min(108.0f, size.width);
    const auto albumLeft = libraryRight;
    const auto albumRight = std::min(216.0f, size.width);
    render_.fill(D2D1::RectF(0.0f, 0.0f, size.width, heightDip),
        PanelColor::background);
    if (libraryRight > 0.0f) {
        render_.text(L"LIBRARY",
            D2D1::RectF(0.0f, 0.0f, libraryRight, heightDip),
            PanelText::center,
            albumActive ? secondary : accent,
            albumActive ? 0.72f : 1.0f);
    }
    if (albumRight > albumLeft) {
        render_.text(L"ALBUM",
            D2D1::RectF(albumLeft, 0.0f, albumRight, heightDip),
            PanelText::center,
            albumActive ? accent : secondary,
            albumActive ? 1.0f : 0.72f);
    }
    render_.line(0.0f, std::max(0.0f, heightDip - 1.0f),
        size.width, std::max(0.0f, heightDip - 1.0f),
        PanelColor::border);
}

FrameOutcome AlbumModeRenderer::paintNavigation(
    HWND window,
    HDC dc,
    bool albumActive,
    float heightDip)
{
    RECT client{};
    GetClientRect(window, &client);
    const auto dpi = std::max<UINT>(1U, GetDpiForWindow(window));
    RECT dirty{0, 0, client.right,
        static_cast<LONG>(std::ceil(heightDip * dpi / 96.0f))};
    if (!render_.beginPartial(window, dc, dirty)) {
        return render_.lastFrameOutcome();
    }
    onResourceEpochChanged(render_.resourceEpoch());
    drawNavigation(render_.sizeDip(), heightDip, albumActive);
    const auto status = render_.end();
    const auto outcome=render_.lastFrameOutcome();
    if(status==D2DERR_RECREATE_TARGET || outcome.state==FrameResult::failed)releaseGraphics();
    return outcome;
}

FrameOutcome AlbumModeRenderer::paint(HWND window, HDC dc,
    const std::shared_ptr<const AlbumSessionSnapshot>& session,
    const std::shared_ptr<const LocalTrackContent>& content,
    const std::shared_ptr<const LyricsSnapshot>& lyrics,
    const std::shared_ptr<const ArtPixels>& artwork,
    const std::wstring& artworkIdentity,
    double playbackSeconds, float trackScroll,
    AlbumPanelKind compactSelection, AlbumPanelVisibility visibility,
    float bookletProgress,
    std::wstring_view personalNote, std::uint64_t personalNoteRevision,
    const AlbumAmbientPose& ambientPose,
    bool rebuildAmbientTexture,
    float topInsetDip, float bottomInsetDip, ArtCacheStatus artworkStatus)
{
    ambientTextureProduced_ = false;
    artworkVisible_ = visibility.artwork;
    auto& telemetry=PerformanceTelemetry::instance();
    const auto prepareStarted=telemetry.beginSpan(TelemetrySurface::album, 100);
    const auto surfacePrepared=render_.prepare(window);
    telemetry.endSpan(TelemetrySurface::album, 100, prepareStarted);
    if (!surfacePrepared) return render_.lastFrameOutcome();
    onResourceEpochChanged(render_.resourceEpoch());
    const bool ready=session && session->state==AlbumSessionState::ready && session->sessionId!=0;
    lyricsRetry_=ready && !session->outsideSession && lyrics && lyrics->state==LyricsState::failed &&
        !lyrics->document && session->currentInSession<session->tracks.size() &&
        !session->tracks[session->currentInSession].entityId.empty() &&
        lyrics->request.recording.entityId==session->tracks[session->currentInSession].entityId;
    // Show recoverable session UI before artwork uploads, lyrics layout or any
    // decorative shader initialization. Only an accepted frame releases this gate.
    graphicsStaging_=ready && !initialUiPresented_;
    if (!ready || !initialUiPresented_) {
        alloySceneReady_=false;
        if(!ready)lyricsStaging_=false;
        if (!render_.begin(window,dc)) return render_.lastFrameOutcome();
        const auto size=render_.sizeDip();
        const auto layout=computeAlbumModeLayout(size.width,
            std::max(0.f,size.height-topInsetDip-bottomInsetDip),true,compactSelection,visibility);
        render_.fill(D2D1::RectF(0,0,size.width,size.height),PanelColor::background);
        drawNavigation(size,topInsetDip,true);
        render_.target()->SetTransform(D2D1::Matrix3x2F::Translation(0,topInsetDip));
        std::wstring message=L"CHOOSE AN ALBUM",action=L"OPEN LIBRARY";
        if(ready){message=L"PREPARING VISUAL…";action.clear();}
        else if(session && session->state==AlbumSessionState::loading) message=L"LOADING ALBUM…";
        else if(session && session->state==AlbumSessionState::error){
            message=session->error.empty()?L"ALBUM UNAVAILABLE":session->error;action=L"RETRY ALBUM";
        }
        render_.fill(layout.chooseAlbum,PanelColor::panel,.9f);
        auto description=layout.chooseAlbum,button=layout.chooseAlbum;
        description.bottom=(description.top+description.bottom)*.5f;button.top=description.bottom;
        if(action.empty())description=layout.chooseAlbum;
        render_.text(message,description,PanelText::center,
            session && session->state==AlbumSessionState::error?PanelColor::amber:PanelColor::text);
        if(!action.empty())render_.text(action,button,PanelText::center,PanelColor::accent);
        if(focusRect_.right>focusRect_.left && focusRect_.bottom>focusRect_.top)
            render_.target()->DrawRectangle(focusRect_,render_.brush(PanelColor::accent),1.5f);
        render_.target()->SetTransform(D2D1::Matrix3x2F::Identity());
        const auto started=telemetry.beginSpan(TelemetrySurface::album,106);
        const auto status=render_.end();
        telemetry.endSpan(TelemetrySurface::album,106,started);
        const auto outcome=render_.lastFrameOutcome();
        if(ready && outcome.state==FrameResult::accepted) initialUiPresented_=true;
        if(status==D2DERR_RECREATE_TARGET || outcome.state==FrameResult::failed) releaseGraphics();
        return outcome;
    }
    const auto beforeSize = render_.sizeDip();
    const auto beforeContentHeight = std::max(0.0f,
        beforeSize.height - topInsetDip - bottomInsetDip);
    const auto beforeLayout = computeAlbumModeLayout(beforeSize.width,
        beforeContentHeight, true, compactSelection, visibility);
    const auto representedLyrics = session && !session->outsideSession
        ? lyrics : std::shared_ptr<const LyricsSnapshot>{};
    if (visibility.lyrics) ensureLyrics(representedLyrics);
    const auto hasLyrics = visibility.lyrics && representedLyrics && representedLyrics->document &&
        (!representedLyrics->document->plainTextUtf8.empty() ||
         !representedLyrics->document->cues.empty());
    ensureFormats();
    if(hasLyrics&&bookletProgress<.32f&&beforeLayout.lyrics.right>beforeLayout.lyrics.left) {
        const auto synced = lyrics_ && (lyrics_->timing == TimingStatus::catalogMatched ||
            lyrics_->timing == TimingStatus::audioVerified);
        if (synced) {
            plainLyricsText_.cancel();plainLyricsFrame_.reset();
            ensureLyricsLayout(std::max(1.f,width(beforeLayout.lyrics)-28.f));
        } else if (surfaceTextDemand_) preparePlainLyrics(window, beforeLayout);
    } else {
        plainLyricsText_.cancel();
        lyricsStaging_ = false;
    }
    D2D1_RECT_F writtenViewport{};
    if (session && writtenOwner_ != session->sessionId) {
        bookletText_.cancel();
        bookletFrame_.reset();
        writtenText_.reset();
        writtenOwner_ = session->sessionId;
        bookletScrollDip_ = bookletMaxScrollDip_ = 0.0f;
    }
    if (bookletProgress > .001f && visibility.lyrics) {
        const auto booklet = computeAlbumBookletLayout(
            beforeSize.width, beforeContentHeight, visibility);
        writtenViewport = D2D1::RectF(booklet.writtenDetails.left + 12.f,
            booklet.writtenDetails.top + 12.f, booklet.writtenDetails.right - 12.f,
            booklet.writtenDetails.bottom - 12.f);
        if (textDemand_ && content && width(writtenViewport) > 0.f && height(writtenViewport) > 0.f) {
            const auto revision = semanticRevision({
                content ? content->revision : 0U, personalNoteRevision,
                session->outsideSession ? 1U : 0U});
            if (writtenTextRevision_ != revision || !writtenText_) {
                auto written = std::make_shared<std::wstring>();
                if (content && !content->biography.empty()) *written += L"BIO · LOCAL\n" + content->biography;
                if (content && !content->review.empty()) {
                    if (!written->empty()) *written += L"\n\n";
                    *written += L"REVIEW · LOCAL\n" + content->review;
                }
                if (written->empty()) *written = L"No local booklet text";
                if (!personalNote.empty()) {
                    *written += L"\n\nNOTE · PERSONAL\n";
                    written->append(personalNote);
                } else if (session->outsideSession) {
                    *written += L"\n\nNOTE · READ ONLY\nCurrent playback belongs to another album.";
                }
                writtenText_ = std::move(written);
                writtenTextRevision_ = revision;
            }
            static const AsyncTextStyle bookletStyle;
            bookletFrame_ = bookletText_.prepare(writtenOwner_, revision,
                writtenText_, bookletStyle, width(writtenViewport),
                height(writtenViewport), bookletScrollDip_, window);
            if (bookletFrame_ && bookletFrame_->generation == bookletText_.stats().generation) {
                bookletMaxScrollDip_ = std::max(0.f,
                    bookletFrame_->document->height - height(writtenViewport));
                bookletScrollDip_ = std::clamp(bookletScrollDip_, 0.f, bookletMaxScrollDip_);
            }
        } else if (textDemand_ && !content) {
            // NEXT can change the content key without changing the album owner.
            // Revoke the old bounds while retaining the user's scroll intent.
            bookletText_.cancel();
            bookletFrame_.reset();
        }
    } else {
        bookletText_.cancel();
        bookletFrame_.reset();
    }
    auto lyricsProtection=hasLyrics?beforeLayout.lyrics:statusCard(beforeLayout.lyrics);
    if(hasLyrics)lyricsProtection.bottom=std::min(lyricsProtection.bottom,
        lyricsProtection.top+preparedLyricsHeight_+16.f);
    const auto tracksProtection = session
        ? occupiedTrackRect(beforeLayout.tracks, session->tracks.size())
        : D2D1::RectF();
    const auto openingBooklet = bookletProgress > 0.001f && !bookletOpen_;
    // A frozen screenshot must not cover a live visualizer. Preserve the
    // live texture drawn below; the booklet only darkens it for reading.
    if (openingBooklet || (bookletProgress <= 0.001f && bookletOpen_)) {
        bookletBackdrop_.Reset();
        bookletBackdropBlur_.Reset();
    }
    bookletOpen_ = bookletProgress > 0.001f;
    ensureArtwork(artwork, artworkIdentity,
        static_cast<float>(std::max<UINT>(1U, GetDpiForWindow(window))),artworkStatus);
    const auto shouldBuildAmbient = !curatedDeferred_ && (rebuildAmbientTexture || !ambientTexture_);
    if (a4::style()==a4::Style::flow31 && artworkHash_ != 0 && shouldBuildAmbient && !ambientBudgetFallback_) {
        if (!ambientEffect_) (void)prepareAmbientEffect(ambientEffect_);
        auto result = AmbientBuildResult::produced;
        if (previousArtworkHash_ != 0 && previousAmbientTexture_) {
            result = buildAmbientTexture(ambientEffect_.Get(),
                previousAmbientPalette_, ambientPose,
                lyricsProtection, tracksProtection, previousArtworkHash_,
                beforeLayout.viewport, previousAmbientTexture_);
        }
        const auto currentResult = buildAmbientTexture(ambientEffect_.Get(),
            ambientPalette_, ambientPose,
            lyricsProtection, tracksProtection, artworkHash_,
            beforeLayout.viewport, ambientTexture_);
        if (currentResult != AmbientBuildResult::produced) result = currentResult;
        ambientTextureProduced_ = result == AmbientBuildResult::produced;
        if (!ambientTextureProduced_) ambientBudgetFallback_ = true;
        publishAmbientRenderDiagnostics();
    }

    const bool useCurated=curatedSelected();
    curatedSelected_=useCurated;
    if(!useCurated)curated_.suspend(rendererClockSeconds());
    if(useCurated && render_.backend()==SurfaceBackend::hardwareDxgi) {
        Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
        if(SUCCEEDED(render_.target()->QueryInterface(IID_PPV_ARGS(context.GetAddressOf())))) {
            curated::Input input;
            input.preset=curated::presetForStyle(static_cast<unsigned>(a4::style()));
            input.mode=curated::mode();input.palette=curated::palette();
            input.response=curated::response();input.speed=curated::causticsSpeed();
            input.active=curatedActive_;input.playing=curatedPlaying_;
            input.restartRevision=curated::restartRevision();
            if(input.mode==curated::Mode::music){
                curated::MusicSignals signals;
                signals.absolute=ambientPose.absoluteEnergy;
                signals.relativeFast=ambientPose.relativeFast;
                signals.relativeSlow=ambientPose.relativeSlow;
                signals.growth=ambientPose.growth;signals.attack=ambientPose.attack;
                signals.gate=ambientPose.gate;signals.intensity=ambientPose.gain;
                const auto bound=curated::bindMusic(input.preset,signals,input.response);
                input.band=bound.continuous;input.transient=bound.impulse;
                input.intensity=bound.intensity;
            }
            const auto palette=displayedArtwork_?displayedArtwork_->palette:ArtPalette{};
            const auto asColor=[](const RgbColor& c){return curated::Color{c.red,c.green,c.blue,1.0f};};
            input.colors={asColor(palette.flowLow),asColor(palette.flowMid),
                asColor(palette.flowHigh),asColor(palette.flowBase)};
            if(a4::style()==a4::Style::selectedCollection ||
                (a4::style()==a4::Style::curatedCaustics&&curated::expressive(input))){
                for(unsigned i=0;i<4;++i)input.colors[i]=asColor(palette.material[i]);
            }
            const auto started=telemetry.beginSpan(TelemetrySurface::album,103);
            (void)curated_.prepare(render_.d3dDevice(),render_.immediateContext(),context.Get(),
                render_.resourceEpoch(),D2D1::SizeF(beforeSize.width,beforeContentHeight),
                input,rendererClockSeconds(),!curatedDeferred_);
            telemetry.endSpan(TelemetrySurface::album,103,started);
            // The independent renderer owns its bounded result-poll deadline,
            // including the first static image. Do not turn IPC startup into
            // an immediate display-rate resource-staging animation.
            graphicsStaging_=!curated_.asynchronous() && !curated_.diagnostics().ready && SUCCEEDED(curated_.diagnostics().lastHr);
            ambientTextureProduced_=curated_.producedThisFrame();
        }
    }

    // Resource generation, material and complete image preparation happen outside
    // PanelD2D::begin. Only the final image and crisp UI text enter owner BeginDraw.
    alloySceneReady_=false;
    const bool useAlloy=a4::style()==a4::Style::spectralAlloy;
    alloySelected_=useAlloy;
    const auto nowSeconds=rendererClockSeconds();

    if(useAlloy&&!curatedDeferred_&&render_.backend()==SurfaceBackend::hardwareDxgi){
        Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
        if(SUCCEEDED(render_.target()->QueryInterface(IID_PPV_ARGS(context.GetAddressOf())))){
            (void)alloy_.setQualityMode(a4::qualityMode());
            alloy_.beginMeasurement(render_.d3dDevice(),render_.immediateContext(),true);
            const auto material=displayedArtwork_?displayedArtwork_->material:std::shared_ptr<const a4::Material>{};
            const auto safePose=motionMode()==MotionMode::normal?ambientPose:AlbumAmbientPose{};
            const auto started=telemetry.beginSpan(TelemetrySurface::album,104);
            const bool prepared=alloy_.prepare(context.Get(),render_.resourceEpoch(),
                D2D1::SizeF(beforeSize.width,beforeContentHeight),float(GetDpiForWindow(window)),
                material,safePose,rebuildAmbientTexture,false,a4::grainAmplitude(),nowSeconds);
            telemetry.endSpan(TelemetrySurface::album,104,started);
            graphicsStaging_=alloy_.resourcesPending();
            a4::SceneGeometry geometry;
            geometry.cover=containRect(beforeLayout.cover,displayedArtwork_?displayedArtwork_->width:1U,
                displayedArtwork_?displayedArtwork_->height:1U);
            geometry.tracks=tracksProtection;geometry.lyrics=lyricsProtection;
            geometry.coverOpacity=visibility.artwork?artworkOpacity_:0.f;geometry.coverRevision=artworkVisualRevision_;
            geometry.coverOptics=visibility.artwork&&motionMode()==MotionMode::normal;
            geometry.bookletProgress=bookletProgress;
            if(bookletProgress>.001f){
                const auto b=computeAlbumBookletLayout(
                    beforeSize.width,beforeContentHeight,visibility);
                geometry.cover=interpolateRect(geometry.cover,containRect(b.artwork,
                    displayedArtwork_?displayedArtwork_->width:1U,
                    displayedArtwork_?displayedArtwork_->height:1U),bookletProgress);
                geometry.bookletAlbum=b.albumDetails;geometry.bookletWritten=b.writtenDetails;
                geometry.coverOptics=false;
            }
            TelemetrySpan composeSpan(TelemetrySurface::album,105);
            if(prepared)alloySceneReady_=alloy_.compose(context.Get(),artworkTransition_.currentBitmap(),
                geometry,artworkVisible_&&artworkTransition_.active(motionMode()),&artworkTransition_,
                [](void* owner,ID2D1DeviceContext* ctx,const D2D1_RECT_F& rect,float opacity){
                    return static_cast<ArtworkTransitionRenderer*>(owner)->draw(ctx,rect,motionMode(),opacity);
                });
            ambientTextureProduced_=alloySceneReady_&&alloy_.diagnostics().fieldProduced;
            const auto& diag=alloy_.diagnostics();
            a4::trace("alloy.prepared",diag.fieldRevision,diag.lastHr,double(diag.resourceBytes),double(diag.sceneCompositions));
        }
    }

    if (!render_.begin(window, dc)) {alloy_.endMeasurement();return render_.lastFrameOutcome();}
    const auto fullSize = render_.sizeDip();
    topInsetDip = std::clamp(topInsetDip, 0.0f, fullSize.height);
    bottomInsetDip = std::clamp(
        bottomInsetDip, 0.0f, fullSize.height - topInsetDip);
    const auto contentHeight = std::max(
        0.0f, fullSize.height - topInsetDip - bottomInsetDip);
    auto* target = render_.target();
    target->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, topInsetDip));
    target->PushAxisAlignedClip(
        D2D1::RectF(0.0f, 0.0f, fullSize.width, contentHeight),
        D2D1_ANTIALIAS_MODE_ALIASED);
    ensureFormats();
    const auto transitionDuration = motionMode() == MotionMode::normal
        ? std::chrono::milliseconds(620)
        : motionMode() == MotionMode::reduced
            ? std::chrono::milliseconds(120)
            : std::chrono::milliseconds(0);
    const auto elapsed = std::chrono::steady_clock::now() - artworkChangedAt_;
    const auto blend = !previousAmbientTexture_ || transitionDuration.count() == 0
        ? 1.0f : std::clamp(static_cast<float>(
            std::chrono::duration<double>(elapsed).count() /
            std::chrono::duration<double>(transitionDuration).count()),
            0.0f, 1.0f);
    const auto finish = [this, blend, target, fullSize, topInsetDip]() {
        if(focusRect_.top>=0 && focusRect_.right>focusRect_.left && focusRect_.bottom>focusRect_.top)
            target->DrawRectangle(focusRect_,render_.brush(PanelColor::accent),1.5f);
        target->PopAxisAlignedClip();
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        // Keep the mode switcher in the same Direct2D presentation as the
        // album surface. Mixing a GDI DrawText pass after EndDraw produced a
        // visible black/grey flash during artwork and palette transitions.
        if (topInsetDip > 0.0f) {
            drawNavigation(fullSize, topInsetDip, true);
            if(focusRect_.top<0 && focusRect_.right>focusRect_.left){
                auto rect=focusRect_;rect.top+=topInsetDip;rect.bottom+=topInsetDip;
                target->DrawRectangle(rect,render_.brush(PanelColor::accent),1.5f);
            }
        }
        if(!actionStatus_.empty() && std::chrono::steady_clock::now()<actionStatusUntil_){
            const auto box=D2D1::RectF(16,topInsetDip+42,fullSize.width-16,topInsetDip+78);
            render_.fill(box,PanelColor::panel,.96f);
            render_.text(actionStatus_,box,PanelText::center,PanelColor::amber);
        }
        auto& telemetry=PerformanceTelemetry::instance();
        const auto started=telemetry.beginSpan(TelemetrySurface::album,106);
        const auto artworkTicket = artworkTransition_.pendingPresentation();
        const auto status = render_.end();
        telemetry.endSpan(TelemetrySurface::album,106,started);
        if (blend >= 1.0f &&
            render_.lastFrameResult() == FrameResult::accepted) {
            previousAmbientTexture_.Reset();
            previousAmbientPalette_ = {};
        }
        const auto outcome=render_.lastFrameOutcome();
        if(curatedSelected())curated_.presented(outcome.state==FrameResult::accepted);
        artworkTransition_.presented(artworkTicket, outcome.state==FrameResult::accepted);
        alloy_.endMeasurement();
        if(status==D2DERR_RECREATE_TARGET || outcome.state==FrameResult::failed)releaseGraphics();
        return outcome;
    };
    const auto size = D2D1::SizeF(fullSize.width, contentHeight);
    const auto layout = computeAlbumModeLayout(size.width, size.height,
        true, compactSelection, visibility);

    render_.fill(layout.viewport, PanelColor::background);
    if(useCurated)(void)curated_.draw(target,layout.viewport);
    if(alloySceneReady_){
        Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
        if(SUCCEEDED(target->QueryInterface(IID_PPV_ARGS(context.GetAddressOf()))))
            alloySceneReady_=alloy_.drawToOwner(context.Get());
    }
    if (a4::style()==a4::Style::flow31 && artworkTransition_.ready()) {
        if (previousAmbientTexture_ && blend < 1.0f) {
            drawArtwork(previousAmbientTexture_.Get(), layout.viewport,
                1.0f - blend);
        }
        if (ambientTexture_) {
            drawArtwork(ambientTexture_.Get(), layout.viewport, blend);
        }
    }
    const auto visualButton=visualMenuRect(layout);
    const wchar_t* visualStatus=L"VISUAL · FLOW  ▾";
    if(useCurated) {
        const auto& d=curated_.diagnostics();
        if(render_.backend()!=SurfaceBackend::hardwareDxgi)visualStatus=L"CURATED · HARDWARE REQUIRED  ▾";
        else if(FAILED(d.lastHr))visualStatus=L"CURATED · ERROR / RETRY  ▾";
        else if(!d.ready)visualStatus=L"CURATED · PREPARING  ▾";
        else if(collection::selected())visualStatus=collection::title();
        else visualStatus=a4::style()==a4::Style::curatedCaustics?
            L"CAUSTICS · CURATED  ▾":L"WAVE FIELD · CURATED  ▾";
    } else if(useAlloy) {
        const auto& d=alloy_.diagnostics();
        if(d.latched) visualStatus=L"A4 · ERROR / RETRY  ▾";
        else if(!alloySceneReady_) visualStatus=FAILED(d.lastHr)?L"A4 · RETRY PENDING  ▾":L"A4 · PREPARING  ▾";
        else if(d.effectiveTier==2U) visualStatus=L"A4 · FIELD ONLY  ▾";
        else if(d.effectiveTier==1U) visualStatus=L"A4 · LIGHT  ▾";
        else visualStatus=L"A4 · FULL  ▾";
    }
    const auto toggleLabel = [](bool shown, const wchar_t* name) {
        return std::wstring(name) + (shown ? L"  \u2713" : L"");
    };
    if (bookletProgress <= .001f) {
        if (layout.tracksToggle.right > layout.tracksToggle.left)
            render_.text(toggleLabel(visibility.tracks, L"TRACKS"),
                layout.tracksToggle, PanelText::center,
                visibility.tracks ? PanelColor::accent : PanelColor::muted);
        if (layout.lyricsToggle.right > layout.lyricsToggle.left)
            render_.text(toggleLabel(visibility.lyrics, L"LYRICS"),
                layout.lyricsToggle, PanelText::center,
                visibility.lyrics ? PanelColor::accent : PanelColor::muted);
        if (layout.panelsMenu.right > layout.panelsMenu.left)
            render_.text(L"PANELS  \u25be", layout.panelsMenu,
                PanelText::center, PanelColor::secondary);
        render_.text(visualStatus,visualButton,
            PanelText::center,PanelColor::secondary);
    }
    if (!session || session->state == AlbumSessionState::empty ||
        session->sessionId == 0U) {
        render_.fill(layout.chooseAlbum, PanelColor::panel, 0.9f);
        render_.text(L"CHOOSE AN ALBUM", layout.chooseAlbum,
            PanelText::center, PanelColor::text);
        return finish();
    }
    if (session->state == AlbumSessionState::loading) {
        render_.fill(layout.chooseAlbum, PanelColor::panel, 0.9f);
        render_.text(L"LOADING ALBUM…", layout.chooseAlbum,
            PanelText::center, PanelColor::accent);
        return finish();
    }
    if (session->state == AlbumSessionState::error) {
        const auto message = session->error.empty()
            ? std::wstring_view(L"ALBUM UNAVAILABLE")
            : std::wstring_view(session->error);
        render_.fill(layout.chooseAlbum, PanelColor::panel, 0.9f);
        render_.text(message, layout.chooseAlbum,
            PanelText::center, PanelColor::amber);
        return finish();
    }

    const auto coverTarget = containRect(layout.cover,
        displayedArtwork_ ? displayedArtwork_->width : 1U, displayedArtwork_ ? displayedArtwork_->height : 1U);
    if (visibility.artwork && bookletProgress <= 0.001f && !alloySceneReady_) {
        if (!artworkTransition_.draw(
                target, coverTarget, motionMode(),artworkOpacity_) &&
                artworkTransition_.currentBitmap()) {
            drawArtwork(artworkTransition_.currentBitmap(), coverTarget, artworkOpacity_);
        }
        if (!artworkTransition_.ready()) {
            render_.fill(layout.cover, PanelColor::selected, 0.8f);
        }
    }
    const auto normalOpacity=1.0f-std::clamp(bookletProgress/.32f,0.0f,1.0f);
    if(normalOpacity>0.001f){
      const bool normalLayer=normalOpacity<.999f;
      if(normalLayer)target->PushLayer(D2D1::LayerParameters(
          D2D1::RectF(0,0,size.width,size.height),nullptr,
          D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),normalOpacity),nullptr);
    if (!layout.threeColumns && visibility.tracks && visibility.lyrics &&
            (layout.lyricsTab.right > layout.lyricsTab.left ||
             layout.tracksTab.right > layout.tracksTab.left)) {
        render_.text(L"LYRICS", layout.lyricsTab, PanelText::center,
            compactSelection == AlbumPanelKind::lyrics
                ? PanelColor::accent : PanelColor::muted);
        render_.text(L"TRACKS", layout.tracksTab, PanelText::center,
            compactSelection == AlbumPanelKind::tracks ||
                !hasLyrics
                ? PanelColor::accent : PanelColor::muted);
    }

    render_.text(session->albumArtist + L"  ·  " + session->album +
        (session->compositionExternallyModified?L"  ·  Managed playlist: Album playback restores its track list":L""),
        D2D1::RectF(layout.header.left, layout.header.top,
            std::max(layout.header.left,std::min(layout.header.right,visualButton.left-8.0f)), layout.header.bottom),
        PanelText::strong, PanelColor::bright);
    if (session->outsideSession) {
        render_.text(L"PLAYING OUTSIDE · FOLLOW CURRENT", layout.followCurrent,
            PanelText::center, PanelColor::amber);
    }
    if (layout.lyrics.right > layout.lyrics.left &&
        !hasLyrics) {
        const auto card = statusCard(layout.lyrics);
        if(!alloySceneReady_)render_.fill(card, PanelColor::panel, 0.42f);
        std::wstring status = L"NO RELIABLE MATCH";
        if (session && session->outsideSession)
            status = L"PLAYING OUTSIDE THIS ALBUM";
        else if (!representedLyrics ||
                representedLyrics->state == LyricsState::loadingLocal)
            status = L"CHECKING LOCAL LYRICS…";
        else if (representedLyrics->state == LyricsState::searching)
            status = L"SEARCHING LYRICS…";
        else if (representedLyrics->state == LyricsState::offline)
            status = L"OFFLINE · LOCAL/CACHE ONLY";
        else if (representedLyrics->state == LyricsState::rateLimited)
            status = L"PROVIDER RATE LIMITED";
        else if (representedLyrics->state == LyricsState::failed)
            status = L"LYRICS TEMPORARILY UNAVAILABLE";
        else if (representedLyrics->state == LyricsState::instrumentalReported)
            status = L"INSTRUMENTAL ACCORDING TO SOURCE";
        render_.text(status, card, PanelText::center,
            PanelColor::muted);
    }
    if (visibility.lyrics) drawLyrics(layout, playbackSeconds);
    if (visibility.tracks) drawTracklist(layout, *session, trackScroll);
      if(normalLayer)target->PopLayer();
    }
    if (bookletProgress > 0.001f) {
        const auto booklet = computeAlbumBookletLayout(
            size.width, size.height, visibility);
        const auto detailsOpacity = std::clamp(
            (bookletProgress - 0.40f) / 0.45f, 0.0f, 1.0f);
        if(!alloySceneReady_){
            render_.fill(booklet.albumDetails, PanelColor::panel,0.88f * detailsOpacity);
            render_.fill(booklet.writtenDetails, PanelColor::panel,0.88f * detailsOpacity);
        }
        const bool detailLayer=detailsOpacity<.999f;
        if(detailLayer)target->PushLayer(D2D1::LayerParameters(
            D2D1::RectF(0,0,size.width,size.height),nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),detailsOpacity),nullptr);
        if (visibility.tracks &&
                booklet.albumDetails.right > booklet.albumDetails.left) {
            const auto albumText = session->albumArtist + L"\n" + session->album +
                (session->year.empty() ? std::wstring{} : L"\n" + session->year);
            const auto albumViewport = D2D1::RectF(
                booklet.albumDetails.left + 12.0f,
                booklet.albumDetails.top + 12.0f,
                booklet.albumDetails.right - 12.0f,
                booklet.albumDetails.bottom - 12.0f);
            const auto albumDocument = textLayouts_.getOrCreate(
                writeFactory_.Get(), session->revision, 0x10000000U, 4U,
                albumText, bookletTitle_.Get(), width(albumViewport));
            if (albumDocument) textLayouts_.drawVisible(render_.target(),
                *albumDocument, albumViewport, render_.brush(PanelColor::bright),
                albumViewport.top);
        }
        if (visibility.lyrics && bookletFrame_) {
            const auto& frame = *bookletFrame_;
            const auto covered = frame.width == width(writtenViewport) &&
                frame.cache->hasVisible(*frame.document,
                bookletScrollDip_, bookletScrollDip_ + height(writtenViewport));
            auto viewport = writtenViewport;
            viewport.right = std::min(viewport.right, viewport.left + frame.width);
            if (!covered) viewport.bottom = std::min(viewport.bottom,
                viewport.top + frame.viewportHeight);
            (void)frame.cache->drawPrepared(target, *frame.document, viewport,
                render_.brush(PanelColor::text), viewport.top,
                covered ? bookletScrollDip_ : frame.scroll);
        }
        if (visibility.lyrics && textDemand_ && width(writtenViewport) > 0.f &&
            (!bookletFrame_ || bookletText_.stats().status == AsyncTextStatus::failed)) {
            auto status = writtenViewport;
            status.bottom = std::min(status.bottom, status.top + 30.f);
            render_.text(bookletText_.stats().status == AsyncTextStatus::failed
                ? L"Booklet text unavailable. Reopen to retry." : L"Preparing booklet…",
                status, PanelText::normal, PanelColor::muted);
        }
        if(detailLayer)target->PopLayer();
        if (booklet.tracksToggle.right > booklet.tracksToggle.left)
            render_.text(toggleLabel(visibility.tracks, L"TRACKS"),
                booklet.tracksToggle, PanelText::center,
                visibility.tracks ? PanelColor::accent : PanelColor::muted);
        if (booklet.lyricsToggle.right > booklet.lyricsToggle.left)
            render_.text(toggleLabel(visibility.lyrics, L"LYRICS"),
                booklet.lyricsToggle, PanelText::center,
                visibility.lyrics ? PanelColor::accent : PanelColor::muted);
        if (booklet.panelsMenu.right > booklet.panelsMenu.left)
            render_.text(L"PANELS  \u25be", booklet.panelsMenu,
                PanelText::center, PanelColor::secondary);
        render_.text(visualStatus, booklet.visualMenu,
            PanelText::center, PanelColor::secondary);
        render_.text(L"CLOSE", booklet.close,
            PanelText::center, PanelColor::text);
        if(visibility.artwork&&!alloySceneReady_){
        const auto morph = interpolateRect(coverTarget,
            containRect(booklet.artwork,
                artwork ? artwork->width : 1U,
                artwork ? artwork->height : 1U),
            bookletProgress);
        // The same fully opaque artwork surface moves continuously from its
        // cover slot into the booklet. Reusing the shared transition renderer
        // avoids the old blend-opacity hole that flashed black mid-morph.
        if (!artworkTransition_.draw(
                target, morph, motionMode()) &&
                artworkTransition_.currentBitmap()) {
            drawArtwork(artworkTransition_.currentBitmap(), morph, 1.0f);
        }
        if (!artworkTransition_.ready()) {
            render_.fill(morph, PanelColor::selected, 0.9f);
        }
        }
    }
    return finish();
}

void AlbumModeRenderer::releaseGraphics() noexcept
{
    initialUiPresented_=false;graphicsStaging_=false;
    curated_.invalidateBitmap();
    render_.reset();ambientEffect_.Reset();ambientTexture_.Reset();previousAmbientTexture_.Reset();
    resetAmbientGpuTiming();alloy_.reset();alloySceneReady_=false;
    bookletBackdrop_.Reset();bookletBackdropBlur_.Reset();bookletOpen_=false;
    artworkTransition_.discardDeviceResources();artworkTarget_=0;observedResourceEpoch_=0;
    lyricBrush_.Reset();
    // CPU artwork identity, lyrics scroll, material and audio phases survive.
}
void AlbumModeRenderer::retryVisualStyle() noexcept
{
    curated_.retry();
    alloy_.retry();ambientBudgetFallback_=false;ambientDegraded_=false;
    ambientTexture_.Reset();previousAmbientTexture_.Reset();alloySceneReady_=false;
}
bool AlbumModeRenderer::setOpticsPointer(float x,float y) noexcept
{
    (void)x;(void)y;
    // Spectral Alloy is music-reactive only. Mouse motion must not change the
    // card perspective, artwork optics or render cadence.
    return false;
}
D2D1_RECT_F AlbumModeRenderer::visualMenuRect(const AlbumModeLayout& layout) const noexcept
{
    return layout.visualMenu;
}

void AlbumModeRenderer::resize()
{
    render_.resize();
    // Viewport geometry changed even when the device epoch did not. Rebuild
    // only viewport-sized outputs; palette, signal and artwork stay valid.
    ambientTexture_.Reset();
    previousAmbientTexture_.Reset();
}
void AlbumModeRenderer::reset()
{
    suspendTextPreparation();
    bookletFrame_.reset();writtenText_.reset();writtenOwner_=writtenTextRevision_=0;
    plainLyricsFrame_.reset();plainLyricsBatch_.reset();pendingPlainWheel_=0.f;
    curated_.reset();curatedActive_=curatedSelected_=curatedDeferred_=false;
    alloy_.reset();alloySceneReady_=false;displayedArtwork_.reset();
    artworkGate_.cleared();artworkRequestIdentity_.clear();artworkOpacity_=0;lyricBrush_.Reset();
    render_.reset();
    ambientEffect_.Reset();
    ambientTexture_.Reset();
    previousAmbientTexture_.Reset();
    resetAmbientGpuTiming();
    bookletBackdrop_.Reset();
    bookletBackdropBlur_.Reset();
    bookletOpen_ = false;
    artworkTransition_.clear();
    artworkIdentity_.clear();
    artworkHash_ = 0;
    artworkWidth_ = 0;
    artworkHeight_ = 0;
    artworkTarget_ = 0;
    observedResourceEpoch_ = 0;
    lyricsPresentationRevision_ = 0;
    textLayouts_.clear();
    preparedLyrics_.clear();
    preparedLyricsRevision_ = 0;
    preparedLyricsWidth_ = 0.0f;
    preparedLyricsHeight_ = 0.0f;
    lyricVisuals_.clear();
    lyricScroll_.setBounds(0.0, 0.0);
    lyricScroll_.setPosition(0.0);
    lyricScrollDip_ = 0.0f;
    lyricMaximumScrollDip_ = 0.0f;
    pendingCueIndex_.reset();
    pendingCueStarted_ = {};
    manualLyricsUntil_ = {};
    ++lyricsGestureRevision_;
}

void AlbumModeRenderer::noteManualLyricsInteraction() noexcept
{
    manualLyricsUntil_ = std::chrono::steady_clock::now() +
        std::chrono::seconds(6);
}

void AlbumModeRenderer::scrollLyrics(float deltaDip) noexcept
{
    const auto plain = lyrics_ && lyrics_->timing != TimingStatus::catalogMatched &&
        lyrics_->timing != TimingStatus::audioVerified;
    if (plain && (!plainLyricsFrame_ || plainLyricsFrame_->generation != plainLyricsText_.stats().generation)) {
        if (std::isfinite(deltaDip)) pendingPlainWheel_ = std::clamp(
            pendingPlainWheel_ + deltaDip, -100000000.f, 100000000.f);
        noteManualLyricsInteraction();
        return;
    }
    lyricScroll_.setBounds(0.0, lyricMaximumScrollDip_);
    lyricScroll_.setResponseScale(
        motionMode() == MotionMode::off ? 0.0 :
        motionMode() == MotionMode::reduced ? 0.55 : 1.0);
    (void)lyricScroll_.addWheelImpulseAt(deltaDip, rendererClockSeconds());
    ++lyricsGestureRevision_;
    lyricScrollDip_ = static_cast<float>(lyricScroll_.position());
    noteManualLyricsInteraction();
}

void AlbumModeRenderer::scrollBooklet(float deltaDip) noexcept
{
    if (!std::isfinite(deltaDip)) return;
    // Before complete geometry arrives, preserve wheel intent rather than
    // clamping it to the previous document's (or a null document's) bounds.
    bookletScrollDip_ = std::max(0.0f, bookletScrollDip_ + deltaDip);
    if (bookletFrame_ && bookletFrame_->generation == bookletText_.stats().generation)
        bookletScrollDip_ = std::min(bookletScrollDip_, bookletMaxScrollDip_);
}

void AlbumModeRenderer::followLyrics() noexcept { manualLyricsUntil_ = {}; }

void AlbumModeRenderer::noteLyricsSeekIntent(
    const LyricsHitToken& token) noexcept
{
    if (!validateLyricsHit(token)) return;
    pendingCueIndex_ = token.cueIndex;
    pendingCueStarted_ = std::chrono::steady_clock::now();
    manualLyricsUntil_ = {};
}

std::optional<LyricsHitToken> AlbumModeRenderer::lyricsHitAt(
    const AlbumModeLayout& layout, float xDip, float yDip) const noexcept
{
    if (!lyrics_ ||
        (lyrics_->timing != TimingStatus::catalogMatched &&
         lyrics_->timing != TimingStatus::audioVerified) ||
        lyrics_->cues.size() != preparedLyrics_.size()) return std::nullopt;
    const auto viewport = D2D1::RectF(layout.lyrics.left + 14.0f,
        layout.lyrics.top + 8.0f, layout.lyrics.right - 14.0f,
        layout.lyrics.bottom - 8.0f);
    if (xDip < viewport.left || xDip > viewport.right ||
        yDip < viewport.top || yDip > viewport.bottom) return std::nullopt;
    const auto contentY = yDip - viewport.top + lyricScrollDip_;
    for (std::size_t index = 0; index < preparedLyrics_.size(); ++index) {
        const auto& line = preparedLyrics_[index];
        const auto envelope = index < lyricVisuals_.size()
            ? lyricVisuals_[index].value : 0.0f;
        const auto scale = motionMode() == MotionMode::normal
            ? 0.94f + 0.12f * envelope : 1.0f;
        const auto center = line.top + line.height * 0.5f;
        const auto top = center - line.height * scale * 0.5f;
        const auto bottom = center + line.height * scale * 0.5f;
        if (contentY < top || contentY >= bottom) continue;
        return LyricsHitToken{lyricsPresentationRevision_,
            lyricsGestureRevision_, index,
            std::max(0.0, (lyrics_->cues[index].startMs - 80) / 1000.0)};
    }
    return std::nullopt;
}

bool AlbumModeRenderer::validateLyricsHit(
    const LyricsHitToken& token) const noexcept
{
    return lyrics_ && token.presentationRevision == lyricsPresentationRevision_ &&
        token.gestureRevision == lyricsGestureRevision_ &&
        token.cueIndex < lyrics_->cues.size() &&
        (lyrics_->timing == TimingStatus::catalogMatched ||
         lyrics_->timing == TimingStatus::audioVerified);
}

bool AlbumModeRenderer::animationActive() const noexcept
{
    const auto duration = motionMode() == MotionMode::normal
        ? std::chrono::milliseconds(620)
        : motionMode() == MotionMode::reduced
            ? std::chrono::milliseconds(120)
            : std::chrono::milliseconds(0);
    return graphicsStaging_ || lyricsStaging_ || artworkGate_.needsFrame(rendererClockSeconds()) ||
        (a4::style()==a4::Style::spectralAlloy && alloy_.transitionActive(rendererClockSeconds())) ||
        (artworkVisible_&&artworkTransition_.active(motionMode())) ||
        (previousAmbientTexture_ &&
        std::chrono::steady_clock::now() - artworkChangedAt_ < duration) ||
        lyricScroll_.active() ||
        std::any_of(lyricVisuals_.begin(), lyricVisuals_.end(),
            [](const LyricVisualState& state) {
                if (state.started == std::chrono::steady_clock::time_point{})
                    return false;
                const auto elapsed = std::chrono::steady_clock::now() - state.started;
                return elapsed < std::chrono::milliseconds(220);
            }) ||
        (pendingCueIndex_ && std::chrono::steady_clock::now() -
            pendingCueStarted_ < std::chrono::milliseconds(750)) ||
        ThemeService::instance().transitionActive();
}

std::optional<std::uint32_t> AlbumModeRenderer::nextLyricsWakeMilliseconds(
    double playbackSeconds) const noexcept
{
    if (!lyrics_ || (lyrics_->timing != TimingStatus::catalogMatched &&
            lyrics_->timing != TimingStatus::audioVerified) ||
        playbackSeconds < 0.0) return std::nullopt;
    for (const auto& cue : lyrics_->cues) {
        const auto timestampSeconds = cue.startMs / 1000.0;
        if (timestampSeconds > playbackSeconds + 0.001) {
            const auto milliseconds = static_cast<std::uint64_t>(std::ceil(
                (timestampSeconds - playbackSeconds) * 1000.0));
            return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
                milliseconds, 1U, 0x7fffffffU));
        }
    }
    return std::nullopt;
}

} // namespace nekorox::component
