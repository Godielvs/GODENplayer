#include "nekorox/realtime_visual_model.h"
#include "nekorox/visual_sample_guard.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace nekorox {
namespace {

float sampleAt(
    const RealtimeFrameV1& frame,
    std::uint32_t frameIndex,
    std::uint16_t channel) noexcept
{
    if (frame.frameCount == 0 || frame.channels == 0) return 0.0f;
    frameIndex = std::min(frameIndex, frame.frameCount - 1U);
    channel = std::min<std::uint16_t>(channel, frame.channels - 1U);
    const auto index = static_cast<std::size_t>(frameIndex) * frame.channels + channel;
    return index < frame.sampleCount ? frame.samples[index] : 0.0f;
}

float monoAt(const RealtimeFrameV1& frame, std::uint32_t frameIndex) noexcept
{
    if (frame.channels == 0) return 0.0f;
    const auto left = sampleAt(frame, frameIndex, 0);
    const auto right = frame.channels > 1 ? sampleAt(frame, frameIndex, 1) : left;
    return (left + right) * 0.5f;
}

float clampUnit(float value) noexcept
{
    return std::clamp(value, 0.0f, 1.0f);
}

float amplitudeToFixedScale(
    float amplitude,
    float floorDecibels,
    float ceilingDecibels) noexcept
{
    const auto decibels = 20.0f * std::log10(std::max(1e-7f, amplitude));
    return clampUnit(
        (decibels - floorDecibels) /
        std::max(1.0f, ceilingDecibels - floorDecibels));
}

struct SpectrumTables final {
    std::array<float, kAnalysisFftSize> window{};
    std::array<std::uint16_t, kAnalysisFftSize> bitReverse{};
    std::array<std::complex<float>, kAnalysisFftSize / 2U> twiddles{};
    double windowSum = 0.0;

    SpectrumTables() noexcept
    {
        constexpr std::size_t fftBits = 12;
        for (std::size_t sample = 0; sample < kAnalysisFftSize; ++sample) {
            window[sample] = static_cast<float>(0.5 - 0.5 * std::cos(
                2.0 * std::numbers::pi * static_cast<double>(sample) /
                static_cast<double>(kAnalysisFftSize - 1U)));
            windowSum += window[sample];

            auto value = sample;
            std::size_t reversed = 0;
            for (std::size_t bit = 0; bit < fftBits; ++bit) {
                reversed = (reversed << 1U) | (value & 1U);
                value >>= 1U;
            }
            bitReverse[sample] = static_cast<std::uint16_t>(reversed);
        }
        for (std::size_t index = 0; index < twiddles.size(); ++index) {
            const auto phase = static_cast<float>(
                -2.0 * std::numbers::pi * static_cast<double>(index) /
                static_cast<double>(kAnalysisFftSize));
            twiddles[index] = {std::cos(phase), std::sin(phase)};
        }
    }
};

const SpectrumTables& spectrumTables() noexcept
{
    static const SpectrumTables tables;
    return tables;
}

// FL Studio 21's short Output Monitor derives its timebase from A4=440 Hz,
// shifted down 33 semitones. That is C2, or one 15.29 ms window.
inline constexpr double kCompactScopeReferenceHz = 65.40639132514966;
inline constexpr double kCompactScopePhaseSearchWindows = 3.5;
inline constexpr double kCompactScopeLockedGraceWindows = 1.25;
inline constexpr float kCompactScopePhaseLockThreshold = 0.82f;
inline constexpr std::size_t kCompactScopeCorrelationPoints = 64;
inline constexpr std::size_t kCompactScopeSearchPoints = 193;
inline constexpr std::size_t kCompactScopeRefinePoints = 17;
inline constexpr double kCompactScopeLockedPresentationTimeConstant = 0.0045;
inline constexpr double kCompactScopeUnlockedPresentationTimeConstant = 0.018;
inline constexpr float kCompactScopePresentationEpsilon = 0.0005f;

} // namespace

MusicMapProfile buildMusicMapProfile(
    std::span<const AnalysisWaveformBinV1> waveform) noexcept
{
    MusicMapProfile raw{};
    if (waveform.empty()) return raw;

    for (std::size_t index = 0; index < raw.size(); ++index) {
        const auto begin = std::min(
            waveform.size() - 1U,
            index * waveform.size() / raw.size());
        const auto end = std::clamp(
            (index + 1U) * waveform.size() / raw.size() + 1U,
            begin + 1U,
            waveform.size());

        double squared = 0.0;
        float peak = 0.0f;
        for (auto source = begin; source < end; ++source) {
            const auto rms = std::clamp(waveform[source].rms, 0.0f, 1.0f);
            squared += static_cast<double>(rms) * rms;
            peak = std::max(
                peak,
                std::clamp(waveform[source].peak, 0.0f, 1.0f));
        }
        const auto count = std::max<std::size_t>(1U, end - begin);
        const auto rms = static_cast<float>(std::sqrt(
            squared / static_cast<double>(count)));
        const auto crestDecibels = 20.0f * std::log10(
            std::max(1.0f, peak / std::max(1e-7f, rms)));

        raw[index].energy = amplitudeToFixedScale(rms, -60.0f, -6.0f);
        raw[index].width = amplitudeToFixedScale(peak, -54.0f, 0.0f);
        raw[index].brightness = clampUnit((crestDecibels - 3.0f) / 15.0f);
        raw[index].observations = static_cast<std::uint32_t>(count);
    }

    for (std::size_t index = 1; index < raw.size(); ++index) {
        const auto levelChange = std::abs(
            raw[index].energy - raw[index - 1U].energy);
        const auto peakAttack = std::max(
            0.0f,
            raw[index].width - raw[index - 1U].width);
        raw[index].pulse = clampUnit(levelChange * 2.4f + peakAttack * 1.7f);
    }

    MusicMapProfile profile{};
    constexpr std::array<float, 5> kernel{1.0f, 2.0f, 3.0f, 2.0f, 1.0f};
    for (std::size_t index = 0; index < profile.size(); ++index) {
        float energy = 0.0f;
        float peak = 0.0f;
        float crest = 0.0f;
        float change = 0.0f;
        float weights = 0.0f;
        for (int offset = -2; offset <= 2; ++offset) {
            const auto candidate = static_cast<std::ptrdiff_t>(index) + offset;
            if (candidate < 0 ||
                candidate >= static_cast<std::ptrdiff_t>(raw.size())) {
                continue;
            }
            const auto weight = kernel[static_cast<std::size_t>(offset + 2)];
            const auto& source = raw[static_cast<std::size_t>(candidate)];
            energy += source.energy * weight;
            peak += source.width * weight;
            crest += source.brightness * weight;
            change = std::max(change, source.pulse);
            weights += weight;
        }
        profile[index].energy = weights > 0.0f ? energy / weights : 0.0f;
        profile[index].width = weights > 0.0f ? peak / weights : 0.0f;
        profile[index].brightness = weights > 0.0f ? crest / weights : 0.0f;
        profile[index].pulse = change;
        profile[index].observations = raw[index].observations;
    }
    return profile;
}

void RealtimeVisualModel::reset(double durationSeconds) noexcept
{
    snapshot_ = {};
    scopeModel_.reset();
    largeScopeModel_.reset();
    previousSpectrum_.fill(0.0f);
    previousSpectrumFft_.fill(0.0f);
    spectrumPower_.fill(0.0);
    spectrumHistory_.fill(0.0f);
    spectrumHistoryLeft_.fill(0.0f);
    spectrumHistoryRight_.fill(0.0f);
    spectrumFft_.fill({0.0f, 0.0f});
    spectrumHistoryWrite_ = 0;
    spectrumHistoryCount_ = 0;
    spectrogram_ = {};
    musicMap_ = {};
    energyHistory_.fill(0.0f);
    widthHistory_.fill(0.0f);
    brightnessHistory_.fill(0.0f);
    spectrogramHead_ = 0;
    spectrogramRevision_ = 0;
    historyHead_ = 0;
    durationSeconds_ = std::max(0.0, durationSeconds);
    featureSampleAccumulator_ = 0;
    vectorPeakEwma_ = 0.15f;
    previousRms_ = 0.0f;
    previousSpectralCentroidHz_ = 0.0f;
    previousSpectralRolloff85Hz_ = 0.0f;
    previousSpectralConfidence_ = 0.0f;
}

bool RealtimeVisualModel::ingest(
    const RealtimeFrameV1& frame,
    bool buildLegacyScope) noexcept
{
    const auto firstFrame = snapshot_.sequence == 0;
    if (frame.version != kRealtimeRingVersion || frame.sequence == 0 ||
        frame.sequence == snapshot_.sequence || frame.frameCount == 0 ||
        frame.channels == 0 || frame.sampleRate == 0 || frame.sampleCount == 0) {
        return false;
    }
    snapshot_.sequence = frame.sequence;
    snapshot_.playbackSeconds = frame.playbackSeconds;
    snapshot_.sampleRate = frame.sampleRate;
    // Listen Grid L1 consumes the authoritative CompactScope publication.
    // LargeScope remains separately testable, but is not fed by the normal
    // realtime Listen path.
    snapshot_.largeScope = {};

    double sumSquares = 0.0;
    double leftSquares = 0.0;
    double rightSquares = 0.0;
    double cross = 0.0;
    float peak = 0.0f;
    for (std::uint32_t index = 0; index < frame.frameCount; ++index) {
        const auto left = sampleAt(frame, index, 0);
        const auto right = frame.channels > 1 ? sampleAt(frame, index, 1) : left;
        const auto mono = (left + right) * 0.5f;
        spectrumHistory_[spectrumHistoryWrite_] = mono;
        spectrumHistoryLeft_[spectrumHistoryWrite_] = left;
        spectrumHistoryRight_[spectrumHistoryWrite_] = right;
        spectrumHistoryWrite_ =
            (spectrumHistoryWrite_ + 1U) % kAnalysisFftSize;
        spectrumHistoryCount_ = std::min(
            kAnalysisFftSize, spectrumHistoryCount_ + 1U);
        sumSquares += static_cast<double>(mono) * mono;
        leftSquares += static_cast<double>(left) * left;
        rightSquares += static_cast<double>(right) * right;
        cross += static_cast<double>(left) * right;
        peak = std::max(peak, std::max(std::abs(left), std::abs(right)));
    }
    snapshot_.rms = static_cast<float>(std::sqrt(
        sumSquares / std::max<std::uint32_t>(1, frame.frameCount)));
    snapshot_.peak = peak;
    const auto denominator = std::sqrt(leftSquares * rightSquares);
    snapshot_.correlation = denominator > 1e-12
        ? static_cast<float>(std::clamp(cross / denominator, -1.0, 1.0))
        : 1.0f;
    snapshot_.width = clampUnit((1.0f - snapshot_.correlation) * 0.5f);
    snapshot_.pulse = clampUnit(
        std::max(0.0f, snapshot_.rms - previousRms_) * 7.5f);
    previousRms_ = previousRms_ * 0.72f + snapshot_.rms * 0.28f;

    if (buildLegacyScope) updateScope(frame);
    updateVectorscope(frame);
    featureSampleAccumulator_ += frame.frameCount;
    const auto featureInterval = std::max<std::uint32_t>(
        1, frame.sampleRate / 60U);
    if (firstFrame || featureSampleAccumulator_ >= featureInterval) {
        featureSampleAccumulator_ %= featureInterval;
        updateSpectrum();
        updateHistories();
    }
    return true;
}

void CompactScopeModel::reset() noexcept
{
    resetSignal();
    sequence_ = 0;
    renderRevision_ = 0;
}

void CompactScopeModel::resetSignal() noexcept
{
    history_.fill(0.0f);
    raw_.fill(0.0f);
    phaseReference_.fill(0.0f);
    phaseCorrelationReference_.fill(0.0f);
    samples_.fill(0.0f);
    minimum_.fill(0.0f);
    maximum_.fill(0.0f);
    historyWrite_ = 0;
    historyCount_ = 0;
    sampleRate_ = 0;
    displayWindowSamples_ = 0;
    renderedStart_ = 0.0;
    phaseReferenceStart_ = 0.0;
    periodSamples_ = 0.0f;
    periodHz_ = 0.0f;
    lockConfidence_ = 0.0f;
    phaseCorrelationEnergy_ = 0.0;
    hasRenderedWindow_ = false;
    hasPhaseReference_ = false;
    locked_ = false;
}

void CompactScopeModel::append(const RealtimeFrameV1& frame) noexcept
{
    sampleRate_ = frame.sampleRate;
    for (std::uint32_t index = 0; index < frame.frameCount; ++index) {
        const auto mono = monoAt(frame, index);
        const auto slot = static_cast<std::size_t>(
            historyWrite_ % kCompactScopeHistorySamples);
        history_[slot] = mono;
        ++historyWrite_;
        historyCount_ = std::min(
            historyCount_ + 1U, kCompactScopeHistorySamples);
    }
}

float CompactScopeModel::rawAt(std::uint64_t absoluteIndex) const noexcept
{
    return history_[static_cast<std::size_t>(
        absoluteIndex % kCompactScopeHistorySamples)];
}

float CompactScopeModel::interpolatedRaw(double absoluteIndex) const noexcept
{
    if (historyCount_ == 0) return 0.0f;
    const auto minimum = historyWrite_ - historyCount_;
    const auto maximum = historyWrite_ - 1U;
    const auto position = std::clamp(
        absoluteIndex,
        static_cast<double>(minimum),
        static_cast<double>(maximum));
    const auto first = static_cast<std::uint64_t>(std::floor(position));
    const auto second = std::min(first + 1U, maximum);
    const auto fraction = static_cast<float>(position - std::floor(position));
    return rawAt(first) + (rawAt(second) - rawAt(first)) * fraction;
}

float CompactScopeModel::windowCorrelation(
    double start,
    double length) const noexcept
{
    if (!hasPhaseReference_ || historyCount_ < 2U || length < 2.0) {
        return -1.0f;
    }

    double candidateSum = 0.0;
    double candidateSquares = 0.0;
    double covariance = 0.0;
    for (std::size_t point = 0; point < kCompactScopeCorrelationPoints; ++point) {
        const auto amount = static_cast<double>(point) /
            static_cast<double>(kCompactScopeCorrelationPoints - 1U);
        const auto candidate = static_cast<double>(
            interpolatedRaw(start + amount * (length - 1.0)));
        candidateSum += candidate;
        candidateSquares += candidate * candidate;
        covariance += static_cast<double>(
            phaseCorrelationReference_[point]) * candidate;
    }
    const auto count = static_cast<double>(kCompactScopeCorrelationPoints);
    const auto candidateEnergy = std::max(
        0.0, candidateSquares - candidateSum * candidateSum / count);
    const auto denominator = std::sqrt(
        phaseCorrelationEnergy_ * candidateEnergy);
    if (denominator < 1e-8) return -1.0f;
    return static_cast<float>(std::clamp(
        covariance / denominator, -1.0, 1.0));
}

void CompactScopeModel::updatePhaseReference() noexcept
{
    phaseReference_ = raw_;
    double mean = 0.0;
    for (std::size_t point = 0; point < kCompactScopeCorrelationPoints; ++point) {
        const auto source = point * (kScopeSamples - 1U) /
            (kCompactScopeCorrelationPoints - 1U);
        mean += phaseReference_[source];
    }
    mean /= static_cast<double>(kCompactScopeCorrelationPoints);
    phaseCorrelationEnergy_ = 0.0;
    for (std::size_t point = 0; point < kCompactScopeCorrelationPoints; ++point) {
        const auto source = point * (kScopeSamples - 1U) /
            (kCompactScopeCorrelationPoints - 1U);
        const auto centered = static_cast<float>(
            static_cast<double>(phaseReference_[source]) - mean);
        phaseCorrelationReference_[point] = centered;
        phaseCorrelationEnergy_ += static_cast<double>(centered) * centered;
    }
}

bool CompactScopeModel::choosePhaseCoherentStart(
    double newestStart,
    double length,
    double& selectedStart,
    float& confidence) const noexcept
{
    selectedStart = newestStart;
    confidence = 0.0f;
    if (!hasRenderedWindow_) return true;

    const auto oldest = static_cast<double>(historyWrite_ - historyCount_);
    const auto minimumAdvance = length * 0.18;
    const auto earliest = std::max({
        oldest,
        renderedStart_ + minimumAdvance,
        newestStart - length * kCompactScopePhaseSearchWindows,
    });
    if (earliest > newestStart) return false;

    const auto span = newestStart - earliest;
    if (span < 1.0) return false;
    const auto step = span /
        static_cast<double>(kCompactScopeSearchPoints - 1U);
    float bestCorrelation = -2.0f;
    double bestStart = newestStart;
    std::size_t bestPoint = 0;
    for (std::size_t point = 0; point < kCompactScopeSearchPoints; ++point) {
        const auto candidate = newestStart - step * static_cast<double>(point);
        const auto correlation = windowCorrelation(candidate, length);
        // A nearly equivalent recent recurrence is preferable to an older one.
        if (correlation > bestCorrelation + 0.0025f) {
            bestCorrelation = correlation;
            bestStart = candidate;
            bestPoint = point;
        }
    }

    const bool earliestBoundary = bestPoint + 2U >= kCompactScopeSearchPoints;
    if (!earliestBoundary) {
        const auto refineBegin = std::max(earliest, bestStart - step);
        const auto refineEnd = std::min(newestStart, bestStart + step);
        for (std::size_t point = 0; point < kCompactScopeRefinePoints; ++point) {
            const auto amount = static_cast<double>(point) /
                static_cast<double>(kCompactScopeRefinePoints - 1U);
            const auto candidate = refineEnd - amount * (refineEnd - refineBegin);
            const auto correlation = windowCorrelation(candidate, length);
            if (correlation > bestCorrelation + 0.0001f) {
                bestCorrelation = correlation;
                bestStart = candidate;
            }
        }
    }

    confidence = std::max(0.0f, bestCorrelation);
    if (!earliestBoundary &&
        bestCorrelation >= kCompactScopePhaseLockThreshold) {
        selectedStart = bestStart;
        return true;
    }

    // A proven periodic lock may wait briefly for its next recurrence. Weak
    // material that never locked, or a lock that has genuinely disappeared,
    // advances immediately instead of holding a stale window for several
    // cycles.
    if (locked_ &&
        newestStart - renderedStart_ <
            length * kCompactScopeLockedGraceWindows) {
        confidence = lockConfidence_;
        return false;
    }
    selectedStart = newestStart;
    confidence = 0.0f;
    return true;
}

void CompactScopeModel::renderWindow(double start, double length) noexcept
{
    if (historyCount_ < 2U || length < 2.0) return;
    const auto minimum = historyWrite_ - historyCount_;
    const auto maximum = historyWrite_ - 1U;
    start = std::clamp(start,
        static_cast<double>(minimum), static_cast<double>(maximum));
    length = std::clamp(length, 2.0,
        static_cast<double>(maximum) - start + 1.0);

    std::array<float, kScopeSamples> renderedLow{};
    std::array<float, kScopeSamples> renderedHigh{};
    auto windowLow = std::numeric_limits<float>::max();
    auto windowHigh = std::numeric_limits<float>::lowest();
    for (std::size_t output = 0; output < kScopeSamples; ++output) {
        const auto pointAmount = static_cast<double>(output) /
            static_cast<double>(kScopeSamples - 1U);
        const auto pointPosition = start + pointAmount * (length - 1.0);
        const auto point = interpolatedRaw(pointPosition);

        const auto binStart = start + length *
            static_cast<double>(output) / static_cast<double>(kScopeSamples);
        const auto binEnd = start + length *
            static_cast<double>(output + 1U) / static_cast<double>(kScopeSamples);
        auto low = point;
        auto high = point;
        const auto first = std::max<std::uint64_t>(minimum,
            static_cast<std::uint64_t>(std::floor(binStart)));
        const auto last = std::min<std::uint64_t>(maximum,
            static_cast<std::uint64_t>(std::ceil(binEnd)));
        for (auto absolute = first; absolute <= last; ++absolute) {
            const auto value = rawAt(absolute);
            low = std::min(low, value);
            high = std::max(high, value);
        }

        raw_[output] = point;
        renderedLow[output] = low;
        renderedHigh[output] = high;
        windowLow = std::min(windowLow, low);
        windowHigh = std::max(windowHigh, high);
    }

    // The compact scope behaves like an AC-coupled oscilloscope.  Its fixed
    // C2 window may contain less than one cycle of boosted sub-bass; drawing
    // that partial lobe around absolute zero makes the waveform appear stuck
    // to the floor.  Center only the presentation window.  raw_ remains the
    // untouched post-DSP signal used by the phase-coherence search.
    const auto visualCenter = std::isfinite(windowLow) &&
            std::isfinite(windowHigh)
        ? (windowLow + windowHigh) * 0.5f
        : 0.0f;
    for (std::size_t output = 0; output < kScopeSamples; ++output) {
        samples_[output] = boundedVisualSample(raw_[output] - visualCenter);
        minimum_[output] = boundedVisualSample(
            renderedLow[output] - visualCenter);
        maximum_[output] = boundedVisualSample(
            renderedHigh[output] - visualCenter);
    }
    ++renderRevision_;
}

bool CompactScopeModel::ingest(const RealtimeFrameV1& frame) noexcept
{
    const auto expectedSamples = static_cast<std::size_t>(frame.frameCount) *
        frame.channels;
    if (frame.version != kRealtimeRingVersion || frame.sequence == 0 ||
        frame.sequence == sequence_ || frame.frameCount == 0 ||
        frame.frameCount > kRealtimeMaxFrames || frame.channels == 0 ||
        frame.channels > kRealtimeMaxChannels || frame.sampleRate == 0 ||
        frame.sampleCount < expectedSamples ||
        frame.sampleCount > kRealtimeMaxSamples) {
        return false;
    }

    if (sampleRate_ != 0 && sampleRate_ != frame.sampleRate) {
        resetSignal();
    } else if (sequence_ != 0 && frame.sequence > sequence_ + 1U) {
        // A transport publication gap invalidates phase correspondence, but
        // the PCM already retained in the scope history remains measured.
        // Keep it and reacquire phase rather than presenting a black reset.
        hasRenderedWindow_ = false;
        hasPhaseReference_ = false;
        locked_ = false;
        lockConfidence_ = 0.0f;
    }
    append(frame);

    const auto referencePeriod = static_cast<double>(frame.sampleRate) /
        kCompactScopeReferenceHz;
    periodSamples_ = static_cast<float>(referencePeriod);
    periodHz_ = static_cast<float>(kCompactScopeReferenceHz);
    displayWindowSamples_ = static_cast<std::uint32_t>(std::lround(
        std::min(referencePeriod, static_cast<double>(historyCount_))));

    if (historyCount_ < 2U) {
        sequence_ = frame.sequence;
        return true;
    }

    if (static_cast<double>(historyCount_) < referencePeriod) {
        locked_ = false;
        lockConfidence_ = static_cast<float>(std::clamp(
            static_cast<double>(historyCount_) / referencePeriod, 0.0, 1.0));
        renderWindow(static_cast<double>(historyWrite_ - historyCount_),
            static_cast<double>(historyCount_));
    } else {
        const auto newestStart = static_cast<double>(historyWrite_) -
            referencePeriod;
        if (!hasRenderedWindow_) {
            renderWindow(newestStart, referencePeriod);
            renderedStart_ = newestStart;
            updatePhaseReference();
            phaseReferenceStart_ = newestStart;
            hasPhaseReference_ = true;
            hasRenderedWindow_ = true;
            locked_ = false;
            lockConfidence_ = 0.0f;
        } else {
            double selectedStart = newestStart;
            float confidence = 0.0f;
            if (choosePhaseCoherentStart(
                    newestStart, referencePeriod, selectedStart, confidence)) {
                renderWindow(selectedStart, referencePeriod);
                renderedStart_ = selectedStart;
                lockConfidence_ = confidence;
                locked_ = confidence >= kCompactScopePhaseLockThreshold;
                if (locked_ || !hasPhaseReference_ ||
                    newestStart - phaseReferenceStart_ >=
                        referencePeriod * kCompactScopePhaseSearchWindows) {
                    updatePhaseReference();
                    phaseReferenceStart_ = selectedStart;
                    hasPhaseReference_ = true;
                }
            }
        }
    }

    sequence_ = frame.sequence;
    return true;
}

void CompactScopePresenter::reset() noexcept
{
    samples_.fill(0.0f);
    minimum_.fill(0.0f);
    maximum_.fill(0.0f);
    targetSamples_.fill(0.0f);
    targetMinimum_.fill(0.0f);
    targetMaximum_.fill(0.0f);
    targetRevision_ = 0;
    targetConfidence_ = 0.0f;
    targetDelta_ = 0.0f;
    targetLocked_ = false;
    ready_ = false;
    active_ = false;
    releasing_ = false;
    releaseElapsed_ = 0.0;
}

bool CompactScopePresenter::syncTarget(
    const CompactScopeModel& model) noexcept
{
    return syncTarget(compactScopeRenderState(model));
}

bool CompactScopePresenter::syncTarget(
    const CompactScopeRenderState& state) noexcept
{
    const auto revision = state.revision;
    if (revision == 0 || revision == targetRevision_) return false;

    targetRevision_ = revision;
    releasing_ = false;
    releaseElapsed_ = 0.0;
    targetSamples_ = state.samples;
    targetMinimum_ = state.minimum;
    targetMaximum_ = state.maximum;
    targetConfidence_ = state.lockConfidence;
    targetLocked_ = state.locked;
    if (!ready_) {
        samples_ = targetSamples_;
        minimum_ = targetMinimum_;
        maximum_ = targetMaximum_;
        targetDelta_ = 0.0f;
        ready_ = true;
        active_ = false;
        return true;
    }

    float largestDelta = 0.0f;
    for (std::size_t index = 0; index < kScopeSamples; ++index) {
        largestDelta = std::max({
            largestDelta,
            std::abs(targetSamples_[index] - samples_[index]),
            std::abs(targetMinimum_[index] - minimum_[index]),
            std::abs(targetMaximum_[index] - maximum_[index]),
        });
    }
    targetDelta_ = largestDelta;
    active_ = largestDelta > kCompactScopePresentationEpsilon;
    return false;
}

bool CompactScopePresenter::beginRelease() noexcept
{
    if (!ready_) return false;
    targetSamples_.fill(0.0f);
    targetMinimum_.fill(0.0f);
    targetMaximum_.fill(0.0f);
    targetDelta_ = 1.0f;
    targetConfidence_ = 0.0f;
    targetLocked_ = false;
    releaseElapsed_ = 0.0;
    releasing_ = true;
    active_ = true;
    return true;
}

bool CompactScopePresenter::advance(double elapsedSeconds) noexcept
{
    if (!ready_ || !active_ || elapsedSeconds <= 0.0) return false;

    const auto elapsed = std::clamp(elapsedSeconds, 0.0005, 0.050);
    releaseElapsed_ += elapsed;
    const auto confidence = !releasing_ && targetLocked_
        ? std::clamp(
            (targetConfidence_ - kCompactScopePhaseLockThreshold) /
                (1.0f - kCompactScopePhaseLockThreshold),
            0.0f,
            1.0f)
        : 0.0f;
    const auto motion = std::clamp(targetDelta_ / 0.75f, 0.0f, 1.0f);
    const auto unlockedTimeConstant =
        kCompactScopeUnlockedPresentationTimeConstant *
        (0.82 + 0.18 * static_cast<double>(motion));
    const auto timeConstant = releasing_ ? 0.090 :
        unlockedTimeConstant +
        (kCompactScopeLockedPresentationTimeConstant -
            unlockedTimeConstant) *
            static_cast<double>(confidence);
    const auto amount = static_cast<float>(
        1.0 - std::exp(-elapsed / timeConstant));
    float largestResidual = 0.0f;
    for (std::size_t index = 0; index < kScopeSamples; ++index) {
        auto approach = [&](float& value, float target) {
            value += (target - value) * amount;
            largestResidual = std::max(
                largestResidual, std::abs(target - value));
        };
        approach(samples_[index], targetSamples_[index]);
        approach(minimum_[index], targetMinimum_[index]);
        approach(maximum_[index], targetMaximum_[index]);
    }

    if ((releasing_ && releaseElapsed_ >= 0.450) ||
            largestResidual <= kCompactScopePresentationEpsilon) {
        samples_ = targetSamples_;
        minimum_ = targetMinimum_;
        maximum_ = targetMaximum_;
        active_ = false;
        releasing_ = false;
    }
    return true;
}

void RealtimeVisualModel::updateScope(const RealtimeFrameV1& frame) noexcept
{
    if (scopeModel_.ingest(frame)) snapshot_.scope = scopeModel_.samples();
}

void RealtimeVisualModel::updateVectorscope(const RealtimeFrameV1& frame) noexcept
{
    float localPeak = 0.0f;
    std::array<StereoPoint, kVectorscopePoints> next{};
    for (std::size_t index = 0; index < kVectorscopePoints; ++index) {
        const auto source = static_cast<std::uint32_t>(
            index * static_cast<std::size_t>(frame.frameCount - 1U) /
            std::max<std::size_t>(1, kVectorscopePoints - 1U));
        const auto left = sampleAt(frame, source, 0);
        const auto right = frame.channels > 1 ? sampleAt(frame, source, 1) : left;
        next[index].x = (left - right) * 0.70710678f;
        next[index].y = (left + right) * 0.70710678f;
        localPeak = std::max(localPeak,
            std::max(std::abs(next[index].x), std::abs(next[index].y)));
    }
    vectorPeakEwma_ = std::max(localPeak,
        vectorPeakEwma_ * (localPeak < vectorPeakEwma_ ? 0.988f : 0.90f));
    const auto gain = std::clamp(0.82f / std::max(0.03f, vectorPeakEwma_), 0.7f, 7.0f);
    for (std::size_t index = 0; index < kVectorscopePoints; ++index) {
        snapshot_.vectorscope[index].x = std::clamp(next[index].x * gain, -1.0f, 1.0f);
        snapshot_.vectorscope[index].y = std::clamp(next[index].y * gain, -1.0f, 1.0f);
    }
}

void RealtimeVisualModel::updateSpectrum() noexcept
{
    const auto& tables = spectrumTables();
    const auto zeroPadding = kAnalysisFftSize - spectrumHistoryCount_;
    const auto oldest =
        (spectrumHistoryWrite_ + kAnalysisFftSize - spectrumHistoryCount_) %
        kAnalysisFftSize;
    for (std::size_t index = 0; index < kAnalysisFftSize; ++index) {
        const auto value = index < zeroPadding
            ? 0.0f
            : spectrumHistory_[
                (oldest + index - zeroPadding) % kAnalysisFftSize];
        spectrumFft_[tables.bitReverse[index]] = {
            value * tables.window[index], 0.0f};
    }

    for (std::size_t length = 2; length <= kAnalysisFftSize; length <<= 1U) {
        const auto half = length / 2U;
        const auto twiddleStep = kAnalysisFftSize / length;
        for (std::size_t base = 0; base < kAnalysisFftSize; base += length) {
            for (std::size_t offset = 0; offset < half; ++offset) {
                const auto even = spectrumFft_[base + offset];
                const auto odd = spectrumFft_[base + offset + half] *
                    tables.twiddles[offset * twiddleStep];
                spectrumFft_[base + offset] = even + odd;
                spectrumFft_[base + offset + half] = even - odd;
            }
        }
    }

    const auto sampleRate = std::max<std::uint32_t>(1, snapshot_.sampleRate);
    constexpr double minimumFrequency = 30.0;
    const auto maximumFrequency = std::min(
        20000.0, static_cast<double>(sampleRate) * 0.5);
    // Match the previous Hann-windowed DFT calibration (a bin-centred
    // full-scale sine reports roughly 0.25 before the dB mapping).
    const auto magnitudeScale = 0.5 / std::max(1.0, tables.windowSum);

    // Analysis remains fixed at 60 Hz, while presentation may run at any
    // monitor cadence. Express the musical attack/release in wall-clock time
    // so a 60 Hz and a 240 Hz display see the same spectral envelope.
    constexpr double featureSeconds = 1.0 / 60.0;
    constexpr double attackSeconds = 0.018;
    constexpr double releaseSeconds = 0.145;
    const auto attackAmount = static_cast<float>(
        1.0 - std::exp(-featureSeconds / attackSeconds));
    const auto releaseAmount = static_cast<float>(
        1.0 - std::exp(-featureSeconds / releaseSeconds));

    double totalPower = 0.0;
    double weightedFrequency = 0.0;
    const auto maximumBin = std::min<std::size_t>(
        kSpectrumFftBins - 1U,
        static_cast<std::size_t>(std::floor(
            maximumFrequency * static_cast<double>(kAnalysisFftSize) /
            static_cast<double>(sampleRate))));
    for (std::size_t bin = 0; bin < kSpectrumFftBins; ++bin) {
        const auto frequency = static_cast<double>(bin) *
            static_cast<double>(sampleRate) /
            static_cast<double>(kAnalysisFftSize);
        float next = 0.0f;
        double instantaneousPower = 0.0;
        if (bin > 0 && bin <= maximumBin && frequency >= minimumFrequency) {
            const auto magnitude =
                static_cast<double>(std::abs(spectrumFft_[bin])) *
                magnitudeScale;
            instantaneousPower = magnitude * magnitude;
            const auto decibels =
                20.0 * std::log10(std::max(1e-7, magnitude));
            next = static_cast<float>(std::clamp(
                (decibels + 72.0) / 66.0, 0.0, 1.0));
        }
        const auto smoothing = next > previousSpectrumFft_[bin]
            ? attackAmount : releaseAmount;
        previousSpectrumFft_[bin] +=
            (next - previousSpectrumFft_[bin]) * smoothing;
        snapshot_.spectrumFft[bin] = previousSpectrumFft_[bin];
        spectrumPower_[bin] = instantaneousPower;

        if (instantaneousPower <= 0.0) {
            continue;
        }
        totalPower += instantaneousPower;
        weightedFrequency += frequency * instantaneousPower;
    }

    auto centroid = totalPower > 1e-14
        ? weightedFrequency / totalPower : 0.0;
    double rolloff = 0.0;
    if (totalPower > 1e-14) {
        double cumulativePower = 0.0;
        for (std::size_t bin = 1; bin <= maximumBin; ++bin) {
            cumulativePower += spectrumPower_[bin];
            if (cumulativePower >= totalPower * 0.85) {
                rolloff = static_cast<double>(bin) *
                    static_cast<double>(sampleRate) /
                    static_cast<double>(kAnalysisFftSize);
                break;
            }
        }
    }

    const auto rmsDb = 20.0 * std::log10(
        std::max(1e-9, static_cast<double>(snapshot_.rms)));
    auto confidenceTarget = static_cast<float>(std::clamp(
        (rmsDb + 78.0) / 26.0, 0.0, 1.0));
    confidenceTarget = confidenceTarget * confidenceTarget *
        (3.0f - 2.0f * confidenceTarget);
    if (totalPower <= 1e-14 || !std::isfinite(centroid) ||
        !std::isfinite(rolloff)) {
        confidenceTarget = 0.0f;
        centroid = 0.0;
        rolloff = 0.0;
    }
    const auto confidenceTime = confidenceTarget > previousSpectralConfidence_
        ? 0.045 : 0.140;
    const auto confidenceAmount = static_cast<float>(
        1.0 - std::exp(-featureSeconds / confidenceTime));
    previousSpectralConfidence_ +=
        (confidenceTarget - previousSpectralConfidence_) * confidenceAmount;
    if (confidenceTarget > 0.02f) {
        constexpr double markerTime = 0.090;
        const auto markerAmount = static_cast<float>(
            1.0 - std::exp(-featureSeconds / markerTime));
        if (previousSpectralCentroidHz_ <= 0.0f)
            previousSpectralCentroidHz_ = static_cast<float>(centroid);
        else
            previousSpectralCentroidHz_ +=
                (static_cast<float>(centroid) -
                    previousSpectralCentroidHz_) * markerAmount;
        if (previousSpectralRolloff85Hz_ <= 0.0f)
            previousSpectralRolloff85Hz_ = static_cast<float>(rolloff);
        else
            previousSpectralRolloff85Hz_ +=
                (static_cast<float>(rolloff) -
                    previousSpectralRolloff85Hz_) * markerAmount;
    }
    snapshot_.spectralCentroidHz = previousSpectralCentroidHz_;
    snapshot_.spectralRolloff85Hz = previousSpectralRolloff85Hz_;
    snapshot_.spectralConfidence = previousSpectralConfidence_;

    double logarithmicWeight = 0.0;
    double normalizedMagnitude = 0.0;
    for (std::size_t bin = 0; bin < kSpectrumBins; ++bin) {
        const auto normalized = static_cast<double>(bin) /
            static_cast<double>(kSpectrumBins - 1U);
        const auto frequency = minimumFrequency * std::pow(
            maximumFrequency / minimumFrequency, normalized);
        const auto fftIndex = std::clamp(
            frequency * static_cast<double>(kAnalysisFftSize) /
                static_cast<double>(sampleRate),
            0.0,
            static_cast<double>(kAnalysisFftSize / 2U - 2U));
        const auto lower = static_cast<std::size_t>(std::floor(fftIndex));
        const auto blend = static_cast<float>(fftIndex - lower);
        const auto next = std::lerp(
            snapshot_.spectrumFft[lower],
            snapshot_.spectrumFft[lower + 1U],
            blend);
        previousSpectrum_[bin] = next;
        snapshot_.spectrum[bin] = previousSpectrum_[bin];
        logarithmicWeight += normalized * previousSpectrum_[bin];
        normalizedMagnitude += previousSpectrum_[bin];
    }
    snapshot_.brightness = normalizedMagnitude > 1e-6
        ? static_cast<float>(logarithmicWeight / normalizedMagnitude)
        : 0.0f;

    updateCalibratedSpectrum();
    auto& column = spectrogram_[spectrogramHead_];
    for (std::size_t y = 0; y < kSpectrogramBins; ++y) {
        const auto amount = static_cast<double>(y) /
            static_cast<double>(std::max<std::size_t>(
                1, kSpectrogramBins - 1U));
        const auto maximumFrequency = std::min(
            20000.0, static_cast<double>(snapshot_.sampleRate) * 0.5);
        const auto frequency = 30.0 * std::pow(
            std::max(1.0, maximumFrequency / 30.0), amount);
        const auto source = std::clamp(
            frequency * static_cast<double>(kAnalysisFftSize) /
                static_cast<double>(std::max(1U, snapshot_.sampleRate)),
            0.0,
            static_cast<double>(kSpectrumFftBins - 1U));
        const auto lower = std::min<std::size_t>(
            kSpectrumFftBins - 2U,
            static_cast<std::size_t>(std::floor(source)));
        const auto power = std::lerp(
            static_cast<double>(snapshot_.spectrumPowerStereo[lower]),
            static_cast<double>(snapshot_.spectrumPowerStereo[lower + 1U]),
            source - static_cast<double>(lower));
        const auto decibels = power > 1e-20
            ? 10.0 * std::log10(power) : -90.0;
        column[kSpectrogramBins - 1U - y] = static_cast<float>(
            std::clamp((decibels + 90.0) / 90.0, 0.0, 1.0));
    }
    spectrogramHead_ = (spectrogramHead_ + 1U) % kSpectrogramColumns;
    ++spectrogramRevision_;
}

void RealtimeVisualModel::updateCalibratedSpectrum() noexcept
{
    const auto& tables = spectrumTables();
    const auto zeroPadding = kAnalysisFftSize - spectrumHistoryCount_;
    const auto oldest =
        (spectrumHistoryWrite_ + kAnalysisFftSize - spectrumHistoryCount_) %
        kAnalysisFftSize;
    snapshot_.spectrumZeroPadded = zeroPadding != 0;

    const auto transform = [&](const auto& history, auto& destination) {
        for (std::size_t index = 0; index < kAnalysisFftSize; ++index) {
            const auto value = index < zeroPadding ? 0.0f : history[
                (oldest + index - zeroPadding) % kAnalysisFftSize];
            spectrumFft_[tables.bitReverse[index]] = {
                value * tables.window[index], 0.0f};
        }
        for (std::size_t length = 2; length <= kAnalysisFftSize;
             length <<= 1U) {
            const auto half = length / 2U;
            const auto step = kAnalysisFftSize / length;
            for (std::size_t base = 0; base < kAnalysisFftSize;
                 base += length) {
                for (std::size_t offset = 0; offset < half; ++offset) {
                    const auto even = spectrumFft_[base + offset];
                    const auto odd = spectrumFft_[base + offset + half] *
                        tables.twiddles[offset * step];
                    spectrumFft_[base + offset] = even + odd;
                    spectrumFft_[base + offset + half] = even - odd;
                }
            }
        }
        for (std::size_t bin = 0; bin < kSpectrumFftBins; ++bin) {
            const auto factor =
                (bin == 0 || bin + 1U == kSpectrumFftBins) ? 1.0 : 2.0;
            const auto amplitude = static_cast<double>(
                std::abs(spectrumFft_[bin])) * factor /
                std::max(1.0, tables.windowSum);
            destination[bin] = static_cast<float>(amplitude * amplitude);
        }
    };

    transform(spectrumHistoryLeft_, snapshot_.spectrumPowerLeft);
    transform(spectrumHistoryRight_, snapshot_.spectrumPowerRight);
    for (std::size_t bin = 0; bin < kSpectrumFftBins; ++bin) {
        snapshot_.spectrumPowerStereo[bin] = 0.5f *
            (snapshot_.spectrumPowerLeft[bin] +
             snapshot_.spectrumPowerRight[bin]);
    }
    transform(spectrumHistory_, snapshot_.spectrumPowerMid);
}

void RealtimeVisualModel::updateHistories() noexcept
{
    energyHistory_[historyHead_] = clampUnit(snapshot_.rms * 2.8f);
    widthHistory_[historyHead_] = snapshot_.width;
    brightnessHistory_[historyHead_] = snapshot_.brightness;
    historyHead_ = (historyHead_ + 1U) % kEvolutionSamples;
}

void RealtimeVisualModel::updateMusicMap() noexcept
{
    if (durationSeconds_ <= 0.0 || snapshot_.playbackSeconds < 0.0) return;
    const auto normalized = std::clamp(
        snapshot_.playbackSeconds / durationSeconds_, 0.0, 0.999999);
    const auto index = std::min<std::size_t>(kMusicMapBins - 1U,
        static_cast<std::size_t>(normalized * kMusicMapBins));
    auto& bin = musicMap_[index];
    const auto blend = bin.observations == 0 ? 1.0f : 0.22f;
    bin.energy += (clampUnit(snapshot_.rms * 2.8f) - bin.energy) * blend;
    bin.width += (snapshot_.width - bin.width) * blend;
    bin.brightness += (snapshot_.brightness - bin.brightness) * blend;
    bin.pulse += (snapshot_.pulse - bin.pulse) * blend;
    ++bin.observations;
}

} // namespace nekorox
