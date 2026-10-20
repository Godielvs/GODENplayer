#pragma once

#include "master_dsp_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <chrono>

namespace nekorox::goden_audio {

enum class MasterDspDomain : std::uint8_t {
    player = 0,
    daw = 1,
};

// Independent stateful endpoint for one final-output domain. The immutable
// rack definition is shared through MasterDspController; filter, envelope,
// delay and reverb state never crosses between player and DAW.
class MasterDspProcessor final {
public:
    explicit MasterDspProcessor(MasterDspDomain domain);
    ~MasterDspProcessor();
    MasterDspProcessor(const MasterDspProcessor&) = delete;
    MasterDspProcessor& operator=(const MasterDspProcessor&) = delete;

    // Interleaved float32. Returns true when output was processed or latency-
    // aligned; false means exact untouched passthrough.
    bool process(
        float* interleaved,
        std::size_t frames,
        std::uint32_t channels,
        std::uint32_t sampleRate) noexcept;

    [[nodiscard]] MasterDspProcessResultV2 processV2(
        float* interleaved,
        std::size_t frames,
        std::uint32_t channels,
        std::uint32_t sampleRate) noexcept;

    // Control-thread preparation. Calling this before the first audio buffer
    // prevents a configured rack from emitting a dry cold-start window.
    [[nodiscard]] MasterDspPrepareTicket prewarm(
        std::uint32_t sampleRate,
        std::uint32_t channels) noexcept;
    [[nodiscard]] MasterDspPrepareTicket prepareTicket() const noexcept;
    [[nodiscard]] bool waitUntilPrepared(
        std::chrono::milliseconds timeout) noexcept;

    void reset() noexcept;
    void flush() noexcept;
    [[nodiscard]] std::uint32_t latencySamples() const noexcept;
    [[nodiscard]] std::uint32_t tailSamples() const noexcept;
    [[nodiscard]] MasterDspTelemetryV1 telemetry() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Control-thread wake used after rack/power revisions. Disabled processors can
// therefore sleep indefinitely instead of polling while audio is dry.
void notifyMasterDspCompilerWorkers() noexcept;
void prewarmMasterDspPlayer(
    std::uint32_t sampleRate, std::uint32_t channels) noexcept;
bool publishMasterDspParameterCommand(
    const std::string& moduleUuid,
    const std::string& parameterKey,
    float value) noexcept;
bool publishMasterDspBypassCommand(
    const std::string& moduleUuid,
    bool bypassed) noexcept;

} // namespace nekorox::goden_audio
