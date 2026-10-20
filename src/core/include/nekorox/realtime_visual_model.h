#pragma once

#include "nekorox/contracts.h"

#include <array>
#include <cstddef>
#include <complex>
#include <cstdint>
#include <span>

namespace nekorox {

inline constexpr std::size_t kScopeSamples = 512;
inline constexpr std::size_t kLargeScopeColumns = 2048;
inline constexpr std::size_t kLargeScopeHistoryFrames = 48000;
inline constexpr std::size_t kCompactScopeHistorySamples = 16384;
inline constexpr std::size_t kVectorscopePoints = 256;
inline constexpr std::size_t kSpectrumBins = 96;
inline constexpr std::size_t kAnalysisFftSize = 4096;
inline constexpr std::size_t kSpectrumFftBins = kAnalysisFftSize / 2U + 1U;
inline constexpr std::size_t kSpectrogramColumns = 240;
inline constexpr std::size_t kSpectrogramBins = 256;
inline constexpr std::size_t kEvolutionSamples = 300;
inline constexpr std::size_t kMusicMapBins = 320;

struct StereoPoint {
    float x = 0.0f;
    float y = 0.0f;
};

enum class LargeScopeChannelMode : std::uint8_t {
    mid,
    left,
    right,
    side,
};

struct LargeScopeRenderState {
    std::array<float, kLargeScopeColumns> minimum{};
    std::array<float, kLargeScopeColumns> maximum{};
    std::size_t columns = 0;
    std::uint64_t revision = 0;
    std::uint64_t parameterRevision = 1;
    std::uint64_t sourceSequence = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t windowMilliseconds = 30;
    LargeScopeChannelMode mode = LargeScopeChannelMode::mid;
    float gain = 1.0f;
    bool triggered = false;
};

class LargeScopeModel final {
public:
    void reset() noexcept;
    bool ingest(const RealtimeFrameV1& frame) noexcept;
    void configure(std::uint32_t windowMilliseconds,
        LargeScopeChannelMode mode, float gain = 1.0f) noexcept;
    [[nodiscard]] LargeScopeRenderState render(
        std::size_t columns = kLargeScopeColumns) const noexcept;
    [[nodiscard]] std::size_t historyCount() const noexcept { return count_; }

private:
    float sampleAt(std::uint64_t absolute,
        LargeScopeChannelMode mode) const noexcept;
    std::array<float, kLargeScopeHistoryFrames> left_{};
    std::array<float, kLargeScopeHistoryFrames> right_{};
    std::uint64_t write_ = 0;
    std::size_t count_ = 0;
    std::uint64_t sequence_ = 0;
    std::uint64_t streamGeneration_ = 0;
    std::uint64_t revision_ = 0;
    std::uint64_t parameterRevision_ = 1;
    std::uint32_t sampleRate_ = 0;
    std::uint32_t windowMilliseconds_ = 30;
    LargeScopeChannelMode mode_ = LargeScopeChannelMode::mid;
    float gain_ = 1.0f;
};

struct MusicMapBin {
    float energy = 0.0f;
    float width = 0.0f;
    float brightness = 0.0f;
    float pulse = 0.0f;
    std::uint32_t observations = 0;
};

using MusicMapProfile = std::array<MusicMapBin, kMusicMapBins>;

// Builds a full-track, deterministic structure map from the persisted 100 ms
// waveform. The four normalized lanes use fixed physical scales: loudness,
// peak, crest/dynamics and short-term change. Unlike the legacy listening map,
// the result does not depend on when or how long Listen happened to be open.
[[nodiscard]] MusicMapProfile buildMusicMapProfile(
    std::span<const AnalysisWaveformBinV1> waveform) noexcept;

struct RealtimeVisualSnapshot {
    std::uint64_t sequence = 0;
    double playbackSeconds = -1.0;
    std::uint32_t sampleRate = 0;
    float rms = 0.0f;
    float peak = 0.0f;
    float correlation = 1.0f;
    float width = 0.0f;
    float brightness = 0.0f;
    float pulse = 0.0f;
    float spectralCentroidHz = 0.0f;
    float spectralRolloff85Hz = 0.0f;
    float spectralConfidence = 0.0f;
    std::array<float, kScopeSamples> scope{};
    std::array<StereoPoint, kVectorscopePoints> vectorscope{};
    std::array<float, kSpectrumBins> spectrum{};
    // Legacy fixed-scale FFT values retained for the compact footer spectrum.
    // Listen V2 consumes the calibrated linear-power arrays below instead.
    std::array<float, kSpectrumFftBins> spectrumFft{};
    // Calibrated one-sided FFT power. A bin-centred full-scale sine is 1.0;
    // dBFS_peak is therefore 10*log10(power).
    std::array<float, kSpectrumFftBins> spectrumPowerLeft{};
    std::array<float, kSpectrumFftBins> spectrumPowerRight{};
    std::array<float, kSpectrumFftBins> spectrumPowerStereo{};
    std::array<float, kSpectrumFftBins> spectrumPowerMid{};
    bool spectrumZeroPadded = true;
    LargeScopeRenderState largeScope{};
};

class CompactScopeModel final {
public:
    void reset() noexcept;
    bool ingest(const RealtimeFrameV1& frame) noexcept;

    const std::array<float, kScopeSamples>& samples() const noexcept {
        return samples_;
    }
    const std::array<float, kScopeSamples>& rawSamples() const noexcept {
        return raw_;
    }
    const std::array<float, kScopeSamples>& minimumSamples() const noexcept {
        return minimum_;
    }
    const std::array<float, kScopeSamples>& maximumSamples() const noexcept {
        return maximum_;
    }
    std::uint64_t sequence() const noexcept { return sequence_; }
    bool locked() const noexcept { return locked_; }
    float lockConfidence() const noexcept { return lockConfidence_; }
    float periodHz() const noexcept { return periodHz_; }
    std::uint64_t renderRevision() const noexcept { return renderRevision_; }
    std::uint32_t displayWindowSamples() const noexcept {
        return displayWindowSamples_;
    }

private:
    void resetSignal() noexcept;
    void append(const RealtimeFrameV1& frame) noexcept;
    float rawAt(std::uint64_t absoluteIndex) const noexcept;
    float interpolatedRaw(double absoluteIndex) const noexcept;
    float windowCorrelation(double start, double length) const noexcept;
    void updatePhaseReference() noexcept;
    bool choosePhaseCoherentStart(
        double newestStart,
        double length,
        double& selectedStart,
        float& confidence) const noexcept;
    void renderWindow(double start, double length) noexcept;

    std::array<float, kCompactScopeHistorySamples> history_{};
    std::array<float, kScopeSamples> raw_{};
    std::array<float, kScopeSamples> phaseReference_{};
    std::array<float, 64> phaseCorrelationReference_{};
    std::array<float, kScopeSamples> samples_{};
    std::array<float, kScopeSamples> minimum_{};
    std::array<float, kScopeSamples> maximum_{};
    std::uint64_t historyWrite_ = 0;
    std::uint64_t sequence_ = 0;
    std::uint64_t renderRevision_ = 0;
    double renderedStart_ = 0.0;
    double phaseReferenceStart_ = 0.0;
    std::size_t historyCount_ = 0;
    std::uint32_t sampleRate_ = 0;
    std::uint32_t displayWindowSamples_ = 0;
    float periodSamples_ = 0.0f;
    float periodHz_ = 0.0f;
    float lockConfidence_ = 0.0f;
    double phaseCorrelationEnergy_ = 0.0;
    bool hasRenderedWindow_ = false;
    bool hasPhaseReference_ = false;
    bool locked_ = false;
};

struct CompactScopeRenderState {
    std::array<float, kScopeSamples> samples{};
    std::array<float, kScopeSamples> minimum{};
    std::array<float, kScopeSamples> maximum{};
    std::uint64_t revision = 0;
    std::uint64_t sourceSequence = 0;
    float lockConfidence = 0.0f;
    float periodHz = 0.0f;
    bool locked = false;
};

inline CompactScopeRenderState compactScopeRenderState(
    const CompactScopeModel& model) noexcept
{
    CompactScopeRenderState state;
    state.samples = model.samples();
    state.minimum = model.minimumSamples();
    state.maximum = model.maximumSamples();
    state.revision = model.renderRevision();
    state.sourceSequence = model.sequence();
    state.lockConfidence = model.lockConfidence();
    state.periodHz = model.periodHz();
    state.locked = model.locked();
    return state;
}

class CompactScopePresenter final {
public:
    void reset() noexcept;
    bool syncTarget(const CompactScopeModel& model) noexcept;
    bool syncTarget(const CompactScopeRenderState& state) noexcept;
    bool beginRelease() noexcept;
    bool advance(double elapsedSeconds) noexcept;

    const std::array<float, kScopeSamples>& samples() const noexcept {
        return samples_;
    }
    const std::array<float, kScopeSamples>& minimumSamples() const noexcept {
        return minimum_;
    }
    const std::array<float, kScopeSamples>& maximumSamples() const noexcept {
        return maximum_;
    }
    bool ready() const noexcept { return ready_; }
    bool active() const noexcept { return active_; }
    std::uint64_t targetRevision() const noexcept { return targetRevision_; }

private:
    std::array<float, kScopeSamples> samples_{};
    std::array<float, kScopeSamples> minimum_{};
    std::array<float, kScopeSamples> maximum_{};
    std::array<float, kScopeSamples> targetSamples_{};
    std::array<float, kScopeSamples> targetMinimum_{};
    std::array<float, kScopeSamples> targetMaximum_{};
    std::uint64_t targetRevision_ = 0;
    float targetConfidence_ = 0.0f;
    float targetDelta_ = 0.0f;
    bool targetLocked_ = false;
    bool ready_ = false;
    bool active_ = false;
    bool releasing_ = false;
    double releaseElapsed_ = 0.0;
};

class RealtimeVisualModel final {
public:
    void reset(double durationSeconds = 0.0) noexcept;
    bool ingest(const RealtimeFrameV1& frame,
        bool buildLegacyScope = true) noexcept;
    void configureLargeScope(std::uint32_t windowMilliseconds,
        LargeScopeChannelMode mode, float gain = 1.0f) noexcept
    {
        largeScopeModel_.configure(windowMilliseconds, mode, gain);
    }

    const RealtimeVisualSnapshot& snapshot() const noexcept { return snapshot_; }
    const std::array<std::array<float, kSpectrogramBins>, kSpectrogramColumns>&
        spectrogram() const noexcept { return spectrogram_; }
    std::size_t spectrogramHead() const noexcept { return spectrogramHead_; }
    std::uint64_t spectrogramRevision() const noexcept {
        return spectrogramRevision_;
    }
    const std::array<MusicMapBin, kMusicMapBins>& musicMap() const noexcept {
        return musicMap_;
    }
    const std::array<float, kEvolutionSamples>& energyHistory() const noexcept {
        return energyHistory_;
    }
    const std::array<float, kEvolutionSamples>& widthHistory() const noexcept {
        return widthHistory_;
    }
    const std::array<float, kEvolutionSamples>& brightnessHistory() const noexcept {
        return brightnessHistory_;
    }
    std::size_t historyHead() const noexcept { return historyHead_; }
    double durationSeconds() const noexcept { return durationSeconds_; }
    std::uint64_t scopeSequence() const noexcept {
        return scopeModel_.sequence();
    }
    std::uint64_t scopeRenderRevision() const noexcept {
        return scopeModel_.renderRevision();
    }
    bool scopeLocked() const noexcept { return scopeModel_.locked(); }
    float scopeLockConfidence() const noexcept {
        return scopeModel_.lockConfidence();
    }
    float scopePeriodHz() const noexcept { return scopeModel_.periodHz(); }
    const CompactScopeModel& scopeModel() const noexcept {
        return scopeModel_;
    }

private:
    void updateScope(const RealtimeFrameV1& frame) noexcept;
    void updateVectorscope(const RealtimeFrameV1& frame) noexcept;
    void updateSpectrum() noexcept;
    void updateCalibratedSpectrum() noexcept;
    void updateHistories() noexcept;
    void updateMusicMap() noexcept;

    RealtimeVisualSnapshot snapshot_{};
    CompactScopeModel scopeModel_;
    LargeScopeModel largeScopeModel_;
    std::array<float, kSpectrumBins> previousSpectrum_{};
    std::array<float, kSpectrumFftBins> previousSpectrumFft_{};
    std::array<double, kSpectrumFftBins> spectrumPower_{};
    std::array<float, kAnalysisFftSize> spectrumHistory_{};
    std::array<float, kAnalysisFftSize> spectrumHistoryLeft_{};
    std::array<float, kAnalysisFftSize> spectrumHistoryRight_{};
    std::array<std::complex<float>, kAnalysisFftSize> spectrumFft_{};
    std::size_t spectrumHistoryWrite_ = 0;
    std::size_t spectrumHistoryCount_ = 0;
    std::array<std::array<float, kSpectrogramBins>, kSpectrogramColumns>
        spectrogram_{};
    std::array<MusicMapBin, kMusicMapBins> musicMap_{};
    std::array<float, kEvolutionSamples> energyHistory_{};
    std::array<float, kEvolutionSamples> widthHistory_{};
    std::array<float, kEvolutionSamples> brightnessHistory_{};
    std::size_t spectrogramHead_ = 0;
    std::uint64_t spectrogramRevision_ = 0;
    std::size_t historyHead_ = 0;
    double durationSeconds_ = 0.0;
    std::uint32_t featureSampleAccumulator_ = 0;
    float vectorPeakEwma_ = 0.15f;
    float previousRms_ = 0.0f;
    float previousSpectralCentroidHz_ = 0.0f;
    float previousSpectralRolloff85Hz_ = 0.0f;
    float previousSpectralConfidence_ = 0.0f;
};

} // namespace nekorox
