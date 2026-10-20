#pragma once

#include <cstddef>
#include <vector>

namespace nekorox::component {

struct CoverflowRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

enum class CoverflowMotionMode {
    normal,
    reduced,
    off,
};

enum class CoverflowNavigationKey {
    previous,
    next,
    first,
    last,
    pagePrevious,
    pageNext,
};

struct CoverflowConfig {
    std::size_t visibleNeighbors = 3;
    std::size_t pageStep = 5;
    double centerWidthFraction = 0.38;
    double centerHeightFraction = 0.86;
    double neighborStepFraction = 0.17;
    double neighborScale = 0.78;
    double neighborAngleDegrees = 54.0;
    double minimumOpacity = 0.24;
    double dragPixelsPerItem = 96.0;
    double normalSettleMilliseconds = 180.0;
    double reducedTransitionMilliseconds = 70.0;
};

struct CoverflowItemGeometry {
    std::size_t itemIndex = 0;
    double relativePosition = 0.0;
    CoverflowRect frame{};
    double angleDegrees = 0.0;
    double depth = 0.0;
    double opacity = 1.0;
    bool center = false;
};

class CoverflowModel final {
public:
    explicit CoverflowModel(CoverflowConfig config = {});

    void setItemCount(std::size_t itemCount) noexcept;
    void setSelectedIndex(std::size_t index, bool animate = false) noexcept;
    void setMotionMode(CoverflowMotionMode mode) noexcept;

    [[nodiscard]] std::size_t itemCount() const noexcept { return itemCount_; }
    [[nodiscard]] std::size_t selectedIndex() const noexcept;
    [[nodiscard]] double visualPosition() const noexcept { return position_; }
    [[nodiscard]] double targetPosition() const noexcept { return target_; }
    [[nodiscard]] CoverflowMotionMode motionMode() const noexcept
    {
        return motionMode_;
    }
    [[nodiscard]] bool active() const noexcept
    {
        return dragging_ || spatialAnimationActive_ || reducedTransitionActive_;
    }
    [[nodiscard]] bool spatialMotionEnabled() const noexcept
    {
        return motionMode_ == CoverflowMotionMode::normal;
    }
    [[nodiscard]] double transitionProgress() const noexcept
    {
        return transitionProgress_;
    }

    bool navigate(CoverflowNavigationKey key) noexcept;
    bool addWheelDelta(double wheelDelta) noexcept;

    void beginDrag(double pointerX) noexcept;
    bool updateDrag(double pointerX) noexcept;
    bool endDrag(double velocityPixelsPerSecond = 0.0) noexcept;
    void cancelDrag() noexcept;

    // Returns true while another frame is required.
    bool update(double deltaSeconds) noexcept;

    // Returned back-to-front so renderers can paint in vector order.
    [[nodiscard]] std::vector<CoverflowItemGeometry> layout(
        CoverflowRect viewport) const;

    [[nodiscard]] static CoverflowRect containRect(
        CoverflowRect bounds,
        double sourceWidth,
        double sourceHeight) noexcept;

private:
    double clampedPosition(double value) const noexcept;
    void moveTarget(double itemDelta) noexcept;
    void beginMotion() noexcept;

    CoverflowConfig config_;
    std::size_t itemCount_ = 0;
    double position_ = 0.0;
    double target_ = 0.0;
    double velocity_ = 0.0;
    double wheelRemainder_ = 0.0;
    double dragStartX_ = 0.0;
    double dragStartPosition_ = 0.0;
    double transitionProgress_ = 1.0;
    bool dragging_ = false;
    bool spatialAnimationActive_ = false;
    bool reducedTransitionActive_ = false;
    CoverflowMotionMode motionMode_ = CoverflowMotionMode::normal;
};

} // namespace nekorox::component
