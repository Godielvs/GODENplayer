#include "coverflow_model.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nekorox::component {
namespace {

bool finiteRect(const CoverflowRect& rect) noexcept
{
    return std::isfinite(rect.x) &&
        std::isfinite(rect.y) &&
        std::isfinite(rect.width) &&
        std::isfinite(rect.height) &&
        rect.width > 0.0 &&
        rect.height > 0.0;
}

double sign(double value) noexcept
{
    return value < 0.0 ? -1.0 : value > 0.0 ? 1.0 : 0.0;
}

} // namespace

CoverflowModel::CoverflowModel(CoverflowConfig config) : config_(config)
{
    config_.visibleNeighbors =
        std::clamp<std::size_t>(config_.visibleNeighbors, 1, 12);
    config_.pageStep = std::max<std::size_t>(config_.pageStep, 1);
    config_.centerWidthFraction =
        std::clamp(config_.centerWidthFraction, 0.1, 1.0);
    config_.centerHeightFraction =
        std::clamp(config_.centerHeightFraction, 0.1, 1.0);
    config_.neighborStepFraction =
        std::clamp(config_.neighborStepFraction, 0.02, 1.0);
    config_.neighborScale = std::clamp(config_.neighborScale, 0.1, 1.0);
    config_.neighborAngleDegrees =
        std::clamp(config_.neighborAngleDegrees, 0.0, 89.0);
    config_.minimumOpacity = std::clamp(config_.minimumOpacity, 0.0, 1.0);
    config_.dragPixelsPerItem =
        std::max(config_.dragPixelsPerItem, 1.0);
    config_.normalSettleMilliseconds =
        std::max(config_.normalSettleMilliseconds, 1.0);
    config_.reducedTransitionMilliseconds =
        std::max(config_.reducedTransitionMilliseconds, 1.0);
}

void CoverflowModel::setItemCount(std::size_t itemCount) noexcept
{
    itemCount_ = itemCount;
    position_ = clampedPosition(position_);
    target_ = clampedPosition(target_);
    if (itemCount_ == 0) {
        position_ = 0.0;
        target_ = 0.0;
        velocity_ = 0.0;
        wheelRemainder_ = 0.0;
        dragging_ = false;
        spatialAnimationActive_ = false;
        reducedTransitionActive_ = false;
        transitionProgress_ = 1.0;
    }
}

void CoverflowModel::setSelectedIndex(
    std::size_t index,
    bool animate) noexcept
{
    if (itemCount_ == 0) {
        return;
    }
    target_ = clampedPosition(static_cast<double>(index));
    if (!animate || motionMode_ != CoverflowMotionMode::normal) {
        position_ = target_;
        velocity_ = 0.0;
    }
    beginMotion();
}

void CoverflowModel::setMotionMode(CoverflowMotionMode mode) noexcept
{
    if (motionMode_ == mode) {
        return;
    }
    motionMode_ = mode;
    velocity_ = 0.0;
    spatialAnimationActive_ = false;
    if (mode == CoverflowMotionMode::normal) {
        position_ = clampedPosition(position_);
        transitionProgress_ = 1.0;
        reducedTransitionActive_ = false;
    } else {
        position_ = target_;
        if (mode == CoverflowMotionMode::reduced) {
            transitionProgress_ = 0.0;
            reducedTransitionActive_ = true;
        } else {
            transitionProgress_ = 1.0;
            reducedTransitionActive_ = false;
        }
    }
}

std::size_t CoverflowModel::selectedIndex() const noexcept
{
    if (itemCount_ == 0) {
        return 0;
    }
    return static_cast<std::size_t>(
        std::llround(clampedPosition(target_)));
}

bool CoverflowModel::navigate(CoverflowNavigationKey key) noexcept
{
    if (itemCount_ == 0) {
        return false;
    }
    const double before = target_;
    switch (key) {
    case CoverflowNavigationKey::previous:
        moveTarget(-1.0);
        break;
    case CoverflowNavigationKey::next:
        moveTarget(1.0);
        break;
    case CoverflowNavigationKey::first:
        target_ = 0.0;
        beginMotion();
        break;
    case CoverflowNavigationKey::last:
        target_ = static_cast<double>(itemCount_ - 1);
        beginMotion();
        break;
    case CoverflowNavigationKey::pagePrevious:
        moveTarget(-static_cast<double>(config_.pageStep));
        break;
    case CoverflowNavigationKey::pageNext:
        moveTarget(static_cast<double>(config_.pageStep));
        break;
    }
    return target_ != before;
}

bool CoverflowModel::addWheelDelta(double wheelDelta) noexcept
{
    if (itemCount_ == 0 || !std::isfinite(wheelDelta) || wheelDelta == 0.0) {
        return false;
    }
    wheelRemainder_ += wheelDelta;
    const double detents = std::trunc(wheelRemainder_ / 120.0);
    if (detents == 0.0) {
        return false;
    }
    wheelRemainder_ -= detents * 120.0;
    const double before = target_;
    moveTarget(-detents);
    return target_ != before;
}

void CoverflowModel::beginDrag(double pointerX) noexcept
{
    if (itemCount_ == 0 || !std::isfinite(pointerX)) {
        return;
    }
    dragging_ = true;
    dragStartX_ = pointerX;
    dragStartPosition_ = position_;
    target_ = position_;
    velocity_ = 0.0;
    spatialAnimationActive_ = false;
    reducedTransitionActive_ = false;
    transitionProgress_ = 1.0;
}

bool CoverflowModel::updateDrag(double pointerX) noexcept
{
    if (!dragging_ || !std::isfinite(pointerX)) {
        return false;
    }
    const double before = position_;
    const double delta = (pointerX - dragStartX_) / config_.dragPixelsPerItem;
    position_ = clampedPosition(dragStartPosition_ - delta);
    target_ = position_;
    return position_ != before;
}

bool CoverflowModel::endDrag(double velocityPixelsPerSecond) noexcept
{
    if (!dragging_) {
        return false;
    }
    dragging_ = false;
    const double projectedItems = std::isfinite(velocityPixelsPerSecond)
        ? -velocityPixelsPerSecond * 0.12 / config_.dragPixelsPerItem
        : 0.0;
    target_ = clampedPosition(std::round(position_ + projectedItems));
    beginMotion();
    return true;
}

void CoverflowModel::cancelDrag() noexcept
{
    if (!dragging_) {
        return;
    }
    dragging_ = false;
    target_ = clampedPosition(std::round(dragStartPosition_));
    beginMotion();
}

bool CoverflowModel::update(double deltaSeconds) noexcept
{
    if (dragging_) {
        return true;
    }
    if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0) {
        return spatialAnimationActive_ || reducedTransitionActive_;
    }

    if (reducedTransitionActive_) {
        transitionProgress_ = std::min(
            1.0,
            transitionProgress_ +
                deltaSeconds * 1000.0 /
                    config_.reducedTransitionMilliseconds);
        reducedTransitionActive_ = transitionProgress_ < 1.0;
    }

    if (spatialAnimationActive_) {
        const double dt = std::clamp(deltaSeconds, 1.0 / 1000.0, 0.05);
        const double settleSeconds =
            config_.normalSettleMilliseconds / 1000.0;
        const double omega = 4.6 / settleSeconds;
        const double displacement = position_ - target_;
        const double helper = velocity_ + omega * displacement;
        const double decay = std::exp(-omega * dt);

        const double nextDisplacement =
            (displacement + helper * dt) * decay;
        velocity_ = (velocity_ - omega * helper * dt) * decay;
        position_ = clampedPosition(target_ + nextDisplacement);

        if (std::abs(position_ - target_) <= 0.0005 &&
            std::abs(velocity_) <= 0.005) {
            position_ = target_;
            velocity_ = 0.0;
            spatialAnimationActive_ = false;
        }
    }
    return spatialAnimationActive_ || reducedTransitionActive_;
}

std::vector<CoverflowItemGeometry> CoverflowModel::layout(
    CoverflowRect viewport) const
{
    std::vector<CoverflowItemGeometry> result;
    if (itemCount_ == 0 || !finiteRect(viewport)) {
        return result;
    }

    const auto radius = static_cast<long long>(config_.visibleNeighbors) + 1;
    const auto centerIndex = static_cast<long long>(std::floor(position_));
    const auto first = std::max(0LL, centerIndex - radius);
    const auto last = std::min(
        static_cast<long long>(itemCount_ - 1),
        centerIndex + radius + 1);
    result.reserve(static_cast<std::size_t>(last - first + 1));

    const double centerSize = std::min(
        viewport.width * config_.centerWidthFraction,
        viewport.height * config_.centerHeightFraction);
    const double centerX = viewport.x + viewport.width * 0.5;
    const double centerY = viewport.y + viewport.height * 0.5;
    const double step = viewport.width * config_.neighborStepFraction;

    for (auto index = first; index <= last; ++index) {
        const double relative = static_cast<double>(index) - position_;
        const double distance = std::abs(relative);
        const double scale = std::pow(config_.neighborScale, distance);
        const double size = centerSize * scale;
        const double x = centerX + relative * step - size * 0.5;
        const double y = centerY - size * 0.5 +
            std::min(distance, 1.0) * centerSize * 0.035;
        const double fadeRange =
            static_cast<double>(config_.visibleNeighbors) + 1.0;
        const double opacity = std::lerp(
            1.0,
            config_.minimumOpacity,
            std::clamp(distance / fadeRange, 0.0, 1.0));

        result.push_back({
            static_cast<std::size_t>(index),
            relative,
            {x, y, size, size},
            -sign(relative) * config_.neighborAngleDegrees *
                std::min(distance, 1.0),
            -distance,
            opacity,
            distance < 0.5,
        });
    }

    std::stable_sort(
        result.begin(),
        result.end(),
        [](const CoverflowItemGeometry& left,
           const CoverflowItemGeometry& right) {
            if (left.depth != right.depth) {
                return left.depth < right.depth;
            }
            return left.itemIndex < right.itemIndex;
        });
    return result;
}

CoverflowRect CoverflowModel::containRect(
    CoverflowRect bounds,
    double sourceWidth,
    double sourceHeight) noexcept
{
    if (!finiteRect(bounds) ||
        !std::isfinite(sourceWidth) ||
        !std::isfinite(sourceHeight) ||
        sourceWidth <= 0.0 ||
        sourceHeight <= 0.0) {
        return {};
    }

    const double scale = std::min(
        bounds.width / sourceWidth,
        bounds.height / sourceHeight);
    const double width = sourceWidth * scale;
    const double height = sourceHeight * scale;
    return {
        bounds.x + (bounds.width - width) * 0.5,
        bounds.y + (bounds.height - height) * 0.5,
        width,
        height,
    };
}

double CoverflowModel::clampedPosition(double value) const noexcept
{
    if (!std::isfinite(value) || itemCount_ == 0) {
        return 0.0;
    }
    return std::clamp(value, 0.0, static_cast<double>(itemCount_ - 1));
}

void CoverflowModel::moveTarget(double itemDelta) noexcept
{
    if (!std::isfinite(itemDelta) || itemDelta == 0.0) {
        return;
    }
    target_ = clampedPosition(std::round(target_) + itemDelta);
    beginMotion();
}

void CoverflowModel::beginMotion() noexcept
{
    if (motionMode_ == CoverflowMotionMode::normal) {
        spatialAnimationActive_ = std::abs(position_ - target_) > 0.0005;
        reducedTransitionActive_ = false;
        transitionProgress_ = 1.0;
    } else {
        position_ = target_;
        velocity_ = 0.0;
        spatialAnimationActive_ = false;
        if (motionMode_ == CoverflowMotionMode::reduced) {
            transitionProgress_ = 0.0;
            reducedTransitionActive_ = true;
        } else {
            transitionProgress_ = 1.0;
            reducedTransitionActive_ = false;
        }
    }
}

} // namespace nekorox::component
