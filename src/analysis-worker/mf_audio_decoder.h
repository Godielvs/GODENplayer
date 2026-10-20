#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace nekorox::analysis {

struct DecodedAudio {
    std::vector<float> samples;
    std::uint32_t sampleRate = 0;
    std::uint32_t sourceChannels = 0;
    double durationSeconds = 0.0;
    bool hasStereoMetrics = false;
    double stereoCorrelation = 0.0;
    double midEnergy = 0.0;
    double sideEnergy = 0.0;
    double outOfPhaseFraction = 0.0;
};

using DecodeProgress = std::function<void(std::uint64_t completed,
    std::uint64_t total)>;

[[nodiscard]] bool decodeLocalAudio(const std::filesystem::path& path,
    DecodedAudio& output, const DecodeProgress& progress, std::string& error);

} // namespace nekorox::analysis
