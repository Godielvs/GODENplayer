#include "glyph_flow_service.h"

#include "ui_design_system.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace nekorox::component {
namespace {

constexpr std::uint32_t kGlyphCount = 6;
constexpr std::uint32_t kSamplesPerAxis = 4;

float srgbToLinear(std::uint8_t value) noexcept
{
    const auto normalized = static_cast<float>(value) / 255.0f;
    return normalized <= 0.04045f
        ? normalized / 12.92f
        : std::pow((normalized + 0.055f) / 1.055f, 2.4f);
}

std::uint8_t unpremultiply(std::uint8_t value, std::uint8_t alpha) noexcept
{
    if (alpha == 0) return 0;
    if (alpha == 255) return value;
    return static_cast<std::uint8_t>(std::min<unsigned>(
        255U,
        (static_cast<unsigned>(value) * 255U + alpha / 2U) / alpha));
}

::nekorox::GlyphFlowMotionModeV1 coreMotionMode(MotionMode mode) noexcept
{
    switch (mode) {
    case MotionMode::normal:
        return ::nekorox::GlyphFlowMotionModeV1::Normal;
    case MotionMode::reduced:
        return ::nekorox::GlyphFlowMotionModeV1::Reduced;
    case MotionMode::off:
        return ::nekorox::GlyphFlowMotionModeV1::Off;
    }
    return ::nekorox::GlyphFlowMotionModeV1::Off;
}

} // namespace

GlyphFlowService& GlyphFlowService::instance()
{
    static GlyphFlowService service;
    return service;
}

bool GlyphFlowService::observeArtwork(
    std::wstring_view identity,
    const ArtPixels& pixels) noexcept
{
    if (identity.empty() || pixels.width == 0 || pixels.height == 0) {
        return false;
    }
    const auto requiredBytes =
        static_cast<std::uint64_t>(pixels.width) * pixels.height * 4ULL;
    if (requiredBytes > pixels.bgra.size()) return false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pixels.contentHash != 0 && artworkContentHash_ == pixels.contentHash && flow_.configured()) {
            // A new track with the same raster changes ownership, not the
            // visual field. Preserve its presented state instead of restarting.
            identity_.assign(identity);
            if (presentedContentHash_ == pixels.contentHash) presentedIdentity_.assign(identity);
            return false;
        }
        if (identity_ == identity &&
                artworkContentHash_ == pixels.contentHash &&
                flow_.configured()) {
            return false;
        }
    }

    const auto luminance = deriveLuminance(pixels);
    std::vector<std::pair<HWND, UINT>> subscribers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (identity_ == identity &&
                artworkContentHash_ == pixels.contentHash &&
                flow_.configured()) {
            return false;
        }

        const auto nextGeneration = generation_ + 1U;
        const ::nekorox::GlyphFlowInputV1 input{
            ::nekorox::kGlyphFlowVersionV1,
            kGlyphFlowFieldWidth,
            kGlyphFlowFieldHeight,
            luminance,
            nextGeneration,
            stableIdentitySeed(identity) ^ pixels.contentHash,
            kGlyphCount,
        };
        const auto validation = flow_.configure(input);
        if (!validation.ok()) return false;

        identity_.assign(identity);
        artworkContentHash_ = pixels.contentHash;
        presentedIdentity_.clear();
        presentedContentHash_ = 0;
        generation_ = nextGeneration;
        presentationClock_.reset(nextGeneration);
        publishedStartSeconds_.store(0.0, std::memory_order_relaxed);
        publishedGeneration_.store(
            generation_, std::memory_order_release);
        subscribers = subscribers_;
    }

    for (const auto& [window, message] : subscribers) {
        if (window != nullptr && message != 0 && IsWindow(window)) {
            PostMessageW(window, message, 0, 0);
        }
    }
    return true;
}

bool GlyphFlowService::presented(std::uint64_t generation, bool accepted, double seconds) noexcept
{
    std::vector<std::pair<HWND, UINT>> subscribers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = seconds >= 0.0 ? seconds : qpcSeconds();
        if (!flow_.configured() || !presentationClock_.accept(generation, accepted, now)) return false;
        presentedIdentity_ = identity_;
        presentedContentHash_ = artworkContentHash_;
        publishedStartSeconds_.store(presentationClock_.startSeconds(), std::memory_order_relaxed);
        publishedGeneration_.store(
            generation_, std::memory_order_release);
        subscribers = subscribers_;
    }

    for (const auto& [window, message] : subscribers) {
        if (window != nullptr && message != 0 && IsWindow(window)) {
            PostMessageW(window, message, 0, 0);
        }
    }
    return true;
}

bool GlyphFlowService::sample(
    MotionMode mode,
    std::span<::nekorox::GlyphFlowCellV1> output,
    GlyphFlowFrame& frame) const noexcept
{
    frame = {};
    std::lock_guard<std::mutex> lock(mutex_);
    if (!flow_.configured()) return false;

    frame.generation = generation_;
    frame.width = flow_.width();
    frame.height = flow_.height();
    const auto duration = durationMilliseconds(mode);
    if (duration <= 0.0) {
        frame.progress = 1.0f;
        return false;
    }

    const auto elapsed = presentationClock_.elapsedMilliseconds(qpcSeconds());
    frame.progress = static_cast<float>(std::clamp(
        elapsed / duration, 0.0, 1.0));
    if (elapsed >= duration) return false;

    const auto coreElapsed =
        frame.progress * ::nekorox::kGlyphFlowDurationMillisecondsV1;
    const auto result = flow_.sample(
        coreMotionMode(mode), coreElapsed, output);
    frame.active = result.ok() && !result.complete;
    return frame.active;
}

bool GlyphFlowService::active(MotionMode mode) const noexcept
{
    return transition(mode).active;
}

GlyphFlowFrame GlyphFlowService::transition(MotionMode mode) const noexcept
{
    GlyphFlowFrame frame{};
    frame.generation = publishedGeneration_.load(std::memory_order_acquire);
    if (frame.generation == 0) return frame;
    const auto duration = durationMilliseconds(mode);
    const auto start =
        publishedStartSeconds_.load(std::memory_order_relaxed);
    if (duration <= 0.0) {
        frame.progress = 1.0f;
        return frame;
    }
    const auto elapsed = start > 0.0 ? std::max(0.0, (qpcSeconds() - start) * 1000.0) : 0.0;
    frame.progress = static_cast<float>(std::clamp(
        elapsed / duration, 0.0, 1.0));
    frame.active = elapsed < duration;
    return frame;
}

std::uint64_t GlyphFlowService::generation() const noexcept
{
    return publishedGeneration_.load(std::memory_order_acquire);
}

void GlyphFlowService::subscribe(HWND window, UINT message)
{
    if (window == nullptr || message == 0) return;
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
}

void GlyphFlowService::unsubscribe(HWND window) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::erase_if(subscribers_, [window](const auto& subscriber) {
        return subscriber.first == window;
    });
}

std::array<float, kGlyphFlowFieldCellCount>
GlyphFlowService::deriveLuminance(const ArtPixels& pixels) noexcept
{
    std::array<float, kGlyphFlowFieldCellCount> field{};
    for (std::uint32_t row = 0; row < kGlyphFlowFieldHeight; ++row) {
        const auto top = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(row) * pixels.height) /
            kGlyphFlowFieldHeight);
        const auto bottom = std::max(
            top + 1U,
            static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(row + 1U) * pixels.height) /
                kGlyphFlowFieldHeight));
        for (std::uint32_t column = 0;
             column < kGlyphFlowFieldWidth;
             ++column) {
            const auto left = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(column) * pixels.width) /
                kGlyphFlowFieldWidth);
            const auto right = std::max(
                left + 1U,
                static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(column + 1U) * pixels.width) /
                    kGlyphFlowFieldWidth));

            float total = 0.0f;
            for (std::uint32_t sampleY = 0;
                 sampleY < kSamplesPerAxis;
                 ++sampleY) {
                const auto y = std::min(
                    pixels.height - 1U,
                    top + static_cast<std::uint32_t>(
                        (static_cast<std::uint64_t>(sampleY * 2U + 1U) *
                            (bottom - top)) /
                        (kSamplesPerAxis * 2U)));
                for (std::uint32_t sampleX = 0;
                     sampleX < kSamplesPerAxis;
                     ++sampleX) {
                    const auto x = std::min(
                        pixels.width - 1U,
                        left + static_cast<std::uint32_t>(
                            (static_cast<std::uint64_t>(sampleX * 2U + 1U) *
                                (right - left)) /
                            (kSamplesPerAxis * 2U)));
                    const auto offset =
                        (static_cast<std::size_t>(y) * pixels.width + x) * 4U;
                    const auto alpha = pixels.bgra[offset + 3U];
                    const auto blue =
                        unpremultiply(pixels.bgra[offset], alpha);
                    const auto green =
                        unpremultiply(pixels.bgra[offset + 1U], alpha);
                    const auto red =
                        unpremultiply(pixels.bgra[offset + 2U], alpha);
                    total +=
                        0.2126f * srgbToLinear(red) +
                        0.7152f * srgbToLinear(green) +
                        0.0722f * srgbToLinear(blue);
                }
            }
            field[static_cast<std::size_t>(row) *
                kGlyphFlowFieldWidth + column] =
                total /
                static_cast<float>(kSamplesPerAxis * kSamplesPerAxis);
        }
    }

    auto sorted = field;
    std::sort(sorted.begin(), sorted.end());
    const auto low = sorted[sorted.size() / 10U];
    const auto high = sorted[sorted.size() - sorted.size() / 10U - 1U];
    const auto span = high - low;
    if (span > 1.0e-5f) {
        for (auto& value : field) {
            const auto normalized = std::clamp(
                (value - low) / span, 0.0f, 1.0f);
            value = std::sqrt(normalized);
        }
    } else {
        for (auto& value : field) {
            value = std::clamp(std::sqrt(value), 0.0f, 1.0f);
        }
    }
    return field;
}

std::uint64_t GlyphFlowService::stableIdentitySeed(
    std::wstring_view identity) noexcept
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto character : identity) {
        const auto value = static_cast<std::uint32_t>(character);
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= (value >> shift) & 0xffU;
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

double GlyphFlowService::qpcSeconds() noexcept
{
    static const double inverseFrequency = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return frequency.QuadPart > 0
            ? 1.0 / static_cast<double>(frequency.QuadPart)
            : 0.0;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) * inverseFrequency;
}

double GlyphFlowService::durationMilliseconds(MotionMode mode) noexcept
{
    return static_cast<double>(UiDesignSystemV1::durationMs(
        MotionToken::artwork, mode));
}

} // namespace nekorox::component
