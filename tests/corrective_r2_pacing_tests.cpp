#include "../src/component/presentation_revision.h"

#include <windows.h>
#include <dxgi.h>

#include <stdexcept>

void runCorrectiveR2PacingTests()
{
    using nekorox::component::PresentationRevision;

    // Three refusals, including the final settled state, must preserve demand.
    PresentationRevision refused;
    auto frame = refused.capture();
    refused.presented(frame, DXGI_ERROR_WAS_STILL_DRAWING, false);
    refused.presented(frame, DXGI_ERROR_WAS_STILL_DRAWING, false);
    const auto finalRevision = refused.invalidate();
    frame = refused.capture();
    refused.presented(frame, DXGI_ERROR_WAS_STILL_DRAWING, false);
    if (!refused.pending() || refused.accepted() >= finalRevision) {
        throw std::runtime_error(
            "C01 final refused frame consumed semantic demand");
    }
    refused.presented(frame, S_OK, true);
    if (refused.pending() || refused.accepted() != finalRevision) {
        throw std::runtime_error(
            "C01 retry did not converge without new input");
    }

    // Present acceptance and externally observed display are distinct facts.
    PresentationRevision observed;
    const auto accepted = observed.capture();
    observed.presented(accepted, S_OK, false);
    if (observed.accepted() != accepted.revision || observed.displayed() != 0) {
        throw std::runtime_error(
            "C01 S_OK was incorrectly classified as displayed");
    }
    observed.observedDisplayed(accepted);
    if (observed.displayed() != accepted.revision) {
        throw std::runtime_error(
            "C01 external display observation was not correlated");
    }

    // A hidden success is accepted, while display remains independently
    // unobserved. Occlusion itself is reported as a non-success result.
    PresentationRevision hidden;
    const auto hiddenFrame = hidden.capture();
    hidden.presented(hiddenFrame, S_OK, false);
    if (hidden.pending() || hidden.displayed() != 0) {
        throw std::runtime_error(
            "C01 hidden S_OK did not preserve acceptance/display separation");
    }
    const auto visibleRevision = hidden.invalidate();
    hidden.presented(hidden.capture(), DXGI_STATUS_OCCLUDED, false);
    if (!hidden.pending() || hidden.accepted() >= visibleRevision) {
        throw std::runtime_error(
            "C01 occlusion consumed a pending revision");
    }

    // A callback/frame token from the previous swap-chain epoch is inert.
    PresentationRevision epochs;
    const auto stale = epochs.capture();
    epochs.deviceEpochChanged();
    epochs.presented(stale, S_OK, true);
    epochs.observedDisplayed(stale);
    if (!epochs.pending() || epochs.accepted() != 0 ||
        epochs.displayed() != 0) {
        throw std::runtime_error(
            "C01 stale epoch mutated the replacement presentation target");
    }
    const auto current = epochs.capture();
    epochs.presented(current, S_OK, true);
    if (epochs.pending()) {
        throw std::runtime_error(
            "C01 current epoch failed to converge");
    }
}
