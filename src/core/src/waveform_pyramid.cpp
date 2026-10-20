#include "nekorox/waveform_pyramid.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace nekorox {
namespace {

constexpr std::size_t kWireFixedHeaderBytes = 80;
constexpr std::size_t kWireLevelDescriptorBytes = 16;
constexpr std::size_t kWireHeaderBytes =
    kWireFixedHeaderBytes +
    kWaveformPyramidLevelCount * kWireLevelDescriptorBytes;
constexpr std::size_t kWireBinBytesV1 = sizeof(float) * 2;
constexpr std::size_t kWireBinBytesV2 =
    kWireBinBytesV1 + sizeof(WaveformDetailV2);

static_assert(sizeof(float) == 4);
static_assert(sizeof(double) == 8);
static_assert(std::numeric_limits<float>::is_iec559);
static_assert(std::numeric_limits<double>::is_iec559);

bool fail(std::string* error, const char* message) noexcept
{
    if (error != nullptr) {
        try {
            *error = message;
        } catch (...) {
        }
    }
    return false;
}

bool validBin(const WaveformBinV1& bin) noexcept
{
    return std::isfinite(bin.peak) && std::isfinite(bin.rms) &&
        bin.peak >= 0.0f && bin.rms >= 0.0f &&
        bin.detail.reserved0 == 0 && bin.detail.reserved1 == 0;
}

float decodeU16(std::uint16_t value) noexcept
{
    return static_cast<float>(value) / 65535.0f;
}

float decodeU8(std::uint8_t value) noexcept
{
    return static_cast<float>(value) / 255.0f;
}

std::uint16_t encodeU16(float value) noexcept
{
    return static_cast<std::uint16_t>(std::lround(
        std::clamp(value, 0.0f, 1.0f) * 65535.0f));
}

std::uint8_t encodeU8(float value) noexcept
{
    return static_cast<std::uint8_t>(std::lround(
        std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

std::vector<WaveformBinV1> aggregate(
    std::span<const WaveformBinV1> source)
{
    std::vector<WaveformBinV1> result;
    result.reserve((source.size() + 3U) / 4U);
    for (std::size_t begin = 0; begin < source.size(); begin += 4U) {
        const auto end = std::min(source.size(), begin + 4U);
        double squareSum = 0.0;
        double leftSquareSum = 0.0;
        double rightSquareSum = 0.0;
        double medianSquareSum = 0.0;
        double denseSquareSum = 0.0;
        double lowSquareSum = 0.0;
        double midSquareSum = 0.0;
        double highSquareSum = 0.0;
        float peak = 0.0f;
        float peakLeft = 0.0f;
        float peakRight = 0.0f;
        float transient = 0.0f;
        for (auto index = begin; index < end; ++index) {
            const auto& bin = source[index];
            peak = std::max(peak, bin.peak);
            squareSum += static_cast<double>(bin.rms) * bin.rms;
            peakLeft = std::max(peakLeft, decodeU16(bin.detail.peakLeft));
            peakRight = std::max(peakRight, decodeU16(bin.detail.peakRight));
            const auto rmsLeft = decodeU16(bin.detail.rmsLeft);
            const auto rmsRight = decodeU16(bin.detail.rmsRight);
            const auto median = decodeU8(bin.detail.medianAmplitude);
            const auto dense = decodeU8(bin.detail.denseAmplitude);
            const auto low = decodeU8(bin.detail.lowEnergy);
            const auto mid = decodeU8(bin.detail.midEnergy);
            const auto high = decodeU8(bin.detail.highEnergy);
            leftSquareSum += static_cast<double>(rmsLeft) * rmsLeft;
            rightSquareSum += static_cast<double>(rmsRight) * rmsRight;
            medianSquareSum += static_cast<double>(median) * median;
            denseSquareSum += static_cast<double>(dense) * dense;
            lowSquareSum += static_cast<double>(low) * low;
            midSquareSum += static_cast<double>(mid) * mid;
            highSquareSum += static_cast<double>(high) * high;
            transient = std::max(transient, decodeU8(bin.detail.transient));
        }
        const auto count = static_cast<double>(end - begin);
        WaveformBinV1 bin;
        bin.peak = peak;
        bin.rms = static_cast<float>(std::sqrt(squareSum / count));
        bin.detail.peakLeft = encodeU16(peakLeft);
        bin.detail.peakRight = encodeU16(peakRight);
        bin.detail.rmsLeft = encodeU16(
            static_cast<float>(std::sqrt(leftSquareSum / count)));
        bin.detail.rmsRight = encodeU16(
            static_cast<float>(std::sqrt(rightSquareSum / count)));
        bin.detail.medianAmplitude = encodeU8(
            static_cast<float>(std::sqrt(medianSquareSum / count)));
        bin.detail.denseAmplitude = encodeU8(
            static_cast<float>(std::sqrt(denseSquareSum / count)));
        bin.detail.lowEnergy = encodeU8(
            static_cast<float>(std::sqrt(lowSquareSum / count)));
        bin.detail.midEnergy = encodeU8(
            static_cast<float>(std::sqrt(midSquareSum / count)));
        bin.detail.highEnergy = encodeU8(
            static_cast<float>(std::sqrt(highSquareSum / count)));
        bin.detail.transient = encodeU8(transient);
        result.push_back(bin);
    }
    return result;
}

void appendU32(std::vector<std::byte>& output, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.push_back(
            static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void appendU16(std::vector<std::byte>& output, std::uint16_t value)
{
    output.push_back(static_cast<std::byte>(value & 0xffU));
    output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void appendU64(std::vector<std::byte>& output, std::uint64_t value)
{
    for (unsigned shift = 0; shift < 64; shift += 8) {
        output.push_back(
            static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void appendFloat(std::vector<std::byte>& output, float value)
{
    appendU32(output, std::bit_cast<std::uint32_t>(value));
}

bool readU32(
    std::span<const std::byte> data,
    std::size_t& offset,
    std::uint32_t& value) noexcept
{
    if (offset > data.size() || data.size() - offset < 4U) return false;
    value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(
            std::to_integer<unsigned char>(data[offset++])) << shift;
    }
    return true;
}

bool readU16(
    std::span<const std::byte> data,
    std::size_t& offset,
    std::uint16_t& value) noexcept
{
    if (offset > data.size() || data.size() - offset < 2U) return false;
    value = static_cast<std::uint16_t>(
        std::to_integer<unsigned char>(data[offset])) |
        static_cast<std::uint16_t>(
            std::to_integer<unsigned char>(data[offset + 1U]) << 8U);
    offset += 2U;
    return true;
}

bool readU64(
    std::span<const std::byte> data,
    std::size_t& offset,
    std::uint64_t& value) noexcept
{
    if (offset > data.size() || data.size() - offset < 8U) return false;
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(
            std::to_integer<unsigned char>(data[offset++])) << shift;
    }
    return true;
}

bool readFloat(
    std::span<const std::byte> data,
    std::size_t& offset,
    float& value) noexcept
{
    std::uint32_t bits = 0;
    if (!readU32(data, offset, bits)) return false;
    value = std::bit_cast<float>(bits);
    return true;
}

std::uint32_t crc32(std::span<const std::byte> data) noexcept
{
    std::uint32_t value = 0xffffffffU;
    for (const auto byte : data) {
        value ^= std::to_integer<std::uint8_t>(byte);
        for (unsigned bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(
                -static_cast<std::int32_t>(value & 1U));
            value = (value >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~value;
}

bool checkedPayloadBytes(
    const WaveformPyramidV1& pyramid,
    std::uint64_t& bytes) noexcept
{
    const auto binBytes = pyramid.version == kWaveformPyramidLegacyVersion
        ? kWireBinBytesV1
        : kWireBinBytesV2;
    bytes = 0;
    for (const auto& level : pyramid.levels) {
        if (level.bins.size() >
            std::numeric_limits<std::uint64_t>::max() / binBytes) {
            return false;
        }
        const auto levelBytes =
            static_cast<std::uint64_t>(level.bins.size()) * binBytes;
        if (bytes > std::numeric_limits<std::uint64_t>::max() - levelBytes) {
            return false;
        }
        bytes += levelBytes;
    }
    return true;
}

} // namespace

bool validateWaveformPyramid(
    const WaveformPyramidV1& pyramid,
    std::string* error) noexcept
{
    if (pyramid.version != kWaveformPyramidLegacyVersion &&
        pyramid.version != kWaveformPyramidVersion) {
        return fail(error, "unsupported waveform pyramid version");
    }
    if ((pyramid.flags & ~kWaveformPyramidFlagComplete) != 0) {
        return fail(error, "waveform pyramid has unknown flags");
    }
    if (pyramid.sampleRate == 0 || pyramid.sampleRate > 768000U) {
        return fail(error, "waveform pyramid sample rate is invalid");
    }
    if (pyramid.channels == 0 || pyramid.channels > 64U) {
        return fail(error, "waveform pyramid channel count is invalid");
    }
    if (!std::isfinite(pyramid.durationSeconds) ||
        pyramid.durationSeconds < 0.0) {
        return fail(error, "waveform pyramid duration is invalid");
    }
    if (pyramid.totalFrames != 0 &&
        pyramid.scannedFrames > pyramid.totalFrames) {
        return fail(error, "waveform pyramid scanned beyond total frames");
    }

    auto expectedCount = pyramid.levels[0].bins.size();
    if (expectedCount > kWaveformPyramidMaximumBaseBins) {
        return fail(error, "waveform pyramid exceeds the base-bin limit");
    }
    for (std::size_t index = 0;
         index < kWaveformPyramidLevelCount; ++index) {
        const auto& level = pyramid.levels[index];
        if (level.resolutionMs != kWaveformPyramidResolutionsMs[index]) {
            return fail(error, "waveform pyramid resolution sequence is invalid");
        }
        if (level.bins.size() != expectedCount) {
            return fail(error, "waveform pyramid level size is inconsistent");
        }
        if (!std::all_of(level.bins.begin(), level.bins.end(), validBin)) {
            return fail(error, "waveform pyramid contains an invalid bin");
        }
        expectedCount = (expectedCount + 3U) / 4U;
    }
    return true;
}

bool encodeWaveformPyramid(
    const WaveformPyramidV1& pyramid,
    std::vector<std::byte>& output,
    std::string& error)
{
    error.clear();
    output.clear();
    if (!validateWaveformPyramid(pyramid, &error)) return false;

    std::uint64_t payloadBytes = 0;
    if (!checkedPayloadBytes(pyramid, payloadBytes) ||
        payloadBytes >
            std::numeric_limits<std::size_t>::max() - kWireHeaderBytes) {
        error = "waveform pyramid is too large to encode";
        return false;
    }

    try {
        output.reserve(
            kWireHeaderBytes + static_cast<std::size_t>(payloadBytes));
        appendU32(output, kWaveformPyramidMagic);
        appendU32(output,
            pyramid.version == kWaveformPyramidLegacyVersion
                ? kWaveformPyramidLegacyWireVersion
                : kWaveformPyramidWireVersion);
        appendU32(output, pyramid.version);
        appendU32(output, static_cast<std::uint32_t>(kWireHeaderBytes));
        appendU32(output, pyramid.flags);
        appendU32(output, pyramid.sampleRate);
        appendU32(output, pyramid.channels);
        appendU32(output,
            static_cast<std::uint32_t>(kWaveformPyramidLevelCount));
        appendU64(output, pyramid.generation);
        appendU64(output, pyramid.totalFrames);
        appendU64(output, pyramid.scannedFrames);
        appendU64(output, std::bit_cast<std::uint64_t>(
            pyramid.durationSeconds));
        appendU64(output, payloadBytes);
        appendU32(output, 0); // Payload CRC, patched after encoding.
        appendU32(output, 0);
        for (const auto& level : pyramid.levels) {
            appendU32(output, level.resolutionMs);
            appendU32(output, 0);
            appendU64(output,
                static_cast<std::uint64_t>(level.bins.size()));
        }
        for (const auto& level : pyramid.levels) {
            for (const auto& bin : level.bins) {
                appendFloat(output, bin.peak);
                appendFloat(output, bin.rms);
                if (pyramid.version >= kWaveformPyramidVersion) {
                    appendU16(output, bin.detail.peakLeft);
                    appendU16(output, bin.detail.peakRight);
                    appendU16(output, bin.detail.rmsLeft);
                    appendU16(output, bin.detail.rmsRight);
                    output.push_back(static_cast<std::byte>(
                        bin.detail.medianAmplitude));
                    output.push_back(static_cast<std::byte>(
                        bin.detail.denseAmplitude));
                    output.push_back(static_cast<std::byte>(
                        bin.detail.lowEnergy));
                    output.push_back(static_cast<std::byte>(
                        bin.detail.midEnergy));
                    output.push_back(static_cast<std::byte>(
                        bin.detail.highEnergy));
                    output.push_back(static_cast<std::byte>(
                        bin.detail.transient));
                    output.push_back(static_cast<std::byte>(
                        bin.detail.reserved0));
                    output.push_back(static_cast<std::byte>(
                        bin.detail.reserved1));
                }
            }
        }
    } catch (const std::bad_alloc&) {
        output.clear();
        error = "waveform pyramid allocation failed";
        return false;
    }

    const auto payload = std::span<const std::byte>(output)
        .subspan(kWireHeaderBytes);
    const auto checksum = crc32(payload);
    constexpr std::size_t kChecksumOffset = 72;
    for (unsigned byte = 0; byte < 4; ++byte) {
        output[kChecksumOffset + byte] = static_cast<std::byte>(
            (checksum >> (byte * 8U)) & 0xffU);
    }
    return true;
}

bool decodeWaveformPyramid(
    std::span<const std::byte> encoded,
    WaveformPyramidV1& output,
    std::string& error)
{
    error.clear();
    output = {};
    if (encoded.size() < kWireHeaderBytes) {
        error = "waveform pyramid payload is truncated";
        return false;
    }

    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint32_t wireVersion = 0;
    std::uint32_t headerBytes = 0;
    std::uint32_t levelCount = 0;
    std::uint32_t checksum = 0;
    std::uint32_t reserved = 0;
    std::uint64_t durationBits = 0;
    std::uint64_t payloadBytes = 0;
    if (!readU32(encoded, offset, magic) ||
        !readU32(encoded, offset, wireVersion) ||
        !readU32(encoded, offset, output.version) ||
        !readU32(encoded, offset, headerBytes) ||
        !readU32(encoded, offset, output.flags) ||
        !readU32(encoded, offset, output.sampleRate) ||
        !readU32(encoded, offset, output.channels) ||
        !readU32(encoded, offset, levelCount) ||
        !readU64(encoded, offset, output.generation) ||
        !readU64(encoded, offset, output.totalFrames) ||
        !readU64(encoded, offset, output.scannedFrames) ||
        !readU64(encoded, offset, durationBits) ||
        !readU64(encoded, offset, payloadBytes) ||
        !readU32(encoded, offset, checksum) ||
        !readU32(encoded, offset, reserved)) {
        error = "waveform pyramid header is truncated";
        return false;
    }
    output.durationSeconds = std::bit_cast<double>(durationBits);

    const auto compatibleVersion =
        (wireVersion == kWaveformPyramidLegacyWireVersion &&
            output.version == kWaveformPyramidLegacyVersion) ||
        (wireVersion == kWaveformPyramidWireVersion &&
            output.version == kWaveformPyramidVersion);
    if (magic != kWaveformPyramidMagic ||
        !compatibleVersion ||
        headerBytes != kWireHeaderBytes ||
        levelCount != kWaveformPyramidLevelCount ||
        reserved != 0) {
        error = "waveform pyramid header is incompatible";
        return false;
    }
    if (payloadBytes != encoded.size() - kWireHeaderBytes) {
        error = "waveform pyramid payload length is inconsistent";
        return false;
    }

    std::array<std::uint64_t, kWaveformPyramidLevelCount> binCounts{};
    for (std::size_t index = 0;
         index < kWaveformPyramidLevelCount; ++index) {
        std::uint32_t descriptorReserved = 0;
        if (!readU32(encoded, offset, output.levels[index].resolutionMs) ||
            !readU32(encoded, offset, descriptorReserved) ||
            !readU64(encoded, offset, binCounts[index]) ||
            descriptorReserved != 0) {
            error = "waveform pyramid level descriptor is invalid";
            return false;
        }
        if (binCounts[index] > kWaveformPyramidMaximumBaseBins) {
            error = "waveform pyramid level exceeds the bin limit";
            return false;
        }
    }
    if (offset != kWireHeaderBytes ||
        crc32(encoded.subspan(kWireHeaderBytes)) != checksum) {
        error = "waveform pyramid checksum mismatch";
        return false;
    }

    try {
        for (std::size_t levelIndex = 0;
             levelIndex < kWaveformPyramidLevelCount; ++levelIndex) {
            auto& bins = output.levels[levelIndex].bins;
            bins.resize(static_cast<std::size_t>(binCounts[levelIndex]));
            for (auto& bin : bins) {
                if (!readFloat(encoded, offset, bin.peak) ||
                    !readFloat(encoded, offset, bin.rms)) {
                    error = "waveform pyramid bin payload is truncated";
                    output = {};
                    return false;
                }
                if (output.version >= kWaveformPyramidVersion) {
                    if (!readU16(encoded, offset, bin.detail.peakLeft) ||
                        !readU16(encoded, offset, bin.detail.peakRight) ||
                        !readU16(encoded, offset, bin.detail.rmsLeft) ||
                        !readU16(encoded, offset, bin.detail.rmsRight) ||
                        encoded.size() - offset < 8U) {
                        error = "waveform detail payload is truncated";
                        output = {};
                        return false;
                    }
                    bin.detail.medianAmplitude =
                        std::to_integer<std::uint8_t>(encoded[offset++]);
                    bin.detail.denseAmplitude =
                        std::to_integer<std::uint8_t>(encoded[offset++]);
                    bin.detail.lowEnergy =
                        std::to_integer<std::uint8_t>(encoded[offset++]);
                    bin.detail.midEnergy =
                        std::to_integer<std::uint8_t>(encoded[offset++]);
                    bin.detail.highEnergy =
                        std::to_integer<std::uint8_t>(encoded[offset++]);
                    bin.detail.transient =
                        std::to_integer<std::uint8_t>(encoded[offset++]);
                    bin.detail.reserved0 =
                        std::to_integer<std::uint8_t>(encoded[offset++]);
                    bin.detail.reserved1 =
                        std::to_integer<std::uint8_t>(encoded[offset++]);
                }
            }
        }
    } catch (const std::bad_alloc&) {
        output = {};
        error = "waveform pyramid allocation failed";
        return false;
    }
    if (offset != encoded.size() ||
        !validateWaveformPyramid(output, &error)) {
        output = {};
        if (error.empty()) error = "waveform pyramid has trailing data";
        return false;
    }
    return true;
}

WaveformPyramidBuilder::WaveformPyramidBuilder(
    std::uint64_t generation,
    std::uint64_t totalFrames) noexcept
    : generation_(generation),
      totalFrames_(totalFrames)
{
}

bool WaveformPyramidBuilder::appendInterleaved(
    std::span<const float> samples,
    std::size_t frameCount,
    std::uint32_t channels,
    std::uint32_t sampleRate,
    std::string& error)
{
    error.clear();
    if (finalized_) {
        error = "cannot append to a finalized waveform pyramid";
        return false;
    }
    if (channels == 0 || channels > 64U ||
        sampleRate == 0 || sampleRate > 768000U ||
        frameCount > samples.size() / channels) {
        error = "waveform PCM chunk is invalid";
        return false;
    }
    if (sampleRate_ == 0) {
        sampleRate_ = sampleRate;
        channels_ = channels;
        const auto onePole = [sampleRate](float frequency) noexcept {
            constexpr float kTau = 6.2831853071795864769f;
            return 1.0f - std::exp(
                -kTau * frequency / static_cast<float>(sampleRate));
        };
        lowCoefficient_ = onePole(std::min(
            250.0f, static_cast<float>(sampleRate) * 0.20f));
        highCoefficient_ = onePole(std::min(
            4000.0f, static_cast<float>(sampleRate) * 0.45f));
        fastEnvelopeCoefficient_ = 1.0f - std::exp(
            -1.0f / (0.0025f * static_cast<float>(sampleRate)));
        slowEnvelopeCoefficient_ = 1.0f - std::exp(
            -1.0f / (0.035f * static_cast<float>(sampleRate)));
    } else if (sampleRate_ != sampleRate || channels_ != channels) {
        error = "waveform PCM format changed during decoding";
        return false;
    }
    if (baseBins_.size() >= kWaveformPyramidMaximumBaseBins) {
        error = "waveform exceeds the supported duration";
        return false;
    }

    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        const auto sampleOffset = frame * channels;
        const auto left = samples[sampleOffset];
        const auto inputRight =
            channels > 1 ? samples[sampleOffset + 1U] : left;
        const auto legacyRight = channels > 1 ? inputRight : 0.0f;
        if (!std::isfinite(left) || !std::isfinite(inputRight)) {
            error = "waveform PCM contains a non-finite sample";
            return false;
        }
        partialPeak_ = std::max(
            partialPeak_, std::max(std::abs(left), std::abs(inputRight)));
        partialSquareSum_ +=
            static_cast<double>(left) * static_cast<double>(left) +
            static_cast<double>(legacyRight) * legacyRight;
        partialLeftPeak_ = std::max(partialLeftPeak_, std::abs(left));
        partialRightPeak_ = std::max(partialRightPeak_, std::abs(inputRight));
        partialLeftSquareSum_ += static_cast<double>(left) * left;
        partialRightSquareSum_ +=
            static_cast<double>(inputRight) * inputRight;

        const auto mono = channels > 1
            ? (left + inputRight) * 0.5f
            : left;
        lowPass250_ += lowCoefficient_ * (mono - lowPass250_);
        lowPass4000_ += highCoefficient_ * (mono - lowPass4000_);
        const auto low = lowPass250_;
        const auto mid = lowPass4000_ - lowPass250_;
        const auto high = mono - lowPass4000_;
        partialLowSquareSum_ += static_cast<double>(low) * low;
        partialMidSquareSum_ += static_cast<double>(mid) * mid;
        partialHighSquareSum_ += static_cast<double>(high) * high;

        const auto magnitude = std::min(
            1.0f, std::max(std::abs(left), std::abs(inputRight)));
        const auto bucket = static_cast<std::size_t>(std::clamp(
            static_cast<int>(magnitude * 31.0f), 0, 31));
        ++partialAmplitudeHistogram_[bucket];
        fastEnvelope_ += fastEnvelopeCoefficient_ *
            (magnitude - fastEnvelope_);
        slowEnvelope_ += slowEnvelopeCoefficient_ *
            (magnitude - slowEnvelope_);
        partialTransient_ = std::max(
            partialTransient_,
            std::clamp(
                (fastEnvelope_ - slowEnvelope_) /
                    (slowEnvelope_ + 0.03f),
                0.0f,
                1.0f));
        ++partialFrameCount_;
        ++scannedFrames_;

        const auto nextBoundary =
            (static_cast<std::uint64_t>(baseBins_.size() + 1U) *
                sampleRate_ + 99U) / 100U;
        if (scannedFrames_ >= nextBoundary) {
            finishPartialBin();
            if (baseBins_.size() >= kWaveformPyramidMaximumBaseBins &&
                frame + 1U < frameCount) {
                error = "waveform exceeds the supported duration";
                return false;
            }
        }
    }
    if (totalFrames_ != 0 && scannedFrames_ > totalFrames_) {
        totalFrames_ = scannedFrames_;
    }
    return true;
}

WaveformBinV1 WaveformPyramidBuilder::partialBin() const noexcept
{
    WaveformBinV1 bin;
    if (partialFrameCount_ == 0) return bin;
    const auto count = static_cast<double>(partialFrameCount_);
    bin.peak = partialPeak_;
    bin.rms = static_cast<float>(std::sqrt(partialSquareSum_ / count));
    bin.detail.peakLeft = encodeU16(partialLeftPeak_);
    bin.detail.peakRight = encodeU16(partialRightPeak_);
    bin.detail.rmsLeft = encodeU16(static_cast<float>(
        std::sqrt(partialLeftSquareSum_ / count)));
    bin.detail.rmsRight = encodeU16(static_cast<float>(
        std::sqrt(partialRightSquareSum_ / count)));
    const auto percentile = [&](std::uint64_t rank) noexcept {
        std::uint64_t cumulative = 0;
        for (std::size_t index = 0;
             index < partialAmplitudeHistogram_.size(); ++index) {
            cumulative += partialAmplitudeHistogram_[index];
            if (cumulative >= rank) {
                return static_cast<float>(index) /
                    static_cast<float>(
                        partialAmplitudeHistogram_.size() - 1U);
            }
        }
        return 0.0f;
    };
    bin.detail.medianAmplitude = encodeU8(percentile(
        (partialFrameCount_ + 1U) / 2U));
    bin.detail.denseAmplitude = encodeU8(percentile(
        (partialFrameCount_ * 9U + 9U) / 10U));
    bin.detail.lowEnergy = encodeU8(static_cast<float>(
        std::sqrt(partialLowSquareSum_ / count)));
    bin.detail.midEnergy = encodeU8(static_cast<float>(
        std::sqrt(partialMidSquareSum_ / count)));
    bin.detail.highEnergy = encodeU8(static_cast<float>(
        std::sqrt(partialHighSquareSum_ / count)));
    bin.detail.transient = encodeU8(partialTransient_);
    return bin;
}

void WaveformPyramidBuilder::finishPartialBin()
{
    if (partialFrameCount_ == 0) return;
    baseBins_.push_back(partialBin());
    partialSquareSum_ = 0.0;
    partialLeftSquareSum_ = 0.0;
    partialRightSquareSum_ = 0.0;
    partialLowSquareSum_ = 0.0;
    partialMidSquareSum_ = 0.0;
    partialHighSquareSum_ = 0.0;
    partialPeak_ = 0.0f;
    partialLeftPeak_ = 0.0f;
    partialRightPeak_ = 0.0f;
    partialTransient_ = 0.0f;
    partialAmplitudeHistogram_.fill(0);
    partialFrameCount_ = 0;
}

WaveformPyramidV1 WaveformPyramidBuilder::snapshot(bool complete) const
{
    WaveformPyramidV1 result;
    result.flags = complete ? kWaveformPyramidFlagComplete : 0;
    result.generation = generation_;
    result.sampleRate = sampleRate_;
    result.channels = channels_;
    result.totalFrames = std::max(totalFrames_, scannedFrames_);
    result.scannedFrames = scannedFrames_;
    const auto durationFrames =
        totalFrames_ != 0 ? std::max(totalFrames_, scannedFrames_)
                          : scannedFrames_;
    result.durationSeconds = sampleRate_ == 0
        ? 0.0
        : static_cast<double>(durationFrames) /
            static_cast<double>(sampleRate_);
    result.levels[0].resolutionMs = kWaveformPyramidResolutionsMs[0];
    result.levels[0].bins = baseBins_;
    if (partialFrameCount_ != 0) {
        result.levels[0].bins.push_back(partialBin());
    }
    for (std::size_t index = 1;
         index < kWaveformPyramidLevelCount; ++index) {
        result.levels[index].resolutionMs =
            kWaveformPyramidResolutionsMs[index];
        result.levels[index].bins =
            aggregate(result.levels[index - 1].bins);
    }
    return result;
}

WaveformPyramidV1 WaveformPyramidBuilder::finalize()
{
    if (!finalized_) {
        finishPartialBin();
        finalized_ = true;
    }
    return snapshot(true);
}

} // namespace nekorox
