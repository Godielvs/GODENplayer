#include "presentation_revision.h"

#include <windows.h>

namespace nekorox::component {

void PresentationRevision::presented(
    std::uint64_t drawn,
    long result,
    bool displayObserved) noexcept
{
    if (result == S_OK && drawn <= desired_) {
        if (drawn > accepted_) accepted_ = drawn;
        if (displayObserved && drawn > displayed_) displayed_ = drawn;
    }
}

void PresentationRevision::presented(
    PresentationFrameToken frame,
    long result,
    bool displayObserved) noexcept
{
    if (frame.epoch != epoch_) return;
    presented(frame.revision, result, displayObserved);
}

void PresentationRevision::observedDisplayed(
    PresentationFrameToken frame) noexcept
{
    if (frame.epoch != epoch_ || frame.revision > accepted_) return;
    if (frame.revision > displayed_) displayed_ = frame.revision;
}

void PresentationRevision::deviceEpochChanged() noexcept
{
    ++epoch_;
    ++desired_;
    displayed_ = 0;
}

} // namespace nekorox::component
