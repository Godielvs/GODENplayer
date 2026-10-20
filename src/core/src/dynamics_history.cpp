#include "nekorox/dynamics_history.h"
#include "nekorox/c11/c11_timed_bins.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <windows.h>

namespace nekorox {

DynamicsHistory::DynamicsHistory() noexcept
{
    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
        qpcFrequency_ = static_cast<std::uint64_t>(frequency.QuadPart);
}

std::optional<double> DynamicsReading::peakDb() const noexcept
{
    if (!(peak > 0.0) || !std::isfinite(peak)) return std::nullopt;
    return 20.0 * std::log10(peak);
}

std::optional<double> DynamicsReading::rmsDb() const noexcept
{
    if (!(power > 0.0) || !std::isfinite(power)) return std::nullopt;
    return 10.0 * std::log10(power);
}

std::optional<double> DynamicsReading::crestDb() const noexcept
{
    const auto peakValue = peakDb();
    const auto rmsValue = rmsDb();
    if (!complete || !peakValue || !rmsValue || *rmsValue < -60.0)
        return std::nullopt;
    return (std::max)(0.0, *peakValue - *rmsValue);
}

bool DynamicsHistory::ingest(const RealtimeFrameV1& frame,
    bool forceDiscontinuity) noexcept
{
    if (frame.version != kRealtimeRingVersion ||
        (frame.flags & kRealtimeFrameFlagPcm) == 0 ||
        frame.channels == 0 || frame.channels > 8 ||
        frame.sampleRate < 8000 || frame.sampleRate > 384000 ||
        frame.frameCount == 0 || frame.sampleCount == 0 ||
        frame.sampleCount > frame.samples.size() ||
        frame.sampleCount != frame.frameCount * frame.channels ||
        frame.playbackSerial == 0 || frame.recordingIdentityHash == 0) {
        return false;
    }
    if (frame.frameCount >
        std::numeric_limits<std::uint64_t>::max() - frame.firstFrameIndex) {
        return false;
    }
    const auto endFrame = frame.firstFrameIndex + frame.frameCount;
    const auto frameTicks = static_cast<std::uint64_t>(std::llround(
        static_cast<double>(frame.frameCount) * qpcFrequency_ /
        static_cast<double>(frame.sampleRate)));
    const auto firstQpc = frame.qpcTicks > frameTicks
        ? frame.qpcTicks - frameTicks : 0U;
    const bool admitted = valid_ &&
        playbackSerial_ == frame.playbackSerial &&
        recordingIdentityHash_ == frame.recordingIdentityHash;
    // Once a newer admission has been observed, delayed PCM belonging to an
    // older recording must never be appended to the new epoch.
    if (valid_ && frame.playbackSerial < playbackSerial_) return false;
    if (valid_ && frame.playbackSerial == playbackSerial_ &&
        frame.recordingIdentityHash != recordingIdentityHash_) return false;
    const bool sameSegment = admitted && channels_ == frame.channels &&
        sampleRate_ == frame.sampleRate &&
        streamGeneration_ == frame.streamGeneration;
    if (sameSegment && !forceDiscontinuity && endFrame <= nextFrame_) return false;
    bool published = false;
    if (!admitted) {
        beginEpoch(frame.channels, frame.sampleRate,
            frame.streamGeneration, frame.playbackSerial,
            frame.recordingIdentityHash, frame.firstFrameIndex, firstQpc);
    } else if (!sameSegment || frame.firstFrameIndex != nextFrame_) {
        const auto previousNextFrame = nextFrame_;
        const auto previousSampleRate = sampleRate_;
        const auto previousEndQpc = pending_.endQpc != 0
            ? pending_.endQpc
            : (count_ != 0 ? chronological(count_ - 1U).endQpc : 0U);
        published |= closePending(true, true);
        if (sameSegment && frame.firstFrameIndex > previousNextFrame) {
            const auto missing = frame.firstFrameIndex - previousNextFrame;
            const auto missingTicks = previousSampleRate != 0
                ? static_cast<std::uint64_t>(std::llround(
                    static_cast<double>(missing) * qpcFrequency_ /
                    previousSampleRate)) : 0U;
            published |= appendUnknown(previousNextFrame,
                frame.firstFrameIndex, previousEndQpc,
                previousEndQpc + missingTicks, previousSampleRate);
        } else if (!sameSegment && previousEndQpc != 0 &&
                firstQpc > previousEndQpc && previousSampleRate != 0) {
            const auto missing = static_cast<std::uint64_t>(std::llround(
                static_cast<double>(firstQpc - previousEndQpc) *
                previousSampleRate / qpcFrequency_));
            published |= appendUnknown(previousNextFrame,
                previousNextFrame + missing, previousEndQpc,
                firstQpc, previousSampleRate);
        }
        beginSegment(frame.channels, frame.sampleRate,
            frame.streamGeneration, frame.firstFrameIndex, firstQpc);
    } else if (forceDiscontinuity) {
        // A missed transport sequence with continuous PCM frame indices is a
        // diagnostic event, not proof that audio time disappeared.
        dataGap_ = true;
    }

    for (std::size_t audioFrame = 0; audioFrame < frame.frameCount;
            ++audioFrame) {
        std::uint32_t validScalars = 0;
        for (std::size_t channel = 0; channel < frame.channels; ++channel) {
            const auto value = static_cast<double>(
                frame.samples[audioFrame * frame.channels + channel]);
            if (!std::isfinite(value)) {
                ++pending_.sanitizedSamples;
                ++sanitizedSamples_;
                continue;
            }
            ++validScalars;
            pending_.sumSquares += value * value;
            pending_.samplePeak = (std::max)(
                pending_.samplePeak, std::abs(value));
            ++pending_.scalarCount;
        }
        if (validScalars == frame.channels) ++pending_.validFrames;
        else ++pending_.unknownFrames;
        ++nextFrame_;
        pending_.endFrame = nextFrame_;

        phase_ += 40U;
        if (phase_ >= sampleRate_) {
        if (firstQpc != 0) {
            pending_.endQpc = firstQpc + static_cast<std::uint64_t>(
                std::llround(static_cast<double>(audioFrame + 1U) *
                    qpcFrequency_ / frame.sampleRate));
        }
            phase_ -= sampleRate_;
            pending_.nominalFrames = pending_.validFrames +
                pending_.unknownFrames;
            pending_.flags = kDynamicsBinMeasured;
            appendClosed(pending_);
            const auto closedEndQpc = pending_.endQpc;
            pending_ = {};
            pending_.firstFrame = nextFrame_;
            pending_.segmentId = segmentId_;
            pending_.startQpc = closedEndQpc;
            pending_.sampleRate = sampleRate_;
            dataGap_ = false;
            published = true;
        }
    }
    if (pending_.validFrames != 0 || pending_.unknownFrames != 0) {
        pending_.endQpc = goden::c11::chunkEndTicks(
            firstQpc, frame.frameCount, frame.sampleRate, qpcFrequency_);
    }
    return published;
}

void DynamicsHistory::invalidate(bool dataGap) noexcept
{
    valid_ = false;
    count_ = 0;
    head_ = 0;
    phase_ = 0;
    pending_ = {};
    playbackSerial_ = 0;
    recordingIdentityHash_ = 0;
    sanitizedSamples_ = 0;
    unknownFrames_ = 0;
    partialBinsCommitted_ = 0;
    unknownMicroseconds_ = 0;
    dataGap_ = dataGap;
    ++epoch_;
    ++revision_;
}

const DynamicsBin& DynamicsHistory::chronological(
    std::size_t index) const noexcept
{
    return bins_[(head_ + capacity - count_ + index) % capacity];
}

DynamicsReading DynamicsHistory::reading(
    std::size_t requestedBins) const noexcept
{
    if (requestedBins == 0) return {};
    return readDynamicsTail(snapshot(0, 0));
}

DynamicsReading readDynamicsTail(const DynamicsSnapshot& snapshot,
    double minimumKnownSeconds) noexcept
{
    DynamicsReading result;
    if (!snapshot.valid || snapshot.count == 0 ||
        !(minimumKnownSeconds > 0.0)) return result;
    const auto segment = snapshot.bins[snapshot.count - 1U].segmentId;
    auto expectedEnd = snapshot.bins[snapshot.count - 1U].endFrame;
    double squares = 0.0;
    std::uint64_t scalars = 0;
    for (std::size_t offset = 0; offset < snapshot.count; ++offset) {
        const auto index = snapshot.count - 1U - offset;
        const auto& bin = snapshot.bins[index];
        if (bin.segmentId != segment || !bin.measured() ||
            bin.coverage() < 0.999 || bin.sampleRate == 0 ||
            bin.endFrame <= bin.firstFrame || bin.endFrame != expectedEnd) break;
        squares += bin.sumSquares;
        scalars += bin.scalarCount;
        result.peak = (std::max)(result.peak, bin.samplePeak);
        result.durationSeconds += static_cast<double>(
            bin.endFrame - bin.firstFrame) /
            static_cast<double>(bin.sampleRate);
        expectedEnd = bin.firstFrame;
        if (result.durationSeconds + 1e-9 >= minimumKnownSeconds) {
            result.complete = true;
            break;
        }
    }
    result.power = scalars != 0
        ? squares / static_cast<double>(scalars) : 0.0;
    return result;
}

DynamicsSnapshot DynamicsHistory::snapshot(
    std::uint64_t sourceSequence,
    std::uint64_t sourceQpc) const noexcept
{
    DynamicsSnapshot result;
    result.count = count_;
    for (std::size_t index = 0; index < count_; ++index)
        result.bins[index] = chronological(index);
    result.epoch = epoch_;
    result.revision = revision_;
    result.sourceSequence = sourceSequence;
    result.sourceQpc = sourceQpc;
    result.streamGeneration = streamGeneration_;
    result.playbackSerial = playbackSerial_;
    result.recordingIdentityHash = recordingIdentityHash_;
    result.sampleRate = sampleRate_;
    result.qpcFrequency = qpcFrequency_;
    result.valid = valid_;
    result.dataGap = dataGap_;
    result.sanitizedSamples = sanitizedSamples_;
    result.unknownFrames = unknownFrames_;
    result.partialBinsCommitted = partialBinsCommitted_;
    result.unknownMicroseconds = unknownMicroseconds_;
    return result;
}

void DynamicsHistory::beginEpoch(std::uint32_t channels,
    std::uint32_t sampleRate, std::uint64_t stream,
    std::uint64_t playbackSerial, std::uint64_t recordingIdentityHash,
    std::uint64_t firstFrame, std::uint64_t firstQpc) noexcept
{
    channels_ = channels;
    sampleRate_ = sampleRate;
    streamGeneration_ = stream;
    playbackSerial_ = playbackSerial;
    recordingIdentityHash_ = recordingIdentityHash;
    nextFrame_ = firstFrame;
    head_ = 0;
    count_ = 0;
    phase_ = 0;
    pending_ = {};
    pending_.firstFrame = firstFrame;
    pending_.startQpc = firstQpc;
    pending_.sampleRate = sampleRate;
    sanitizedSamples_ = 0;
    unknownFrames_ = 0;
    partialBinsCommitted_ = 0;
    unknownMicroseconds_ = 0;
    segmentId_ = 1;
    pending_.segmentId = segmentId_;
    valid_ = true;
    dataGap_ = false;
    ++epoch_;
    ++revision_;
}

void DynamicsHistory::beginSegment(std::uint32_t channels,
    std::uint32_t sampleRate, std::uint64_t stream,
    std::uint64_t firstFrame, std::uint64_t firstQpc) noexcept
{
    if (firstQpc != 0 && qpcFrequency_ != 0) {
        const auto span = qpcFrequency_ * 8U;
        trimBefore(firstQpc > span ? firstQpc - span : 0U);
    }
    channels_ = channels;
    sampleRate_ = sampleRate;
    streamGeneration_ = stream;
    nextFrame_ = firstFrame;
    phase_ = 0;
    pending_ = {};
    pending_.firstFrame = firstFrame;
    pending_.startQpc = firstQpc;
    pending_.sampleRate = sampleRate;
    ++segmentId_;
    if (segmentId_ == 0) ++segmentId_;
    pending_.segmentId = segmentId_;
    dataGap_ = true;
    ++revision_;
}

bool DynamicsHistory::closePending(bool partial, bool gapAfter) noexcept
{
    if (pending_.validFrames == 0 && pending_.unknownFrames == 0) return false;
    pending_.flags = pending_.scalarCount != 0
        ? kDynamicsBinMeasured : kDynamicsBinUnknown;
    if (partial) {
        ++partialBinsCommitted_;
        pending_.flags |= kDynamicsBinPartial;
    }
    pending_.nominalFrames = pending_.validFrames + pending_.unknownFrames;
    if (gapAfter) pending_.flags |= kDynamicsBinGapAfter;
    appendClosed(pending_);
    return true;
}

bool DynamicsHistory::appendUnknown(std::uint64_t firstFrame,
    std::uint64_t endFrame, std::uint64_t startQpc,
    std::uint64_t endQpc, std::uint32_t sampleRate) noexcept
{
    if (endFrame <= firstFrame) return false;
    DynamicsBin unknown;
    unknown.firstFrame = firstFrame;
    unknown.endFrame = endFrame;
    unknown.segmentId = segmentId_ + 1U;
    unknown.startQpc = startQpc;
    unknown.endQpc = endQpc;
    unknown.sampleRate = sampleRate;
    unknown.unknownFrames = static_cast<std::uint32_t>((std::min)(
        endFrame - firstFrame,
        static_cast<std::uint64_t>(
            (std::numeric_limits<std::uint32_t>::max)())));
    unknown.nominalFrames = unknown.unknownFrames;
    unknown.flags = kDynamicsBinUnknown |
        kDynamicsBinGapBefore | kDynamicsBinGapAfter;
    unknownFrames_ += unknown.unknownFrames;
    if (sampleRate != 0) {
        unknownMicroseconds_ += static_cast<std::uint64_t>(std::llround(
            static_cast<double>(unknown.unknownFrames) * 1000000.0 /
            sampleRate));
    }
    appendClosed(unknown);
    dataGap_ = true;
    return true;
}

void DynamicsHistory::appendClosed(const DynamicsBin& bin) noexcept
{
    bins_[head_] = bin;
    head_ = (head_ + 1U) % capacity;
    count_ = (std::min)(count_ + 1U, capacity);
    ++revision_;
    ++totalClosed_;
}

void DynamicsHistory::trimBefore(std::uint64_t minimumQpc) noexcept
{
    while (count_ != 0) {
        const auto oldest = (head_ + capacity - count_) % capacity;
        const auto end = bins_[oldest].endQpc;
        if (end == 0 || end >= minimumQpc) break;
        --count_;
    }
}

} // namespace nekorox
