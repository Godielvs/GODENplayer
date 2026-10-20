#pragma once

#include <algorithm>
#include <cmath>

namespace nekorox::goden_audio {

/// Undithered signed PCM-style quantization used by the Master DSP utility.
/// Two bits deliberately expose four codes: -1, -0.5, 0 and +0.5. A 32-bit
/// setting is a true passthrough so enabling a neutral quantizer stays exact.
inline float quantizeUnditheredSample(float sample, int bits) noexcept
{
    bits = std::clamp(bits, 2, 32);
    if (bits >= 32) return sample;
    const auto scale = std::ldexp(1.0f, bits - 1);
    const auto maximum = 1.0f - 1.0f / scale;
    const auto finite = std::isfinite(sample) ? sample : 0.0f;
    const auto quantized = std::clamp(
        std::round(std::clamp(finite, -1.0f, maximum) * scale) / scale,
        -1.0f, maximum);
    return quantized == 0.0f ? 0.0f : quantized;
}

} // namespace nekorox::goden_audio
