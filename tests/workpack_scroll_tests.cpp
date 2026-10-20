#include "nekorox/scroll_controller.h"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
void check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
}

void runWorkpackScrollTests()
{
    using nekorox::ScrollController;
    ScrollController one;
    ScrollController fractions;
    for (auto* controller : {&one, &fractions}) {
        controller->setBounds(0.0, 5000.0);
        controller->setPosition(500.0);
    }
    one.addWheelImpulseAt(84.0, 10.0);
    for (int index = 0; index < 4; ++index) {
        fractions.addWheelImpulseAt(21.0, 10.0);
    }
    check(one.target() == fractions.target() &&
        one.position() == fractions.position(),
        "WP03 same-timestamp fractional deltas diverged");
    check(one.position() == 500.0,
        "WP03 input moved x before the first frame");

    ScrollController hz60;
    ScrollController hz240;
    for (auto* controller : {&hz60, &hz240}) {
        controller->setBounds(0.0, 5000.0);
        controller->setPosition(500.0);
        controller->addWheelImpulseAt(420.0, 0.0);
    }
    for (int frame = 1; frame <= 60; ++frame) {
        const auto common = static_cast<double>(frame) / 60.0;
        hz60.advanceTo(common);
        for (int sub = 1; sub <= 4; ++sub) {
            hz240.advanceTo((static_cast<double>((frame - 1) * 4 + sub)) / 240.0);
        }
        check(std::abs(hz60.position() - hz240.position()) <= 0.25,
            "WP03 refresh-rate trajectories differ at the same instant");
    }

    const auto revision = hz60.visualRevision();
    hz60.addWheelImpulseAt(-120.0, 1.0);
    const auto beforeReverseFrame = hz60.position();
    hz60.advanceTo(1.0 + 1.0 / 165.0);
    check(hz60.position() < beforeReverseFrame &&
        hz60.visualRevision() > revision,
        "WP03 reversal missed the next useful frame/revision");

    ScrollController bounded;
    bounded.setBounds(0.0, 1000.0);
    bounded.setPosition(900.0);
    bounded.addWheelImpulseAt(500.0, 0.0);
    bounded.advanceTo(0.2); // a real stall, never subdivided or clamped
    check(std::isfinite(bounded.position()) && bounded.position() <= 1000.0,
        "WP03 stalled frame escaped finite bounds");
    bounded.setBounds(0.0, 300.0);
    check(bounded.position() == 300.0 && bounded.target() == 300.0 &&
        bounded.velocity() <= 0.0,
        "WP03 bounds shrink retained outward state");
    check(!bounded.addWheelImpulseAt(20.0, -1.0),
        "WP03 accepted a regressive timestamp");
    check(!bounded.addWheelImpulseAt(
        std::numeric_limits<double>::quiet_NaN(), 1.0),
        "WP03 accepted an invalid delta");

    ScrollController off;
    off.setBounds(0.0, 1000.0);
    off.setPosition(200.0);
    off.setResponseScale(0.0);
    off.addWheelImpulseAt(84.0, 1.0);
    check(!off.active() && off.position() == 284.0,
        "WP03 motion-off did not snap to q");

    ScrollController idle;
    idle.setBounds(0.0, 1000.0);
    idle.setPosition(100.0);
    idle.addWheelImpulseAt(84.0, 1.0);
    idle.advanceTo(3.0);
    check(!idle.active() && idle.position() == idle.target(),
        "WP03 long silence did not analytically settle");
}
