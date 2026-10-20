#include "nekorox/realtime_visual_model.h"

#include <cmath>
#include <memory>
#include <numbers>
#include <stdexcept>

namespace {
void requireSpectrum(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

nekorox::RealtimeFrameV1 sineFrame(std::uint64_t sequence,
    float leftAmplitude, float rightAmplitude, std::size_t bin = 64)
{
    nekorox::RealtimeFrameV1 frame;
    frame.version = nekorox::kRealtimeRingVersion;
    frame.sequence = sequence;
    frame.streamGeneration = sequence;
    frame.sampleRate = 48000;
    frame.channels = 2;
    frame.frameCount = nekorox::kAnalysisFftSize;
    frame.sampleCount = frame.frameCount * frame.channels;
    for (std::size_t index = 0; index < frame.frameCount; ++index) {
        const auto sample = std::sin(2.0 * std::numbers::pi *
            static_cast<double>(bin) * static_cast<double>(index) /
            static_cast<double>(nekorox::kAnalysisFftSize));
        frame.samples[index * 2U] = leftAmplitude * static_cast<float>(sample);
        frame.samples[index * 2U + 1U] =
            rightAmplitude * static_cast<float>(sample);
    }
    return frame;
}

nekorox::RealtimeFrameV1 silenceFrame(std::uint64_t sequence)
{
    return sineFrame(sequence, 0.0f, 0.0f);
}

nekorox::RealtimeFrameV1 nyquistFrame(std::uint64_t sequence)
{
    auto frame = silenceFrame(sequence);
    for (std::size_t index = 0; index < frame.frameCount; ++index) {
        const auto sample = (index & 1U) == 0U ? 1.0f : -1.0f;
        frame.samples[index * 2U] = sample;
        frame.samples[index * 2U + 1U] = sample;
    }
    return frame;
}

float db(float power)
{
    return 10.0f * std::log10(std::max(power, 1e-20f));
}
}

void runWorkpackSpectrumTests()
{
    constexpr std::size_t bin = 64;
    const auto equal = std::make_unique<nekorox::RealtimeVisualModel>();
    requireSpectrum(equal->ingest(sineFrame(1, 1.0f, 1.0f, bin)),
        "WP19 rejected full-scale sine");
    const auto full = equal->snapshot();
    requireSpectrum(!full.spectrumZeroPadded &&
            std::abs(db(full.spectrumPowerStereo[bin])) <= 0.2f,
        "WP19 full-scale bin-centred sine was not 0 dBFS_peak");

    const auto half = std::make_unique<nekorox::RealtimeVisualModel>();
    half->ingest(sineFrame(2, 0.5f, 0.5f, bin));
    requireSpectrum(std::abs(db(
            half->snapshot().spectrumPowerStereo[bin]) + 6.0206f) <= 0.2f,
        "WP19 half-scale sine was not -6.0206 dBFS_peak");

    const auto anti = std::make_unique<nekorox::RealtimeVisualModel>();
    anti->ingest(sineFrame(3, 1.0f, -1.0f, bin));
    requireSpectrum(std::abs(db(
            anti->snapshot().spectrumPowerStereo[bin])) <= 0.2f &&
            anti->snapshot().spectrumPowerMid[bin] < 1e-10f,
        "WP19 anti-phase energy cancelled in Stereo or survived in Mid");

    const auto left = std::make_unique<nekorox::RealtimeVisualModel>();
    left->ingest(sineFrame(4, 1.0f, 0.0f, bin));
    requireSpectrum(std::abs(db(
            left->snapshot().spectrumPowerStereo[bin]) + 3.0103f) <= 0.2f,
        "WP19 L-only stereo mean power was not -3.0103 dB");

    const auto silence = std::make_unique<nekorox::RealtimeVisualModel>();
    silence->ingest(silenceFrame(5));
    for (const auto power : silence->snapshot().spectrumPowerStereo) {
        requireSpectrum(power == 0.0f,
            "WP19 silence produced synthetic spectrum energy");
    }
    const auto silenceColumn =
        silence->spectrogram()[(silence->spectrogramHead() +
            nekorox::kSpectrogramColumns - 1U) %
            nekorox::kSpectrogramColumns];
    for (const auto value : silenceColumn) {
        requireSpectrum(value == 0.0f,
            "WP19 silence produced a fake spectrogram fill");
    }

    const auto nyquist = std::make_unique<nekorox::RealtimeVisualModel>();
    nyquist->ingest(nyquistFrame(6));
    requireSpectrum(std::abs(db(
            nyquist->snapshot().spectrumPowerStereo.back())) <= 0.2f,
        "WP19 Nyquist bin used the interior-bin one-sided factor");
}
