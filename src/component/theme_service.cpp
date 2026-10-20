#include "theme_service.h"
#include "a4/a4_material_cache.h"

#include "ui_design_system.h"
#include "ui_preferences.h"

#include <algorithm>
#include <cmath>

namespace nekorox::component {
namespace {

constexpr RgbColor kDefaultAccent{0.0f, 0.847f, 0.816f};
constexpr RgbColor kDefaultSecondary{0.66f, 0.91f, 0.90f};
constexpr RgbColor kDefaultHighlight{0.84f, 0.94f, 0.95f};
constexpr std::uint64_t kThemeProgressBuckets = 2047;

double qpcSeconds() noexcept
{
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    static const auto frequency = [] { LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value); return value.QuadPart; }();
    return frequency > 0
        ? static_cast<double>(counter.QuadPart) /
            static_cast<double>(frequency)
        : 0.0;
}

float smootherStep(float value) noexcept
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * value *
        (value * (value * 6.0f - 15.0f) + 10.0f);
}

std::uint64_t identityHash(
    std::wstring_view value,
    std::uint64_t artworkContentHash) noexcept
{
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    auto hash = offset;
    for (const auto character : value) {
        hash ^= static_cast<std::uint16_t>(character);
        hash *= prime;
    }
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash ^= (artworkContentHash >> shift) & 0xffU;
        hash *= prime;
    }
    return hash;
}

struct FallbackPalette {
    RgbColor accent;
    RgbColor secondary;
    RgbColor highlight;
};

FallbackPalette fallbackPalette() noexcept
{
    const auto accent = fallbackAccentColor();
    const auto defaultDistance =
        std::abs(accent.red - kDefaultAccent.red) +
        std::abs(accent.green - kDefaultAccent.green) +
        std::abs(accent.blue - kDefaultAccent.blue);
    if (defaultDistance < 0.01f) {
        return {{0.70f,0.72f,0.73f},{0.58f,0.61f,0.62f},{0.90f,0.92f,0.93f}};
    }
    return {
        accent,
        interpolateOklch(accent, {0.78f, 0.82f, 0.82f}, 0.62f),
        interpolateOklch(accent, {0.94f, 0.96f, 0.96f}, 0.82f),
    };
}

RgbaColorV1 rgba(const RgbColor& color) noexcept
{
    return {color.red, color.green, color.blue, 1.0f};
}

void postTransitionStarted(
    const std::vector<std::pair<HWND, UINT>>& subscribers) noexcept
{
    for (const auto& [window, message] : subscribers) {
        if (window != nullptr && message != 0 && IsWindow(window)) {
            PostMessageW(window, message, 0, 0);
        }
    }
}

} // namespace

ThemeService& ThemeService::instance()
{
    static ThemeService service;
    return service;
}

ThemeState ThemeService::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return sampleLocked(qpcSeconds());
}

ThemeState ThemeService::targetSnapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto target = state_;
    target.transitionProgress = 1.0f;
    target.transitionActive = false;
    return target;
}

ThemeSnapshotV2 ThemeService::contractSnapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto sampled = sampleLocked(qpcSeconds());
    ThemeSnapshotV2 result;
    result.generation = sampled.generation;
    result.artworkIdentityHash = artworkIdentityHash_;
    result.artworkGeneration = sampled.artworkGeneration;
    result.base = {0.039f, 0.043f, 0.043f, 1.0f};
    result.panel = {0.071f, 0.078f, 0.078f, 1.0f};
    result.text = {0.824f, 0.847f, 0.847f, 1.0f};
    result.accent = rgba(sampled.accent);
    result.secondary = rgba(sampled.secondary);
    result.highlight = rgba(sampled.highlight);
    result.minimumContrast = 4.5f;
    result.transitionProgress = sampled.transitionProgress;
    if (artworkDerived_) result.flags |= kThemeFlagArtworkDerived;
    return result;
}

bool ThemeService::transitionActive() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return sampleLocked(qpcSeconds()).transitionActive;
}

bool ThemeService::updateFromArtwork(
    const std::wstring& identity,
    const ArtPixels& pixels)
{
    if (identity.empty() || pixels.width == 0 || pixels.height == 0 ||
        pixels.bgra.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Artwork completion is allowed to resolve only the identity that
        // currently owns the palette. A late decoder result from another
        // view/track is inert.
        if (identity_ != identity) return false;
        if (resolved_ && identity_ == identity &&
                artworkContentHash_ == pixels.contentHash) {
            return false;
        }
    }
    // Production artwork carries a palette computed by ArtworkBroker's
    // background worker. Retain the synchronous fallback for synthetic/test
    // ArtPixels only; decoded covers never block the UI here.
    const auto palette = pixels.palettePrepared
        ? pixels.palette
        : extractArtPalette(
            pixels.bgra, pixels.width, pixels.height, pixels.width * 4U);

    const auto now = qpcSeconds();
    std::vector<std::pair<HWND, UINT>> subscribers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (identity_ != identity) return false;
        if (resolved_ && identity_ == identity &&
                artworkContentHash_ == pixels.contentHash) {
            return false;
        }
        identity_ = identity;
        artworkContentHash_ = pixels.contentHash;
        artworkIdentityHash_ = identityHash(identity_, artworkContentHash_);
        resolved_ = true;
        const bool quiet = pixels.material && pixels.material->valid &&
            (pixels.material->neutral || !palette.valid);
        artworkDerived_ = palette.valid || quiet;
        const auto fallback = fallbackPalette();
        const auto tint = quiet ? a4::quietAccent(*pixels.material) : a4::Vec3{};
        const RgbColor quietColor{tint.x,tint.y,tint.z};
        beginTransitionLocked(
            quiet ? quietColor : palette.valid ? palette.primary : fallback.accent,
            quiet ? RgbColor{tint.x*.84f,tint.y*.84f,tint.z*.84f} : palette.valid ? palette.secondary : fallback.secondary,
            quiet ? RgbColor{.90f,.92f,.93f} : palette.valid ? palette.highlight : fallback.highlight,
            now);
        subscribers = subscribers_;
    }
    postTransitionStarted(subscribers);
    return true;
}

void ThemeService::resetForTrack(const std::wstring& identity)
{
    const auto now = qpcSeconds();
    std::vector<std::pair<HWND, UINT>> subscribers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (identity_ == identity && !resolved_) return;

        // Hold the actually presented palette while artwork for the next track
        // is decoded. Falling back to cyan here created a visible
        // old/default/new sequence and detached theme motion from the artwork
        // reconstruction.
        const auto presented = sampleLocked(now);
        identity_ = identity;
        ++intentGeneration_;
        state_.intentGeneration = intentGeneration_;
        artworkContentHash_ = 0;
        artworkIdentityHash_ = identityHash(identity_, 0);
        resolved_ = false;
        artworkDerived_ = false;
        if (identity.empty()) {
            const auto fallback = fallbackPalette();
            beginTransitionLocked(
                fallback.accent,
                fallback.secondary,
                fallback.highlight,
                now);
            subscribers = subscribers_;
        } else {
            state_.accent = presented.accent;
            state_.secondary = presented.secondary;
            state_.highlight = presented.highlight;
            ++state_.generation;
            source_ = state_;
            transitionStartSeconds_ = now;
            transitionDurationSeconds_ = 0.0;
            ++sampleRevision_;
        }
    }
    postTransitionStarted(subscribers);
}

bool ThemeService::resolveWithoutArtwork(const std::wstring& identity)
{
    if (identity.empty()) return false;
    const auto now = qpcSeconds();
    std::vector<std::pair<HWND, UINT>> subscribers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (identity_ != identity) return false;
        if (resolved_ && !artworkDerived_ && identity_ == identity &&
            artworkContentHash_ == 0) {
            return false;
        }
        const auto fallback = fallbackPalette();
        identity_ = identity;
        artworkContentHash_ = 0;
        artworkIdentityHash_ = identityHash(identity_, 0);
        resolved_ = true;
        artworkDerived_ = false;
        beginTransitionLocked(
            fallback.accent,
            fallback.secondary,
            fallback.highlight,
            now);
        subscribers = subscribers_;
    }
    postTransitionStarted(subscribers);
    return true;
}

bool ThemeService::refreshFallbackPalette()
{
    const auto now = qpcSeconds();
    std::vector<std::pair<HWND, UINT>> subscribers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Do not flash the fallback while a new cover is still decoding, and
        // never replace a palette that genuinely came from artwork.
        if (artworkDerived_ || (!resolved_ && !identity_.empty())) return false;
        const auto fallback = fallbackPalette();
        beginTransitionLocked(
            fallback.accent,
            fallback.secondary,
            fallback.highlight,
            now);
        subscribers = subscribers_;
    }
    postTransitionStarted(subscribers);
    return true;
}

void ThemeService::subscribe(HWND window, UINT message)
{
    if (window == nullptr || message == 0) return;
    bool active = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = std::find_if(
            subscribers_.begin(), subscribers_.end(),
            [window](const auto& subscriber) {
                return subscriber.first == window;
            });
        if (found == subscribers_.end()) {
            subscribers_.emplace_back(window, message);
        } else {
            found->second = message;
        }
        active = sampleLocked(qpcSeconds()).transitionActive;
    }
    if (active && IsWindow(window)) PostMessageW(window, message, 0, 0);
}

void ThemeService::unsubscribe(HWND window) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::erase_if(subscribers_, [window](const auto& subscriber) {
        return subscriber.first == window;
    });
}

std::uint64_t ThemeService::nextSampleQpc() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto next = nextThemeSampleSeconds(qpcSeconds(),
        transitionStartSeconds_, transitionDurationSeconds_);
    if (!(next > 0.0)) return 0;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return static_cast<std::uint64_t>(std::ceil(
        next * static_cast<double>(frequency.QuadPart)));
}

ThemeState ThemeService::sampleLocked(double nowSeconds) const noexcept
{
    const auto step = themeSampleStep(nowSeconds, transitionStartSeconds_, transitionDurationSeconds_);
    if (sampledRevision_ == sampleRevision_ && sampledBucket_ == step.bucket) return sampled_;
    auto result = state_;
    const auto progress = step.progress;
    const auto eased = smootherStep(progress);
    if (progress <= 0.0f) {
        result.accent = source_.accent;
        result.secondary = source_.secondary;
        result.highlight = source_.highlight;
    } else if (progress < 1.0f) {
        result.accent = interpolateOklch(source_.accent, state_.accent, eased);
        result.secondary = interpolateOklch(source_.secondary, state_.secondary, eased);
        result.highlight = interpolateOklch(source_.highlight, state_.highlight, eased);
    }
    result.transitionProgress = progress;
    result.transitionActive = step.active;
    const auto bucket = static_cast<std::uint64_t>(
        std::llround(progress * kThemeProgressBuckets));
    result.generation =
        state_.generation * (kThemeProgressBuckets + 1U) +
        bucket;
    sampled_ = result;
    sampledBucket_ = step.bucket;
    sampledRevision_ = sampleRevision_;
    return result;
}

void ThemeService::beginTransitionLocked(
    RgbColor accent,
    RgbColor secondary,
    RgbColor highlight,
    double nowSeconds)
{
    source_ = sampleLocked(nowSeconds);
    const auto same = [](RgbColor a, RgbColor b) {
        return std::abs(a.red-b.red) <= 0.000001f && std::abs(a.green-b.green) <= 0.000001f &&
            std::abs(a.blue-b.blue) <= 0.000001f;
    };
    const auto colorChanged = !same(source_.accent, accent) || !same(source_.secondary, secondary) ||
        !same(source_.highlight, highlight);
    state_.accent = accent;
    state_.secondary = secondary;
    state_.highlight = highlight;
    state_.intentGeneration = intentGeneration_;
    ++state_.generation;
    ++state_.artworkGeneration;
    transitionStartSeconds_ = nowSeconds;
    transitionDurationSeconds_ = colorChanged ? static_cast<double>(
        UiDesignSystemV1::durationMs(
            MotionToken::artwork,
            motionMode())) / 1000.0 : 0.0;
    ++sampleRevision_;
}

} // namespace nekorox::component
