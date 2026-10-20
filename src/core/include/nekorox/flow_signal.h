#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace nekorox {

struct FlowBandFeatures {
    std::array<double, 3> power{};
    double sourceSeconds = 0.0; // monotonic QPC seconds, NOT playback position
    std::uint64_t playbackSerial = 0;
    std::array<float, 3> absoluteEnergy{};
    std::array<float, 3> relativeFast{1.0f, 1.0f, 1.0f};
    std::array<float, 3> relativeSlow{1.0f, 1.0f, 1.0f};
    std::array<float, 3> growth{};
    std::array<float, 3> attack{};
    float gate = 0.0f;
    bool rich = false;
};

struct FlowPose {
    std::array<float, 3> level{};
    std::array<float, 3> transient{};
    std::array<float, 3> phase{};
    std::array<float, 3> absoluteEnergy{};
    std::array<float, 3> relativeFast{1.0f, 1.0f, 1.0f};
    std::array<float, 3> relativeSlow{1.0f, 1.0f, 1.0f};
    std::array<float, 3> growth{};
    std::array<float, 3> attack{};
    float availability = 0.0f;
    float gate = 0.0f;
    bool needsFrame = false;
};

// One audio response; sampling is pure. Transport discontinuities invalidate
// samples, not the visual phase. The A3.1 and A4 shaders use these SAME angles.
class FlowSignal final {
public:
    bool ingest(const FlowBandFeatures& features) noexcept
    {
        if (features.playbackSerial == 0 ||
            !std::isfinite(features.sourceSeconds) || features.sourceSeconds < 0.0)
            return false;
        if (session_ != 0 && features.playbackSerial < session_) return false;
        if (features.playbackSerial == session_ && lastSource_ >= 0.0 &&
            features.sourceSeconds <= lastSource_) return false;
        for (const auto value : features.power)
            if (!std::isfinite(value) || value < 0.0) return false;
        if (features.rich) {
            if (!std::isfinite(features.gate) || features.gate < 0.0f ||
                features.gate > 1.0f) return false;
            for (std::size_t band = 0; band < 3; ++band) {
                if (!std::isfinite(features.absoluteEnergy[band]) ||
                    features.absoluteEnergy[band] < 0.0f ||
                    features.absoluteEnergy[band] > 1.0f ||
                    !std::isfinite(features.relativeFast[band]) ||
                    features.relativeFast[band] < 0.0f ||
                    features.relativeFast[band] > 4.0f ||
                    !std::isfinite(features.relativeSlow[band]) ||
                    features.relativeSlow[band] < 0.0f ||
                    features.relativeSlow[band] > 4.0f ||
                    !std::isfinite(features.growth[band]) ||
                    features.growth[band] < -1.0f ||
                    features.growth[band] > 1.0f ||
                    !std::isfinite(features.attack[band]) ||
                    features.attack[band] < 0.0f ||
                    features.attack[band] > 1.0f) return false;
            }
        }

        const auto previousSource = lastSource_;
        if (lastSource_ >= 0.0 || suspended_) {
            // Commit the exact phase already visible at the source timestamp.
            // A large source gap is sampled through the bounded stale tail;
            // it is never integrated as seconds of imaginary fresh audio.
            const auto prior = sample(features.sourceSeconds, true, true, false);
            for (std::size_t band = 0; band < 3; ++band) {
                levels_[band] = prior.level[band];
                transients_[band] = prior.transient[band];
                phases_[band] = prior.phase[band];
            }
            availability_ = prior.availability;
        }
        if (features.playbackSerial != session_) {
            session_ = features.playbackSerial;
            initialized_.fill(false);
            targets_.fill(0.0);
            hits_.fill(0.0);
            featureAbsolute_.fill(0.0);
            featureRelativeFast_.fill(1.0);
            featureRelativeSlow_.fill(1.0);
            featureGrowth_.fill(0.0);
            featureAttack_.fill(0.0);
            featureGate_ = 0.0;
            // Deliberately preserve phase and the currently visible envelope.
        }
        const auto dt = previousSource >= 0.0 && !suspended_
            ? std::clamp(features.sourceSeconds - previousSource, 0.0, 0.25)
            : 0.02;
        suspended_ = false;
        lastSource_ = features.sourceSeconds;
        for (std::size_t band = 0; band < 3; ++band) {
            ingestBand(band, features.power[band], dt);
            if (features.rich) {
                const auto relative = unit(
                    std::max(0.0f, features.relativeFast[band] - 1.0f) / 1.5f);
                const auto slowRelative = unit(
                    std::max(0.0f, features.relativeSlow[band] - 1.0f) / 1.5f);
                const auto positiveGrowth = unit(features.growth[band]);
                targets_[band] = features.gate * unit(
                    0.55 * features.absoluteEnergy[band] +
                    0.25 * relative + 0.10 * slowRelative +
                    0.10 * positiveGrowth);
                hits_[band] = features.gate * unit(
                    0.78 * features.attack[band] + 0.22 * positiveGrowth);
                featureAbsolute_[band] = features.absoluteEnergy[band];
                featureRelativeFast_[band] = features.relativeFast[band];
                featureRelativeSlow_[band] = features.relativeSlow[band];
                featureGrowth_[band] = features.growth[band];
                featureAttack_[band] = features.attack[band];
            }
        }
        if (features.rich) featureGate_ = features.gate;
        return true;
    }

    [[nodiscard]] FlowPose sample(double now, bool playing,
        bool visible, bool muted) const noexcept
    {
        FlowPose result;
        for (std::size_t band = 0; band < 3; ++band)
            result.phase[band] = static_cast<float>(phases_[band]);
        if (!std::isfinite(now)) return result;
        const auto anchor = suspended_ ? suspendedAt_ : lastSource_;
        if (anchor < 0.0 || now < anchor) return result;
        const auto age = now - anchor;
        const auto live = playing && visible && !muted && !suspended_;
        const auto freshDt = live ? std::min(age, 0.250) : 0.0;
        const auto tailDt = live ? std::max(0.0, age - 0.250) : age;
        const auto featureTail = std::exp(-tailDt / 0.18);
        const auto present = targets_[0] + targets_[1] + targets_[2] > 0.002;
        const auto availabilityTarget = present ? 1.0 : 0.0;
        auto available = approach(availability_, availabilityTarget, freshDt,
            present ? 0.09 : 0.13);
        const auto meanAvailable = 0.5 * (availability_ + available);

        for (std::size_t band = 0; band < 3; ++band) {
            auto level = approach(levels_[band], targets_[band], freshDt,
                targets_[band] > levels_[band] ? attack_[band] : release_[band]);
            auto hit = approach(transients_[band], hits_[band], freshDt,
                hits_[band] > transients_[band] ? attack_[band] : release_[band]);
            const auto meanLevel = 0.5 * (levels_[band] + level);
            const auto meanHit = 0.5 * (transients_[band] + hit);
            result.phase[band] = static_cast<float>(wrap(phases_[band] +
                freshDt * meanAvailable * (phaseBase_[band] +
                    phaseGain_[band] * meanLevel + 0.45 * meanHit)));
            if (tailDt > 0.0) {
                level *= std::exp(-tailDt / release_[band]);
                hit *= std::exp(-tailDt / release_[band]);
            }
            result.level[band] = static_cast<float>(unit(level));
            result.transient[band] = static_cast<float>(unit(hit));
            result.absoluteEnergy[band] = static_cast<float>(
                unit(featureAbsolute_[band] * featureTail));
            result.relativeFast[band] = static_cast<float>(std::clamp(
                1.0 + (featureRelativeFast_[band] - 1.0) * featureTail,
                0.0, 4.0));
            result.relativeSlow[band] = static_cast<float>(std::clamp(
                1.0 + (featureRelativeSlow_[band] - 1.0) * featureTail,
                0.0, 4.0));
            result.growth[band] = static_cast<float>(std::clamp(
                featureGrowth_[band] * featureTail, -1.0, 1.0));
            result.attack[band] = static_cast<float>(
                unit(featureAttack_[band] * featureTail));
        }
        if (tailDt > 0.0) available *= std::exp(-tailDt / 0.13);
        result.availability = static_cast<float>(unit(available));
        result.gate = static_cast<float>(unit(featureGate_ * featureTail));
        if (age >= 1.5 || (!present && result.availability < 1.0f / 4096.0f &&
                *std::max_element(result.level.begin(), result.level.end()) <
                    1.0f / 4096.0f)) {
            result.level.fill(0.0f);
            result.transient.fill(0.0f);
            result.absoluteEnergy.fill(0.0f);
            result.relativeFast.fill(1.0f);
            result.relativeSlow.fill(1.0f);
            result.growth.fill(0.0f);
            result.attack.fill(0.0f);
            result.availability = 0.0f;
            result.gate = 0.0f;
        }
        result.needsFrame = result.availability > 1.0f / 4096.0f ||
            *std::max_element(result.level.begin(), result.level.end()) >
                1.0f / 4096.0f;
        return result;
    }

    // Seal the visible pose once. Repeated hidden notifications are idempotent.
    void suspend(double now) noexcept
    {
        if (suspended_ || !std::isfinite(now)) return;
        const auto current = sample(now, true, true, false);
        for (std::size_t band = 0; band < 3; ++band) {
            levels_[band] = current.level[band];
            transients_[band] = current.transient[band];
            phases_[band] = current.phase[band];
        }
        availability_ = current.availability;
        targets_.fill(0.0);
        hits_.fill(0.0);
        suspendedAt_ = now;
        suspended_ = true;
    }

    // Hard reset is reserved for object lifetime / explicit test initialization.
    void reset() noexcept { *this = FlowSignal{}; }

private:
    static constexpr std::array<double, 3> attack_{0.028, 0.045, 0.016};
    static constexpr std::array<double, 3> release_{0.22, 0.30, 0.11};
    static constexpr std::array<double, 3> phaseBase_{0.11, 0.20, 0.34};
    static constexpr std::array<double, 3> phaseGain_{0.40, 0.68, 0.95};
    static double approach(double oldValue, double target, double dt,
        double tau) noexcept
    {
        return oldValue + (target - oldValue) * (-std::expm1(-dt / tau));
    }
    static double unit(double value) noexcept { return std::clamp(value, 0.0, 1.0); }
    static double wrap(double angle) noexcept
    {
        constexpr double twoPi = 6.2831853071795864769;
        const auto value = std::fmod(angle, twoPi);
        return value < 0.0 ? value + twoPi : value;
    }
    static double smoothUnit(double value) noexcept
    {
        value = unit(value);
        return value * value * (3.0 - 2.0 * value);
    }
    void ingestBand(std::size_t band, double power, double dt) noexcept
    {
        if (power <= 1.0e-12) {
            targets_[band] = hits_[band] = 0.0;
            return;
        }
        const auto db = std::clamp(10.0 * std::log10(power), -120.0, 12.0);
        const auto gate = smoothUnit((db + 72.0) / 14.0);
        if (!initialized_[band]) {
            mean_[band] = fast_[band] = db;
            deviation_[band] = 3.0;
            initialized_[band] = true;
        }
        fast_[band] = approach(fast_[band], db, dt,
            db > fast_[band] ? 0.020 : 0.085);
        const auto relative = (fast_[band] - mean_[band]) /
            std::max(3.0, deviation_[band]);
        const auto absolute = smoothUnit((db + 78.0) / 58.0);
        const auto activity = unit(0.45 + 0.55 * std::tanh(relative * 1.35));
        targets_[band] = gate * unit(0.40 * absolute + 0.60 * activity);
        hits_[band] = gate * smoothUnit((fast_[band] - mean_[band] - 0.65) /
            std::max(2.5, 1.5 * deviation_[band]));
        deviation_[band] = approach(deviation_[band],
            std::abs(db - mean_[band]), dt, 4.0);
        mean_[band] = approach(mean_[band], db, dt, 3.0);
    }

    std::array<double, 3> levels_{}, transients_{}, targets_{}, hits_{}, phases_{};
    std::array<double, 3> mean_{}, deviation_{}, fast_{};
    std::array<double, 3> featureAbsolute_{}, featureRelativeFast_{1.0,1.0,1.0};
    std::array<double, 3> featureRelativeSlow_{1.0,1.0,1.0}, featureGrowth_{}, featureAttack_{};
    std::array<bool, 3> initialized_{};
    std::uint64_t session_ = 0;
    double lastSource_ = -1.0;
    double suspendedAt_ = -1.0;
    double availability_ = 0.0;
    double featureGate_ = 0.0;
    bool suspended_ = false;
};

} // namespace nekorox
