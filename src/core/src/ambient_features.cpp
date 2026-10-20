#include "nekorox/ambient_features.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace nekorox {
namespace {

double unit(double value) noexcept
{
    return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
}

double smooth(double low, double high, double value) noexcept
{
    const auto x = unit((value - low) / (high - low));
    return x * x * (3.0 - 2.0 * x);
}

double follow(double value, double target, double seconds,
    double attack, double release) noexcept
{
    if (!std::isfinite(seconds) || seconds <= 0.0) return value;
    const auto tau = target > value ? attack : release;
    return target + (value - target) * std::exp(-seconds / tau);
}

double powerDb(double power) noexcept
{
    const auto finite = std::isfinite(power) ? power : 0.0;
    return 10.0 * std::log10(std::max(1.0e-12, finite));
}

} // namespace

bool validAmbientFeatures(const AmbientFeaturesV1& features) noexcept
{
    if (features.version != kAmbientFeaturesVersionV1 ||
        features.sizeBytes != sizeof(AmbientFeaturesV1) ||
        features.channels > kRealtimeMaxChannels ||
        features.sampleRate > 384000U) return false;
    if (!std::isfinite(features.gate) || features.gate < 0.0f ||
        features.gate > 1.0f || !std::isfinite(features.broadbandDb)) {
        return false;
    }
    for (std::size_t band = 0; band < kAmbientBandCount; ++band) {
        if (!std::isfinite(features.level[band]) ||
            features.level[band] < 0.0f || features.level[band] > 1.0f ||
            !std::isfinite(features.bandDb[band]) ||
            !std::isfinite(features.absoluteEnergy[band]) ||
            features.absoluteEnergy[band] < 0.0f ||
            features.absoluteEnergy[band] > 1.0f ||
            !std::isfinite(features.relativeFast[band]) ||
            features.relativeFast[band] < 0.0f ||
            features.relativeFast[band] > 4.0f ||
            !std::isfinite(features.relativeSlow[band]) ||
            features.relativeSlow[band] < 0.0f ||
            features.relativeSlow[band] > 4.0f ||
            !std::isfinite(features.growth[band]) ||
            features.growth[band] < -1.0f || features.growth[band] > 1.0f ||
            !std::isfinite(features.attack[band]) ||
            features.attack[band] < 0.0f || features.attack[band] > 1.0f) {
            return false;
        }
    }
    return true;
}

void AmbientFeaturesModel::Biquad::configure(double sampleRate,
    double frequency, bool highPass) noexcept
{
    const auto omega = 2.0 * std::numbers::pi * frequency / sampleRate;
    const auto cosine = std::cos(omega);
    const auto alpha = std::sin(omega) / std::sqrt(2.0);
    const auto a0 = 1.0 + alpha;
    if (highPass) {
        b0 = (1.0 + cosine) * 0.5 / a0;
        b1 = -(1.0 + cosine) / a0;
        b2 = b0;
    } else {
        b0 = (1.0 - cosine) * 0.5 / a0;
        b1 = (1.0 - cosine) / a0;
        b2 = b0;
    }
    a1 = -2.0 * cosine / a0;
    a2 = (1.0 - alpha) / a0;
    z1 = 0.0;
    z2 = 0.0;
}

double AmbientFeaturesModel::Biquad::step(double sample) noexcept
{
    const auto output = b0 * sample + z1;
    z1 = b1 * sample - a1 * output + z2;
    z2 = b2 * sample - a2 * output;
    if (std::abs(z1) < 1.0e-24) z1 = 0.0;
    if (std::abs(z2) < 1.0e-24) z2 = 0.0;
    return output;
}

bool AmbientFeaturesModel::configure(std::uint32_t sampleRate,
    std::uint16_t channels, std::uint64_t streamGeneration,
    std::uint64_t firstFrame, std::uint64_t playbackSerial,
    std::uint64_t recordingIdentityHash) noexcept
{
    reset();
    if (sampleRate < 8000U || sampleRate > 384000U || channels == 0 ||
        channels > kRealtimeMaxChannels) {
        snapshot_.sourceKind = AmbientSourceKind::unsupported;
        return false;
    }
    sampleRate_ = sampleRate;
    channels_ = channels;
    nextFrame_ = firstFrame;
    windowFirstFrame_ = firstFrame;
    framesPerWindow_ = std::max(1U, static_cast<std::uint32_t>(
        std::lround(static_cast<double>(sampleRate_) / 100.0)));
    const auto highTop = std::min(14000.0, sampleRate_ * 0.42);
    constexpr std::array<std::array<double, 2>, kAmbientBandCount> edges{{
        {28.0, 180.0}, {180.0, 2500.0}, {2500.0, 0.0}}};
    for (std::uint16_t channel = 0; channel < channels_; ++channel) {
        for (std::size_t band = 0; band < kAmbientBandCount; ++band) {
            const auto top = band == 2 ? highTop : edges[band][1];
            filters_[channel][band].highPass.configure(
                sampleRate_, edges[band][0], true);
            filters_[channel][band].lowPass.configure(
                sampleRate_, top, false);
        }
    }
    snapshot_.streamGeneration = streamGeneration;
    snapshot_.playbackSerial = playbackSerial;
    snapshot_.recordingIdentityHash = recordingIdentityHash;
    snapshot_.sampleRate = sampleRate_;
    snapshot_.channels = channels_;
    snapshot_.firstFrame = firstFrame;
    snapshot_.endFrame = firstFrame;
    snapshot_.sourceKind = AmbientSourceKind::playback;
    snapshot_.flags = kAmbientFeatureDiscontinuity;
    return true;
}

void AmbientFeaturesModel::reset() noexcept
{
    *this = AmbientFeaturesModel{};
}

bool AmbientFeaturesModel::ingest(std::span<const float> pcm,
    std::uint64_t firstFrame, std::uint64_t streamGeneration,
    std::uint64_t sourceSequence, std::uint64_t sourceEndQpc) noexcept
{
    if (!configured() || pcm.empty() || pcm.size() % channels_ != 0 ||
        firstFrame != nextFrame_ ||
        streamGeneration != snapshot_.streamGeneration) return false;
    const auto frameCount = pcm.size() / channels_;
    latestSourceSequence_ = sourceSequence;
    latestSourceEndQpc_ = sourceEndQpc;
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        for (std::uint16_t channel = 0; channel < channels_; ++channel) {
            const auto raw = static_cast<double>(
                pcm[frame * channels_ + channel]);
            const auto sample = std::isfinite(raw)
                ? std::clamp(raw, -8.0, 8.0) : 0.0;
            accumulatedBroadbandPower_ += sample * sample;
            for (std::size_t band = 0; band < kAmbientBandCount; ++band) {
                const auto filtered = filters_[channel][band].step(sample);
                accumulatedBandPower_[band] += filtered * filtered;
            }
        }
        ++nextFrame_;
        if (++framesInWindow_ == framesPerWindow_) finishWindow();
    }
    return true;
}

void AmbientFeaturesModel::finishWindow() noexcept
{
    const auto duration = static_cast<double>(framesInWindow_) / sampleRate_;
    const auto divisor = static_cast<double>(framesInWindow_) * channels_;
    snapshot_.broadbandDb = static_cast<float>(
        powerDb(accumulatedBroadbandPower_ / divisor));
    if (snapshot_.broadbandDb >= -58.0f) {
        audible_ = true;
        quietSeconds_ = 0.0;
    } else if (snapshot_.broadbandDb <= -66.0f) {
        quietSeconds_ += duration;
        if (quietSeconds_ >= 0.12) audible_ = false;
    } else {
        quietSeconds_ = 0.0;
    }
    gate_ = follow(gate_, audible_ ? 1.0 : 0.0,
        duration, 0.12, 0.32);
    constexpr std::array<double, kAmbientBandCount> attack{
        0.065, 0.095, 0.035};
    constexpr std::array<double, kAmbientBandCount> release{
        0.45, 0.60, 0.18};
    for (std::size_t band = 0; band < kAmbientBandCount; ++band) {
        const auto db = powerDb(accumulatedBandPower_[band] / divisor);
        snapshot_.bandPower[band] = accumulatedBandPower_[band] / divisor;
        snapshot_.bandDb[band] = static_cast<float>(db);
        if (!baselineReady_) {
            slowBandDb_[band] = db;
            fastBandDb_[band] = db;
            previousFastBandDb_[band] = db;
            relativeEnvelope_[band] = 1.0;
        }
        fastBandDb_[band] = follow(fastBandDb_[band], db, duration,
            0.025, 0.090);
        const auto relativeFast = std::clamp(std::pow(10.0,
            (fastBandDb_[band] - slowBandDb_[band]) / 20.0), 0.0, 4.0);
        relativeEnvelope_[band] = follow(relativeEnvelope_[band],
            relativeFast, duration, 0.18, 0.45);
        const auto growthDb = fastBandDb_[band] - previousFastBandDb_[band];
        const auto growth = std::clamp(growthDb / 6.0, -1.0, 1.0);
        const auto attackTarget = audible_
            ? smooth(0.08, 0.85, std::max(0.0, growth)) : 0.0;
        attackEnvelope_[band] = follow(attackEnvelope_[band], attackTarget,
            duration, 0.012, 0.11);
        previousFastBandDb_[band] = fastBandDb_[band];
        const auto novelty = smooth(1.5, 9.0, db - slowBandDb_[band]) *
            smooth(-65.0, -42.0, db);
        slowBandDb_[band] = follow(
            slowBandDb_[band], db, duration, 1.8, 1.8);
        const auto level = smooth(-55.0, -14.0, db);
        const auto target = audible_
            ? 0.65 * level + 0.35 * novelty : 0.0;
        envelopes_[band] = follow(envelopes_[band], target, duration,
            attack[band], release[band]);
        snapshot_.level[band] = static_cast<float>(
            unit(envelopes_[band] * gate_));
        snapshot_.absoluteEnergy[band] = static_cast<float>(
            unit(gate_ * smooth(-70.0, -12.0, db)));
        snapshot_.relativeFast[band] = static_cast<float>(relativeFast);
        snapshot_.relativeSlow[band] = static_cast<float>(std::clamp(
            relativeEnvelope_[band], 0.0, 4.0));
        snapshot_.growth[band] = static_cast<float>(growth);
        snapshot_.attack[band] = static_cast<float>(
            unit(gate_ * attackEnvelope_[band]));
        accumulatedBandPower_[band] = 0.0;
    }
    baselineReady_ = true;
    snapshot_.gate = static_cast<float>(unit(gate_));
    snapshot_.firstFrame = windowFirstFrame_;
    snapshot_.endFrame = nextFrame_;
    snapshot_.sourceSequence = latestSourceSequence_;
    snapshot_.sourceEndQpc = latestSourceEndQpc_;
    snapshot_.flags = kAmbientFeatureValid |
        (snapshot_.publicationRevision == 0
            ? kAmbientFeatureDiscontinuity : 0U);
    ++snapshot_.publicationRevision;
    ++completedWindows_;
    windowFirstFrame_ = nextFrame_;
    accumulatedBroadbandPower_ = 0.0;
    framesInWindow_ = 0;
}

bool AlbumAmbientEligibility::wantsAudio() const noexcept
{
    return visible && playing && !muted && hardware && hasArtwork &&
        albumMode && !bookletOpen && !scrubbing && !dawPreview &&
        !outsideSession && !reducedMotion && mode != AlbumAmbientMode::off;
}

std::array<float, kAmbientBandCount> ambientTail(
    std::array<float, kAmbientBandCount> start,
    double elapsedSeconds) noexcept
{
    const auto elapsed = std::max(0.0,
        std::isfinite(elapsedSeconds) ? elapsedSeconds : 2.0);
    const auto decay = elapsed >= 2.0 ? 0.0 : std::exp(-elapsed / 0.32);
    for (auto& value : start) {
        value = static_cast<float>(unit(value) * decay);
    }
    return start;
}

bool ambientSampleFresh(double ageSeconds) noexcept
{
    return std::isfinite(ageSeconds) && ageSeconds >= -0.01 &&
        ageSeconds <= 0.20;
}

double ambientBackgroundHz(double displayHz) noexcept
{
    if (!std::isfinite(displayHz) || displayHz <= 0.0) return 30.0;
    return displayHz / std::max(1.0,
        std::ceil(displayHz / 30.0 - 1.0e-10));
}

AmbientTextureSize fitAmbientTexture(std::uint32_t width,
    std::uint32_t height, bool degraded) noexcept
{
    if (width == 0 || height == 0) return {};
    const auto maxWidth = degraded ? 320.0 : 512.0;
    const auto maxHeight = degraded ? 180.0 : 288.0;
    const auto scale = std::min({1.0, maxWidth / width, maxHeight / height});
    return {
        std::max(1U, static_cast<std::uint32_t>(std::floor(width * scale))),
        std::max(1U, static_cast<std::uint32_t>(std::floor(height * scale))),
    };
}

} // namespace nekorox
