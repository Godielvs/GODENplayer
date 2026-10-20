#include "nekorox/crt_xy_model.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nekorox {
namespace {

bool clipAxis(
    double origin,
    double delta,
    double minimum,
    double maximum,
    double& first,
    double& last) noexcept
{
    if (delta == 0.0) {
        return origin >= minimum && origin <= maximum;
    }

    auto entry = (minimum - origin) / delta;
    auto exit = (maximum - origin) / delta;
    if (entry > exit) std::swap(entry, exit);
    first = std::max(first, entry);
    last = std::min(last, exit);
    return first <= last;
}

std::size_t rasterIndex(
    double value,
    double minimum,
    double maximum,
    std::size_t count) noexcept
{
    const auto normalized = (value - minimum) / (maximum - minimum);
    if (normalized <= 0.0) return 0;
    if (normalized >= 1.0) return count - 1U;
    return std::min(
        count - 1U,
        static_cast<std::size_t>(
            normalized * static_cast<double>(count)));
}

bool validSegment(const CrtBeamSegment& segment) noexcept
{
    return std::isfinite(static_cast<double>(segment.x0)) &&
        std::isfinite(static_cast<double>(segment.y0)) &&
        std::isfinite(static_cast<double>(segment.x1)) &&
        std::isfinite(static_cast<double>(segment.y1)) &&
        std::isfinite(static_cast<double>(segment.charge)) &&
        std::isfinite(static_cast<double>(segment.ageSeconds)) &&
        segment.charge >= 0.0f;
}

} // namespace

void CrtXyModel::reset() noexcept
{
    stats_ = {};
    segmentCount_ = 0;
    previousX_ = 0.0f;
    previousY_ = 0.0f;
    hasPrevious_ = false;
}

std::span<const CrtBeamSegment> CrtXyModel::ingest(
    const RealtimeFrameV1& frame,
    bool discontinuity,
    double frameAgeSeconds) noexcept
{
    segmentCount_ = 0;
    const auto expectedSamples =
        static_cast<std::size_t>(frame.frameCount) * frame.channels;
    if (frame.version != kRealtimeRingVersion ||
        frame.sampleRate == 0 ||
        frame.channels < 2 ||
        frame.channels > kRealtimeMaxChannels ||
        frame.frameCount == 0 ||
        frame.frameCount > kRealtimeMaxFrames ||
        frame.sampleCount < expectedSamples ||
        frame.sampleCount > kRealtimeMaxSamples ||
        !std::isfinite(frameAgeSeconds) ||
        frameAgeSeconds < 0.0) {
        return {};
    }

    if (discontinuity && hasPrevious_) {
        hasPrevious_ = false;
        ++stats_.brokenPaths;
    }

    const auto charge =
        static_cast<float>(1.0 / static_cast<double>(frame.sampleRate));
    for (std::uint32_t index = 0; index < frame.frameCount; ++index) {
        const auto sampleIndex =
            static_cast<std::size_t>(index) * frame.channels;
        const auto x = frame.samples[sampleIndex];
        const auto y = frame.samples[sampleIndex + 1U];
        if (!std::isfinite(static_cast<double>(x)) ||
            !std::isfinite(static_cast<double>(y))) {
            if (hasPrevious_) {
                hasPrevious_ = false;
                ++stats_.brokenPaths;
            }
            continue;
        }
        if (hasPrevious_) {
            const auto sampleAge =
                frameAgeSeconds +
                (static_cast<double>(frame.frameCount - index) + 0.5) /
                    static_cast<double>(frame.sampleRate);
            segments_[segmentCount_++] = {
                previousX_,
                previousY_,
                x,
                y,
                charge,
                static_cast<float>(sampleAge),
            };
        }
        previousX_ = x;
        previousY_ = y;
        hasPrevious_ = true;
    }

    stats_.samples += frame.frameCount;
    stats_.segments += segmentCount_;
    return {segments_.data(), segmentCount_};
}

bool CrtXyReferenceRaster::valid() const noexcept
{
    if (width == 0 || height == 0 ||
        width > std::numeric_limits<std::size_t>::max() / height) {
        return false;
    }
    return energy.size() == width * height;
}

double CrtXyReferenceRaster::pixel(
    std::size_t x,
    std::size_t y) const noexcept
{
    if (!valid() || x >= width || y >= height) return 0.0;
    return energy[y * width + x];
}

double CrtXyReferenceRaster::totalEnergy() const noexcept
{
    double total = 0.0;
    for (const auto value : energy) total += value;
    return total;
}

bool validCrtXyRasterSpec(const CrtXyRasterSpec& spec) noexcept
{
    if (spec.width == 0 || spec.height == 0 ||
        spec.width > std::numeric_limits<std::size_t>::max() / spec.height) {
        return false;
    }
    const auto pixels = spec.width * spec.height;
    return pixels <=
            std::numeric_limits<std::size_t>::max() / sizeof(double) &&
        std::isfinite(spec.minimumX) &&
        std::isfinite(spec.maximumX) &&
        std::isfinite(spec.minimumY) &&
        std::isfinite(spec.maximumY) &&
        spec.minimumX < spec.maximumX &&
        spec.minimumY < spec.maximumY;
}

CrtXyReferenceRaster rasterizeCrtXyReference(
    std::span<const CrtBeamSegment> segments,
    const CrtXyRasterSpec& spec)
{
    CrtXyReferenceRaster raster;
    if (!validCrtXyRasterSpec(spec)) return raster;

    raster.width = spec.width;
    raster.height = spec.height;
    raster.energy.assign(spec.width * spec.height, 0.0);

    std::vector<double> cuts;
    cuts.reserve(spec.width + spec.height + 2U);
    for (const auto& segment : segments) {
        if (!validSegment(segment)) continue;

        const auto charge = static_cast<double>(segment.charge);
        raster.inputCharge += charge;
        if (charge == 0.0) continue;

        const auto x0 = static_cast<double>(segment.x0);
        const auto y0 = static_cast<double>(segment.y0);
        const auto deltaX = static_cast<double>(segment.x1) - x0;
        const auto deltaY = static_cast<double>(segment.y1) - y0;
        double first = 0.0;
        double last = 1.0;
        if (!clipAxis(
                x0, deltaX, spec.minimumX, spec.maximumX, first, last) ||
            !clipAxis(
                y0, deltaY, spec.minimumY, spec.maximumY, first, last) ||
            last <= first) {
            continue;
        }

        cuts.clear();
        cuts.push_back(first);
        cuts.push_back(last);
        if (deltaX != 0.0) {
            for (std::size_t column = 1; column < spec.width; ++column) {
                const auto amount = static_cast<double>(column) /
                    static_cast<double>(spec.width);
                const auto boundary = spec.minimumX +
                    (spec.maximumX - spec.minimumX) * amount;
                const auto crossing = (boundary - x0) / deltaX;
                if (crossing > first && crossing < last) {
                    cuts.push_back(crossing);
                }
            }
        }
        if (deltaY != 0.0) {
            for (std::size_t row = 1; row < spec.height; ++row) {
                const auto amount = static_cast<double>(row) /
                    static_cast<double>(spec.height);
                const auto boundary = spec.minimumY +
                    (spec.maximumY - spec.minimumY) * amount;
                const auto crossing = (boundary - y0) / deltaY;
                if (crossing > first && crossing < last) {
                    cuts.push_back(crossing);
                }
            }
        }

        std::sort(cuts.begin(), cuts.end());
        const auto uniqueEnd = std::unique(cuts.begin(), cuts.end());
        cuts.erase(uniqueEnd, cuts.end());
        for (std::size_t interval = 1; interval < cuts.size(); ++interval) {
            const auto begin = cuts[interval - 1U];
            const auto end = cuts[interval];
            if (end <= begin) continue;

            const auto midpoint = begin + (end - begin) * 0.5;
            const auto x = x0 + deltaX * midpoint;
            const auto y = y0 + deltaY * midpoint;
            const auto column = rasterIndex(
                x, spec.minimumX, spec.maximumX, spec.width);
            const auto row = rasterIndex(
                y, spec.minimumY, spec.maximumY, spec.height);
            raster.energy[row * spec.width + column] +=
                charge * (end - begin);
        }
        raster.visibleCharge += charge * (last - first);
    }
    return raster;
}

bool validCrtXyDecay(
    double deltaSeconds,
    double tauSeconds) noexcept
{
    return std::isfinite(deltaSeconds) &&
        std::isfinite(tauSeconds) &&
        deltaSeconds >= 0.0 &&
        tauSeconds > 0.0;
}

bool crtXyDecayFactor(
    double deltaSeconds,
    double tauSeconds,
    double& factor) noexcept
{
    if (!validCrtXyDecay(deltaSeconds, tauSeconds)) return false;
    factor = std::exp(-deltaSeconds / tauSeconds);
    return true;
}

bool decayCrtXyRaster(
    std::span<double> energy,
    double deltaSeconds,
    double tauSeconds) noexcept
{
    double factor = 0.0;
    if (!crtXyDecayFactor(deltaSeconds, tauSeconds, factor)) return false;
    for (auto& value : energy) value *= factor;
    return true;
}

} // namespace nekorox
