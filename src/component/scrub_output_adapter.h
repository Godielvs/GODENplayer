#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace nekorox::component {

struct ScrubOutputRoute {
    bool available = false;
    std::array<std::uint8_t, 16> outputId{};
    std::array<std::uint8_t, 16> deviceId{};
    std::uint32_t bitDepth = 0;
    bool dither = false;
    float userVolumeDb = 0.0f;
    std::string reason;
};

struct ScrubPcmBlock {
    std::span<const float> samples;
    std::size_t frames = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint32_t channelMask = 0;
};

class ScrubOutputStream {
public:
    virtual ~ScrubOutputStream() = default;
    virtual bool submit(const ScrubPcmBlock& block,
        std::string& error) noexcept = 0;
    virtual void flush() noexcept = 0;
    virtual void forcePlay() noexcept = 0;
    virtual void stop() noexcept = 0;
};

[[nodiscard]] ScrubOutputRoute probeScrubOutputRoute() noexcept;
[[nodiscard]] std::unique_ptr<ScrubOutputStream> openScrubOutput(
    const ScrubOutputRoute& route, std::string& error) noexcept;

} // namespace nekorox::component
