#include "scrub_preview_service.h"

#include <cmath>
#include <stdexcept>

namespace {

void check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

} // namespace

void runR3AuditionTests()
{
    using namespace nekorox::component;
    for (const auto rate : {44100U, 48000U, 96000U, 192000U}) {
        for (const auto channels : {1U, 2U, 8U, 32U}) {
            const auto limits = scrubDeliveryLimits(rate, channels);
            const auto blockMs = 1000.0 * limits.blockFrames / rate;
            const auto queueMs = 1000.0 * limits.maximumQueuedFrames / rate;
            check(blockMs >= 5.0 && blockMs <= 10.0,
                "AUD02 output block escaped the 5-10 ms bound");
            check(queueMs <= 100.001,
                "AUD02 output queue escaped the 100 ms bound");
            check(limits.maximumQueuedFrames * channels <=
                    limits.maximumPcmSamples &&
                    limits.maximumPcmSamples * sizeof(float) <= (8U << 20U),
                "AUD02 PCM plan escaped the 8 MiB bound");
        }
    }
    check(scrubDeliveryLimits(0, 2).blockFrames == 0 &&
            scrubDeliveryLimits(48000, 33).blockFrames == 0,
        "AUD02 invalid output formats produced a delivery plan");

    constexpr std::size_t overlap = 1200;
    for (std::size_t frame = 0; frame < overlap; ++frame) {
        const auto gains = scrubPreviewCrossfadeGains(frame, overlap);
        const auto expectedIncoming = static_cast<float>(frame + 1U) /
            static_cast<float>(overlap + 1U);
        check(std::abs(gains.incoming - expectedIncoming) < 1.0e-7f &&
                std::abs(gains.outgoing + gains.incoming - 1.0f) < 1.0e-7f,
            "AUD02 default overlap is not linear and complementary");
    }
}
