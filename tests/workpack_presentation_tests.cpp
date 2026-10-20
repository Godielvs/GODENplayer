#include "../src/component/presentation_revision.h"
#include "../src/component/render_surface_contracts.h"

#include <windows.h>
#include <dxgi.h>

#include <stdexcept>

void runWorkpackPresentationTests()
{
    using nekorox::component::PresentationRevision;
    PresentationRevision state;
    const auto first = state.drawnRevision();
    state.presented(first, DXGI_ERROR_WAS_STILL_DRAWING, false);
    state.presented(first, DXGI_ERROR_WAS_STILL_DRAWING, false);
    if (!state.pending() || state.accepted() != 0U) {
        throw std::runtime_error("WP04 refusal consumed the pending revision");
    }
    const auto finalRevision = state.invalidate();
    state.presented(finalRevision, DXGI_ERROR_WAS_STILL_DRAWING, false);
    if (!state.pending()) {
        throw std::runtime_error("WP04 final refusal lost retry demand");
    }
    state.presented(finalRevision, S_OK, true);
    if (state.pending() || state.accepted() != finalRevision) {
        throw std::runtime_error("WP04 valid display did not converge");
    }
    const auto epoch = state.epoch();
    const auto staleFrame = state.capture();
    state.deviceEpochChanged();
    state.presented(staleFrame, S_OK, true);
    if (state.epoch() == epoch || !state.pending()) {
        throw std::runtime_error("WP04 stale frame cleared recovery revision");
    }
    state.presented(state.drawnRevision(), S_OK, false);
    if (state.pending() || state.displayed() != 0) {
        throw std::runtime_error(
            "WP04 accepted/displayed watermarks were conflated");
    }

    using nekorox::component::FrameOutcome;
    using nekorox::component::FrameResult;
    const FrameOutcome refused{
        FrameResult::notReady,
        S_OK,
        DXGI_ERROR_WAS_STILL_DRAWING,
        7,
        3,
        true,
    };
    if (refused.accepted() || !refused.backingValid ||
        refused.resourceEpoch != 7 || refused.sizeRevision != 3) {
        throw std::runtime_error(
            "R5 refused frame lost explicit surface outcome/epoch");
    }
    const FrameOutcome accepted{
        FrameResult::accepted, S_OK, S_OK, 7, 3, true};
    if (!accepted.accepted()) {
        throw std::runtime_error("R5 accepted frame was not observable");
    }
}
