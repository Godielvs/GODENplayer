#include "nekorox/realtime_visual_model.h"
#include "nekorox/visual_sample_guard.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nekorox {
namespace {
float frameSample(const RealtimeFrameV1& frame, std::uint32_t index,
    std::uint16_t channel) noexcept
{
    if (index >= frame.frameCount || frame.channels == 0) return 0.0f;
    channel = std::min<std::uint16_t>(channel, frame.channels - 1U);
    const auto offset = static_cast<std::size_t>(index) * frame.channels + channel;
    return offset < frame.sampleCount && std::isfinite(frame.samples[offset])
        ? std::clamp(frame.samples[offset], -8.0f, 8.0f) : 0.0f;
}
}

void LargeScopeModel::reset() noexcept
{
    left_.fill(0.0f);
    right_.fill(0.0f);
    write_ = 0;
    count_ = 0;
    sequence_ = 0;
    streamGeneration_ = 0;
    sampleRate_ = 0;
    ++revision_;
}

void LargeScopeModel::configure(std::uint32_t windowMilliseconds,
    LargeScopeChannelMode mode, float gain) noexcept
{
    const auto boundedWindow = std::clamp(windowMilliseconds, 15U, 120U);
    const auto boundedGain = std::clamp(gain, 0.125f, 8.0f);
    if (windowMilliseconds_ == boundedWindow && mode_ == mode &&
        gain_ == boundedGain) return;
    windowMilliseconds_ = boundedWindow;
    mode_ = mode;
    gain_ = boundedGain;
    ++parameterRevision_;
    ++revision_;
}

bool LargeScopeModel::ingest(const RealtimeFrameV1& frame) noexcept
{
    if (frame.version != kRealtimeRingVersion || frame.sequence == 0 ||
        frame.frameCount == 0 || frame.channels == 0 || frame.sampleRate == 0 ||
        frame.sampleCount == 0 || frame.sequence == sequence_) return false;
    const auto discontinuity = sequence_ != 0 &&
        (frame.sequence != sequence_ + 1U ||
         frame.streamGeneration != streamGeneration_ ||
         frame.sampleRate != sampleRate_ ||
         (frame.flags & kRealtimeFrameFlagDiscontinuity) != 0);
    if (discontinuity) {
        count_ = 0;
        write_ = 0;
    }
    sampleRate_ = frame.sampleRate;
    streamGeneration_ = frame.streamGeneration;
    sequence_ = frame.sequence;
    for (std::uint32_t frameIndex = 0; frameIndex < frame.frameCount;
         ++frameIndex) {
        const auto slot = static_cast<std::size_t>(
            write_ % kLargeScopeHistoryFrames);
        const auto left = frameSample(frame, frameIndex, 0);
        const auto right = frame.channels > 1
            ? frameSample(frame, frameIndex, 1) : left;
        left_[slot] = left;
        right_[slot] = right;
        ++write_;
        count_ = std::min(kLargeScopeHistoryFrames, count_ + 1U);
    }
    ++revision_;
    return true;
}

float LargeScopeModel::sampleAt(std::uint64_t absolute,
    LargeScopeChannelMode mode) const noexcept
{
    const auto slot = static_cast<std::size_t>(
        absolute % kLargeScopeHistoryFrames);
    const auto left = left_[slot];
    const auto right = right_[slot];
    switch (mode) {
    case LargeScopeChannelMode::left: return left * gain_;
    case LargeScopeChannelMode::right: return right * gain_;
    case LargeScopeChannelMode::side: return (left - right) * 0.5f * gain_;
    case LargeScopeChannelMode::mid:
    default: return (left + right) * 0.5f * gain_;
    }
}

LargeScopeRenderState LargeScopeModel::render(std::size_t columns) const noexcept
{
    LargeScopeRenderState result;
    result.revision = revision_;
    result.parameterRevision = parameterRevision_;
    result.sourceSequence = sequence_;
    result.sampleRate = sampleRate_;
    result.windowMilliseconds = windowMilliseconds_;
    result.mode = mode_;
    result.gain = gain_;
    if (sampleRate_ == 0 || count_ < 2 || columns == 0) return result;
    columns = std::clamp<std::size_t>(columns, 1, kLargeScopeColumns);
    const auto requested = static_cast<std::size_t>(std::llround(
        static_cast<double>(sampleRate_) * windowMilliseconds_ / 1000.0));
    const auto window = std::clamp<std::size_t>(requested, 2, count_);
    auto start = write_ - window;

    // Stable rising zero crossing near the first fifth of the view. Search a
    // bounded preceding window; if no credible edge exists, newest PCM wins.
    const auto search = std::min<std::size_t>(count_ - window, window);
    const auto desired = start + window / 5U;
    std::uint64_t best = start;
    auto bestDistance = (std::numeric_limits<std::uint64_t>::max)();
    for (std::uint64_t absolute = start - search + 1U;
         absolute + 1U < write_; ++absolute) {
        const auto before = sampleAt(absolute - 1U, mode_);
        const auto after = sampleAt(absolute, mode_);
        if (!(before <= 0.0f && after > 0.0f && after - before > 0.002f)) continue;
        const auto candidateStart = absolute > window / 5U
            ? absolute - window / 5U : 0U;
        if (candidateStart + window > write_) continue;
        const auto distance = absolute > desired
            ? absolute - desired : desired - absolute;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = candidateStart;
            result.triggered = true;
        }
    }
    if (result.triggered) start = best;

    result.columns = columns;
    for (std::size_t column = 0; column < columns; ++column) {
        const auto begin = start + column * window / columns;
        const auto end = std::max(begin + 1U,
            start + (column + 1U) * window / columns);
        auto minimum = (std::numeric_limits<float>::max)();
        auto maximum = (std::numeric_limits<float>::lowest)();
        for (auto absolute = begin; absolute < end; ++absolute) {
            const auto sample = sampleAt(absolute, mode_);
            minimum = std::min(minimum, sample);
            maximum = std::max(maximum, sample);
        }
        result.minimum[column] = std::clamp(minimum, -1.0f, 1.0f);
        result.maximum[column] = std::clamp(maximum, -1.0f, 1.0f);
    }
    return result;
}

} // namespace nekorox
