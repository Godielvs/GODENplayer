#include "nekorox/recording_identity.h"
#include "nekorox/scroll_controller.h"
#include "physical_file_edit_plan.h"
#include "render_surface_contracts.h"

#include <cmath>
#include <stdexcept>
#include <string>

namespace {

void check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

void checkNear(double actual, double expected, double tolerance,
    const char* message)
{
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(message);
    }
}

void verifyScrollPhysicsGolden()
{
    struct Sample {
        double seconds;
        double position;
    };
    constexpr Sample golden[]{
        {1.0 / 240.0, 503.417500135557},
        {1.0 / 165.0, 506.947967307005},
        {1.0 / 144.0, 508.954799836039},
        {1.0 / 60.0, 542.199834635539},
        {0.05, 699.529002349836},
        {0.10, 848.095472182162},
        {0.25, 918.731951266529},
        {1.00, 920.0},
    };

    for (const auto& expected : golden) {
        nekorox::ScrollController scroll;
        scroll.setBounds(0.0, 5000.0);
        scroll.setPosition(500.0);
        check(scroll.addWheelImpulseAt(420.0, 0.0),
            "BASE02 golden impulse was rejected");
        scroll.advanceTo(expected.seconds);
        checkNear(scroll.position(), expected.position, 1.0e-9,
            "BASE02 approved scroll trajectory changed");
    }

    nekorox::ScrollController reversal;
    reversal.setBounds(0.0, 5000.0);
    reversal.setPosition(500.0);
    reversal.addWheelImpulseAt(420.0, 0.0);
    reversal.advanceTo(0.05);
    check(reversal.addWheelImpulseAt(-120.0, 0.05),
        "BASE02 reversal impulse was rejected");
    reversal.advanceTo(0.05 + 1.0 / 165.0);
    checkNear(reversal.position(), 697.543868833549, 1.0e-9,
        "BASE02 reversal trajectory changed");
    checkNear(reversal.target(), 579.529002349836, 1.0e-9,
        "BASE02 reversal target changed");
}

void verifySharedContracts()
{
    nekorox::RecordingKey recording{"entity-1", 7U, "media-r1"};
    const nekorox::RequestToken first{recording, 11U};
    const nekorox::RequestToken same{recording, 11U};
    const nekorox::RequestToken newer{recording, 12U};
    check(first == same && !(first == newer),
        "R3 request generations are not independent");

    nekorox::component::PhysicalFileEditPlan plan;
    plan.path = L"recording.flac";
    plan.bySubsong[1U].push_back(
        {"TITLE", nekorox::TagMutationMode::set, {"one"}});
    plan.bySubsong[2U].push_back(
        {"TITLE", nekorox::TagMutationMode::set, {"two"}});
    check(plan.bySubsong.size() == 2U &&
            plan.bySubsong.at(1U).front().values.front() == "one" &&
            plan.bySubsong.at(2U).front().values.front() == "two",
        "R3 physical edit plan collapsed subsongs");

    constexpr nekorox::component::PreparedFrame prepared{19U, 4U};
    check(prepared.revision == 19U && prepared.deviceEpoch == 4U &&
            nekorox::component::FrameResult::accepted !=
                nekorox::component::FrameResult::notReady,
        "R3 graphics contracts lost revision/backend semantics");
}

} // namespace

void runR3ProbeTests()
{
    verifyScrollPhysicsGolden();
    verifySharedContracts();
}
