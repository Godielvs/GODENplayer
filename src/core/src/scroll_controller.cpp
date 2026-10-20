#include "nekorox/scroll_controller.h"

#include <algorithm>
#include <cmath>

namespace nekorox {
namespace {
constexpr double kPositionEpsilon = 0.1;
constexpr double kVelocityEpsilon = 2.0;
}

ScrollController::ScrollController(ScrollConfig config) : config_(config) {}

void ScrollController::setBounds(double minimum, double maximum) noexcept
{
    if (!std::isfinite(minimum) || !std::isfinite(maximum)) return;
    minimum_ = std::min(minimum, maximum);
    maximum_ = std::max(minimum, maximum);
    const auto before = position_;
    position_ = clamped(position_);
    target_ = clamped(target_);
    cancelOutwardVelocity();
    active_ = moving();
    if (position_ != before) ++visualRevision_;
}

void ScrollController::setPosition(double position) noexcept
{
    if (!std::isfinite(position)) return;
    const auto next = clamped(position);
    if (next != position_) ++visualRevision_;
    position_ = next;
    target_ = next;
    velocity_ = 0.0;
    inputVelocityEwma_ = 0.0;
    active_ = false;
    leadArmed_ = true;
}

void ScrollController::setResponseScale(double scale) noexcept
{
    responseScale_ = std::clamp(scale, 0.0, 1.0);
    if (responseScale_ == 0.0 && moving()) {
        if (position_ != target_) ++visualRevision_;
        position_ = target_;
        velocity_ = 0.0;
        active_ = false;
    }
}

void ScrollController::addWheelImpulse(
    double pixels,
    double secondsSinceLastEvent) noexcept
{
    legacyMode_ = true;
    if (!std::isfinite(pixels) || pixels == 0.0) return;
    const bool newGesture = !active_ || !std::isfinite(secondsSinceLastEvent) ||
        secondsSinceLastEvent * 1000.0 >= config_.inputResetMilliseconds;
    const auto eventSeconds = newGesture
        ? config_.nominalInputMilliseconds / 1000.0
        : std::clamp(secondsSinceLastEvent, 0.004, 0.035);
    const auto instantaneous = pixels / eventSeconds;
    const auto pending = target_ - position_;
    const bool reversing =
        (pending != 0.0 && pending * pixels < 0.0) ||
        (velocity_ != 0.0 && velocity_ * pixels < 0.0);
    if (newGesture || reversing || inputVelocityEwma_ * instantaneous <= 0.0) {
        inputVelocityEwma_ = instantaneous;
    } else {
        inputVelocityEwma_ = std::lerp(
            inputVelocityEwma_, instantaneous, config_.velocityEwmaAlpha);
    }
    if (reversing) {
        target_ = position_;
        velocity_ = 0.0;
    }
    target_ = clamped(target_ + pixels);
    if (responseScale_ == 0.0) {
        if (position_ != target_) ++visualRevision_;
        position_ = target_;
        velocity_ = 0.0;
        inputVelocityEwma_ = 0.0;
        active_ = false;
        return;
    }
    const auto lead = std::lerp(1.0,
        std::clamp(config_.inputLeadFraction, 0.0, 1.0), responseScale_);
    const auto before = position_;
    position_ = clamped(position_ + (target_ - position_) * lead);
    if (position_ != before) ++visualRevision_;
    active_ = std::abs(position_ - target_) > config_.settlePositionEpsilon ||
        std::abs(velocity_) > config_.settleVelocityEpsilon;
}

bool ScrollController::addWheelImpulseAt(
    double pixels,
    double nowSeconds) noexcept
{
    if (!std::isfinite(pixels) || pixels == 0.0 ||
        !std::isfinite(nowSeconds)) return false;
    legacyMode_ = false;
    bool advancedTime = false;
    // An input cycle is not a presented frame: the lead below may carry the
    // rendered offset forward at most once per frame interval, so a denser
    // wheel or touchpad stream cannot inflate the per-frame step.
    const auto frameLeadArmed = leadArmed_;
    if (!timeInitialized_) {
        timeSeconds_ = nowSeconds;
        timeInitialized_ = true;
    } else if (nowSeconds < timeSeconds_) {
        return false;
    } else {
        advancedTime = nowSeconds > timeSeconds_;
        advanceTo(nowSeconds);
    }
    leadArmed_ = frameLeadArmed;

    // A delta that arrives while an earlier motion is still being presented can
    // otherwise only reach the screen through the spring, so a wheel burst
    // renders as one independent easing per notch and the rendered offset
    // trails the accumulated target until the input stops. Continue an
    // in-flight gesture the way the legacy input path did: carry the approved
    // lead fraction of the pending gap into the rendered offset on the same
    // frame cycle. The first delta of a gesture and any direction reversal keep
    // the existing contract that input moves only the modelled target.
    const auto continuingGesture = advancedTime && leadArmed_ && moving();

    const auto debt = target_ - position_;
    const auto intent = std::abs(debt) >= kPositionEpsilon
        ? debt : (std::abs(velocity_) >= kVelocityEpsilon ? velocity_ : 0.0);
    const auto reversing =
        intent != 0.0 && std::signbit(intent) != std::signbit(pixels);
    if (reversing) {
        target_ = position_;
        velocity_ = 0.0;
    }
    target_ = clamped(target_ + pixels);
    if (responseScale_ == 0.0) {
        if (position_ != target_) ++visualRevision_;
        position_ = target_;
        velocity_ = 0.0;
        active_ = false;
        return true;
    }
    if (continuingGesture && !reversing) {
        const auto lead = std::lerp(1.0,
            std::clamp(config_.inputLeadFraction, 0.0, 1.0), responseScale_);
        const auto before = position_;
        position_ = clamped(position_ + (target_ - position_) * lead);
        if (position_ != before) ++visualRevision_;
        leadArmed_ = false;
    }
    active_ = moving();
    return active_;
}

bool ScrollController::update(double deltaSeconds) noexcept
{
    if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0) return false;
    if (legacyMode_) {
        if (!active_) return false;
        const auto dt = std::clamp(deltaSeconds, 1.0 / 1000.0, 0.05);
        const auto settleScale = std::lerp(0.35, 1.0, responseScale_);
        const auto settleSeconds = adaptiveSettleMilliseconds() *
            settleScale / 1000.0;
        const auto spring = 4.6 / std::max(0.001, settleSeconds);
        const auto displacement = position_ - target_;
        const auto helper = velocity_ + spring * displacement;
        const auto decay = std::exp(-spring * dt);
        const auto before = position_;
        position_ = clamped(target_ + (displacement + helper * dt) * decay);
        velocity_ = (velocity_ - spring * helper * dt) * decay;
        inputVelocityEwma_ *= std::exp(-dt * 12.0);
        cancelOutwardVelocity();
        if (std::abs(position_ - target_) <= config_.settlePositionEpsilon &&
            std::abs(velocity_) <= config_.settleVelocityEpsilon) {
            position_ = target_;
            velocity_ = 0.0;
            inputVelocityEwma_ = 0.0;
            active_ = false;
        }
        if (position_ != before) ++visualRevision_;
        return active_;
    }
    if (!timeInitialized_) {
        timeInitialized_ = true;
        timeSeconds_ = 0.0;
    }
    return advanceTo(timeSeconds_ + deltaSeconds);
}

bool ScrollController::advanceTo(double nowSeconds) noexcept
{
    if (!std::isfinite(nowSeconds)) return false;
    if (!timeInitialized_) {
        timeInitialized_ = true;
        timeSeconds_ = nowSeconds;
        return false;
    }
    if (nowSeconds <= timeSeconds_) return false;
    const auto dt = nowSeconds - timeSeconds_;
    timeSeconds_ = nowSeconds;
    // A presented-frame interval began; the next input may lead once.
    leadArmed_ = true;
    if (!moving()) {
        active_ = false;
        return false;
    }

    const auto before = position_;
    const auto spring = omega();
    if (responseScale_ == 0.0 || spring * dt > 60.0) {
        position_ = target_;
        velocity_ = 0.0;
    } else {
        const auto displacement = position_ - target_;
        const auto helper = velocity_ + spring * displacement;
        const auto decay = std::exp(-spring * dt);
        position_ = clamped(target_ + (displacement + helper * dt) * decay);
        velocity_ = (velocity_ - spring * helper * dt) * decay;
        cancelOutwardVelocity();
        if (std::abs(target_ - position_) < kPositionEpsilon &&
            std::abs(velocity_) < kVelocityEpsilon) {
            position_ = target_;
            velocity_ = 0.0;
        }
    }
    if (position_ != before) ++visualRevision_;
    active_ = moving();
    return position_ != before;
}

std::size_t ScrollController::overscanRows(double rowHeight) const noexcept
{
    if (!std::isfinite(rowHeight) || rowHeight <= 0.0) {
        return config_.baseOverscanRows;
    }
    const auto predictedPixels =
        std::abs(velocity_) * (config_.predictionMilliseconds / 1000.0);
    const auto predictedRows = static_cast<std::size_t>(
        std::ceil(predictedPixels / rowHeight));
    return std::clamp(
        config_.baseOverscanRows + predictedRows,
        config_.baseOverscanRows,
        config_.maximumOverscanRows);
}

double ScrollController::clamped(double value) const noexcept
{
    return std::clamp(value, minimum_, maximum_);
}

double ScrollController::omega() const noexcept
{
    return responseScale_ < 1.0 ? 80.0 : 32.0;
}

double ScrollController::adaptiveSettleMilliseconds() const noexcept
{
    const auto normalized =
        std::clamp(std::abs(inputVelocityEwma_) / 9000.0, 0.0, 1.0);
    return std::clamp(
        std::lerp(config_.settleMilliseconds,
            config_.minimumSettleMilliseconds, normalized),
        config_.minimumSettleMilliseconds,
        config_.maximumSettleMilliseconds);
}

bool ScrollController::moving() const noexcept
{
    return position_ != target_ || velocity_ != 0.0;
}

void ScrollController::cancelOutwardVelocity() noexcept
{
    if ((position_ <= minimum_ && velocity_ < 0.0) ||
        (position_ >= maximum_ && velocity_ > 0.0) ||
        minimum_ == maximum_) velocity_ = 0.0;
}

} // namespace nekorox
