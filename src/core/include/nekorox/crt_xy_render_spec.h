#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace nekorox {

enum class CrtXyOrientation : std::uint8_t {
    midSide,
    directLeftRight,
};

inline constexpr float kCrtFastTauSeconds = 0.024f;
inline constexpr float kCrtTailTauSeconds = 0.095f;
inline constexpr float kCrtSettledEnergy = 1.0f / 255.0f;
inline constexpr double kCrtMinimumSettleSeconds = 0.5;
inline constexpr float kCrtNominalFrameSeconds = 1.0f / 165.0f;
inline constexpr float kCrtBeamIntensity = 14000.0f;
inline constexpr float kCrtExposure = 22.0f;
inline constexpr std::uint32_t kCrtMaximumRenderWidth = 2048;
inline constexpr std::uint32_t kCrtMaximumRenderHeight = 1536;

struct alignas(16) CrtXyShaderConstants {
    float renderSize[2]{};
    float outputSize[2]{};
    float inverseRenderSize[2]{};
    float exposure = 1.0f;
    float radiusScale = 0.46f;
    float decayFast = 1.0f;
    float decayTail = 1.0f;
    float coreSigma = 1.3f;
    float haloSigma = 4.8f;
    float coreWeight = 0.96f;
    float haloWeight = 0.04f;
    float beamIntensity = 1.0f;
    float gridOpacity = 0.08f;
    float phosphorColor[3]{0.25f, 1.0f, 0.68f};
    float inverseFastTau = 1.0f;
    float inverseTailTau = 1.0f;
    float midSideOrientation = 0.0f;
    float reserved[2]{};
};

static_assert(sizeof(CrtXyShaderConstants) % 16 == 0);

[[nodiscard]] inline CrtXyShaderConstants makeCrtXyShaderConstants(
    std::uint32_t renderWidth,
    std::uint32_t renderHeight,
    std::uint32_t outputWidth,
    std::uint32_t outputHeight,
    double elapsedSeconds,
    CrtXyOrientation orientation) noexcept
{
    CrtXyShaderConstants constants;
    constants.renderSize[0] = static_cast<float>(renderWidth);
    constants.renderSize[1] = static_cast<float>(renderHeight);
    constants.outputSize[0] = static_cast<float>(outputWidth);
    constants.outputSize[1] = static_cast<float>(outputHeight);
    constants.inverseRenderSize[0] = 1.0f / constants.renderSize[0];
    constants.inverseRenderSize[1] = 1.0f / constants.renderSize[1];
    constants.exposure = kCrtExposure;
    constexpr float directRadiusScale = 0.445f;
    constexpr float inverseSqrtTwo = 0.7071067811865475f;
    constants.radiusScale = orientation == CrtXyOrientation::midSide
        ? directRadiusScale * inverseSqrtTwo
        : directRadiusScale;
    constants.decayFast = std::exp(
        -static_cast<float>(elapsedSeconds) / kCrtFastTauSeconds);
    constants.decayTail = std::exp(
        -static_cast<float>(elapsedSeconds) / kCrtTailTauSeconds);
    const auto supersample = constants.renderSize[0] /
        std::max(1.0f, constants.outputSize[0]);
    constants.coreSigma = 0.65f * supersample;
    constants.haloSigma = 2.4f * supersample;
    constants.coreWeight = 0.96f;
    constants.haloWeight = 0.04f;
    constants.beamIntensity = kCrtBeamIntensity;
    constants.gridOpacity = 0.11f;
    constants.phosphorColor[0] = 0.20f;
    constants.phosphorColor[1] = 0.98f;
    constants.phosphorColor[2] = 0.66f;
    constants.inverseFastTau = 1.0f / kCrtFastTauSeconds;
    constants.inverseTailTau = 1.0f / kCrtTailTauSeconds;
    constants.midSideOrientation =
        orientation == CrtXyOrientation::midSide ? 1.0f : 0.0f;
    return constants;
}

[[nodiscard]] inline std::uint32_t crtXyRenderWidth(
    std::uint32_t outputWidth,
    std::uint32_t outputHeight,
    bool softwareRasterizer) noexcept
{
    if (outputWidth == 0 || outputHeight == 0) return 0;
    const auto scale = std::min({
        softwareRasterizer ? 1.0f : 2.0f,
        static_cast<float>(kCrtMaximumRenderWidth) /
            static_cast<float>(outputWidth),
        static_cast<float>(kCrtMaximumRenderHeight) /
            static_cast<float>(outputHeight)});
    return std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(std::lround(outputWidth * scale)));
}

[[nodiscard]] inline std::uint32_t crtXyRenderHeight(
    std::uint32_t outputWidth,
    std::uint32_t outputHeight,
    bool softwareRasterizer) noexcept
{
    if (outputWidth == 0 || outputHeight == 0) return 0;
    const auto scale = std::min({
        softwareRasterizer ? 1.0f : 2.0f,
        static_cast<float>(kCrtMaximumRenderWidth) /
            static_cast<float>(outputWidth),
        static_cast<float>(kCrtMaximumRenderHeight) /
            static_cast<float>(outputHeight)});
    return std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(std::lround(outputHeight * scale)));
}

} // namespace nekorox
