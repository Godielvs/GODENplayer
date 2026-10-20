#include "nekorox/realtime_visual_model.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
void requireScope(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

nekorox::RealtimeFrameV1 scopeFrame(std::uint64_t sequence,
    float left, float right, bool impulse = false)
{
    nekorox::RealtimeFrameV1 frame;
    frame.version = nekorox::kRealtimeRingVersion;
    frame.sequence = sequence;
    frame.streamGeneration = 1;
    frame.sampleRate = 48000;
    frame.channels = 2;
    frame.frameCount = 4096;
    frame.sampleCount = frame.frameCount * frame.channels;
    for (std::uint32_t index = 0; index < frame.frameCount; ++index) {
        const auto amount = impulse && index == 3500 ? 1.0f :
            std::sin(static_cast<float>(index) * 0.071f);
        frame.samples[index * 2U] = left * amount;
        frame.samples[index * 2U + 1U] = right * amount;
    }
    return frame;
}
}

void runWorkpackScopeTests()
{
    nekorox::LargeScopeModel model;
    requireScope(model.ingest(scopeFrame(1, 0.0f, 0.0f, true)),
        "WP18 large scope rejected valid PCM");
    model.ingest(scopeFrame(2, 1.0f, 1.0f, true));
    const auto wide = model.render(1200);
    requireScope(wide.columns == 1200 && wide.sourceSequence == 2 &&
            wide.windowMilliseconds == 30,
        "WP18 large scope did not render the requested physical columns");
    requireScope(*std::max_element(wide.maximum.begin(),
            wide.maximum.begin() + wide.columns) > 0.95f,
        "WP18 transient disappeared in min/max downsampling");

    model.configure(30, nekorox::LargeScopeChannelMode::side);
    const auto sideEqual = model.render(1024);
    requireScope(std::all_of(sideEqual.minimum.begin(),
            sideEqual.minimum.begin() + sideEqual.columns,
            [](float value) { return std::abs(value) < 1e-6f; }) &&
        std::all_of(sideEqual.maximum.begin(),
            sideEqual.maximum.begin() + sideEqual.columns,
            [](float value) { return std::abs(value) < 1e-6f; }),
        "WP18 side mode did not cancel equal stereo");

    model.reset();
    model.configure(120, nekorox::LargeScopeChannelMode::mid);
    model.ingest(scopeFrame(10, 1.0f, -1.0f));
    const auto antiMid = model.render(2048);
    requireScope(antiMid.columns == 2048 &&
        std::all_of(antiMid.maximum.begin(),
            antiMid.maximum.begin() + antiMid.columns,
            [](float value) { return std::abs(value) < 1e-6f; }),
        "WP18 mid mode did not cancel anti-phase stereo");
    model.ingest(scopeFrame(12, 1.0f, 1.0f));
    requireScope(model.historyCount() == 4096,
        "WP18 sequence gap did not reset stale PCM history");
}
