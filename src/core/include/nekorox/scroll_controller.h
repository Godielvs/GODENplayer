#pragma once

#include <cstddef>
#include <cstdint>

namespace nekorox {

struct ScrollConfig {
    double settleMilliseconds = 105.0;
    double minimumSettleMilliseconds = 70.0;
    double maximumSettleMilliseconds = 140.0;
    double velocityEwmaAlpha = 0.28;
    double inputResetMilliseconds = 85.0;
    double nominalInputMilliseconds = 16.0;
    // Share of the pending gap carried into the rendered offset by a delta that
    // continues an in-flight gesture. Keeps a wheel burst one continuous
    // movement instead of one independent easing per notch.
    double inputLeadFraction = 0.22;
    double predictionMilliseconds = 90.0;
    double settlePositionEpsilon = 0.08;
    double settleVelocityEpsilon = 1.0;
    std::size_t baseOverscanRows = 5;
    std::size_t maximumOverscanRows = 48;
};

class ScrollController final {
public:
    ScrollController(ScrollConfig config = {});

    void setBounds(double minimum, double maximum) noexcept;
    void setPosition(double position) noexcept;
    void setResponseScale(double scale) noexcept;
    void addWheelImpulse(double pixels, double secondsSinceLastEvent) noexcept;
    bool addWheelImpulseAt(double pixels, double nowSeconds) noexcept;
    bool update(double deltaSeconds) noexcept;
    bool advanceTo(double nowSeconds) noexcept;

    double position() const noexcept { return position_; }
    double target() const noexcept { return target_; }
    double velocity() const noexcept { return velocity_; }
    bool active() const noexcept { return active_; }
    std::uint64_t visualRevision() const noexcept { return visualRevision_; }
    std::size_t overscanRows(double rowHeight) const noexcept;

private:
    double clamped(double value) const noexcept;
    double omega() const noexcept;
    double adaptiveSettleMilliseconds() const noexcept;
    bool moving() const noexcept;
    void cancelOutwardVelocity() noexcept;

    ScrollConfig config_;
    double minimum_ = 0.0;
    double maximum_ = 0.0;
    double position_ = 0.0;
    double target_ = 0.0;
    double velocity_ = 0.0;
    double inputVelocityEwma_ = 0.0;
    double responseScale_ = 1.0;
    double timeSeconds_ = 0.0;
    std::uint64_t visualRevision_ = 0;
    bool timeInitialized_ = false;
    bool legacyMode_ = false;
    bool active_ = false;
    bool leadArmed_ = true;
};

} // namespace nekorox
