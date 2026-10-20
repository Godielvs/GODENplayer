#include "../src/component/scrub_preview_service.h"

#include <cmath>
#include <stdexcept>

void runCorrectiveR2AuditionTests()
{
    using namespace nekorox::component;

    for (std::size_t frame = 0; frame < 1200; ++frame) {
        const auto gains = scrubPreviewCrossfadeGains(frame, 1200);
        if (std::abs((gains.outgoing + gains.incoming) - 1.0f) > 0.00001f ||
            gains.outgoing < 0.0f || gains.incoming < 0.0f)
            throw std::runtime_error("C08 overlap gains are not complementary");
    }

    std::uint64_t outputOpens = 0;
    std::uint64_t decoderOpens = 0;
    constexpr std::uint64_t session = 41;
    std::uint64_t openSession = 0;
    for (std::uint64_t targetRevision = 1; targetRevision <= 10; ++targetRevision) {
        const auto needsOpen = scrubPreviewNeedsSessionOpen(openSession, session,
            openSession == session, outputOpens != 0, decoderOpens != 0);
        if (needsOpen) {
            openSession = session;
            ++outputOpens;
            ++decoderOpens;
        }
    }
    if (outputOpens != 1 || decoderOpens != 1)
        throw std::runtime_error("C08 target revisions reopened the audition session");
    if (!scrubPreviewNeedsSessionOpen(openSession, session + 1, true, true, true))
        throw std::runtime_error("C08 a new audition session reused stale devices");

    const auto rate = 48000U;
    const auto frames = static_cast<std::size_t>(rate * 0.180);
    if (scrubPreviewEnvelope(0, frames, rate) != 0.0f ||
        scrubPreviewEnvelope(frames - 1, frames, rate) != 0.0f ||
        scrubPreviewEnvelope(frames / 2, frames, rate) > 0.501188f)
        throw std::runtime_error("C08 180ms/-6dB/8ms preview envelope regressed");
}
