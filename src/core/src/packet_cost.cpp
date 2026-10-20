#include "nekorox/packet_cost.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace nekorox {
namespace {

std::uint16_t readBig16(std::span<const std::uint8_t> bytes, std::size_t at)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[at]) << 8U) |
        static_cast<std::uint16_t>(bytes[at + 1]));
}

std::uint32_t readBig24(std::span<const std::uint8_t> bytes, std::size_t at)
{
    return (static_cast<std::uint32_t>(bytes[at]) << 16U) |
        (static_cast<std::uint32_t>(bytes[at + 1]) << 8U) |
        static_cast<std::uint32_t>(bytes[at + 2]);
}

std::uint32_t readBig32(std::span<const std::uint8_t> bytes, std::size_t at)
{
    return (static_cast<std::uint32_t>(bytes[at]) << 24U) |
        (static_cast<std::uint32_t>(bytes[at + 1]) << 16U) |
        (static_cast<std::uint32_t>(bytes[at + 2]) << 8U) |
        static_cast<std::uint32_t>(bytes[at + 3]);
}

std::uint64_t readBig64(std::span<const std::uint8_t> bytes, std::size_t at)
{
    return (static_cast<std::uint64_t>(readBig32(bytes, at)) << 32U) |
        readBig32(bytes, at + 4U);
}

std::uint16_t readLittle16(std::span<const std::uint8_t> bytes, std::size_t at)
{
    return static_cast<std::uint16_t>(bytes[at]) |
        (static_cast<std::uint16_t>(bytes[at + 1U]) << 8U);
}

std::uint32_t readLittle32(std::span<const std::uint8_t> bytes, std::size_t at)
{
    return static_cast<std::uint32_t>(bytes[at]) |
        (static_cast<std::uint32_t>(bytes[at + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[at + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[at + 3U]) << 24U);
}

std::uint64_t readLittle64(std::span<const std::uint8_t> bytes, std::size_t at)
{
    return static_cast<std::uint64_t>(readLittle32(bytes, at)) |
        (static_cast<std::uint64_t>(readLittle32(bytes, at + 4U)) << 32U);
}

constexpr std::uint32_t fourCc(char a, char b, char c, char d) noexcept
{
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24U) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16U) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8U) |
        static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

std::size_t skipId3v2(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 10 || bytes[0] != 'I' || bytes[1] != 'D' ||
        bytes[2] != '3') {
        return 0;
    }
    if ((bytes[6] | bytes[7] | bytes[8] | bytes[9]) & 0x80U) {
        return 0;
    }
    const std::size_t payload =
        (static_cast<std::size_t>(bytes[6]) << 21U) |
        (static_cast<std::size_t>(bytes[7]) << 14U) |
        (static_cast<std::size_t>(bytes[8]) << 7U) |
        static_cast<std::size_t>(bytes[9]);
    const std::size_t footer = (bytes[5] & 0x10U) != 0 ? 10 : 0;
    return std::min(bytes.size(), 10 + payload + footer);
}

std::uint8_t flacCrc8(std::span<const std::uint8_t> bytes)
{
    std::uint8_t crc = 0;
    for (const auto value : bytes) {
        crc ^= value;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80U) != 0
                ? static_cast<std::uint8_t>((crc << 1U) ^ 0x07U)
                : static_cast<std::uint8_t>(crc << 1U);
        }
    }
    return crc;
}

std::uint16_t flacCrc16(std::span<const std::uint8_t> bytes)
{
    std::uint16_t crc = 0;
    for (const auto value : bytes) {
        crc ^= static_cast<std::uint16_t>(value) << 8U;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) != 0
                ? static_cast<std::uint16_t>((crc << 1U) ^ 0x8005U)
                : static_cast<std::uint16_t>(crc << 1U);
        }
    }
    return crc;
}

struct Utf8Integer {
    std::uint64_t value = 0;
    std::size_t bytes = 0;
};

std::optional<Utf8Integer> decodeFlacUtf8Integer(
    std::span<const std::uint8_t> bytes,
    std::size_t at)
{
    if (at >= bytes.size()) {
        return std::nullopt;
    }
    const auto first = bytes[at];
    if ((first & 0x80U) == 0) {
        return Utf8Integer{first, 1};
    }

    std::size_t length = 0;
    for (std::uint8_t mask = 0x80U; (first & mask) != 0; mask >>= 1U) {
        ++length;
    }
    if (length < 2 || length > 7 || at + length > bytes.size()) {
        return std::nullopt;
    }

    std::uint64_t value = first & static_cast<std::uint8_t>(0x7FU >> length);
    for (std::size_t i = 1; i < length; ++i) {
        const auto next = bytes[at + i];
        if ((next & 0xC0U) != 0x80U) {
            return std::nullopt;
        }
        value = (value << 6U) | (next & 0x3FU);
    }
    return Utf8Integer{value, length};
}

struct FlacHeader {
    std::size_t offset = 0;
    std::uint64_t sampleNumber = 0;
    std::uint32_t blockSize = 0;
};

std::optional<FlacHeader> parseFlacHeader(
    std::span<const std::uint8_t> bytes,
    std::size_t at)
{
    if (at + 7 > bytes.size() || bytes[at] != 0xFFU ||
        (bytes[at + 1] & 0xFEU) != 0xF8U) {
        return std::nullopt;
    }

    const bool variableBlock = (bytes[at + 1] & 0x01U) != 0;
    const auto blockCode = bytes[at + 2] >> 4U;
    const auto sampleRateCode = bytes[at + 2] & 0x0FU;
    const auto channelAssignment = bytes[at + 3] >> 4U;
    const auto sampleSizeCode = (bytes[at + 3] >> 1U) & 0x07U;
    if (blockCode == 0 || sampleRateCode == 0x0FU ||
        channelAssignment > 10 || sampleSizeCode == 3 ||
        sampleSizeCode == 7 || (bytes[at + 3] & 0x01U) != 0) {
        return std::nullopt;
    }

    const auto number = decodeFlacUtf8Integer(bytes, at + 4);
    if (!number) {
        return std::nullopt;
    }
    std::size_t cursor = at + 4 + number->bytes;
    std::uint32_t blockSize = 0;
    if (blockCode == 1) {
        blockSize = 192;
    } else if (blockCode >= 2 && blockCode <= 5) {
        blockSize = 576U << (blockCode - 2U);
    } else if (blockCode == 6) {
        if (cursor >= bytes.size()) return std::nullopt;
        blockSize = static_cast<std::uint32_t>(bytes[cursor++]) + 1U;
    } else if (blockCode == 7) {
        if (cursor + 2 > bytes.size()) return std::nullopt;
        blockSize = static_cast<std::uint32_t>(readBig16(bytes, cursor)) + 1U;
        cursor += 2;
    } else {
        blockSize = 256U << (blockCode - 8U);
    }

    if (sampleRateCode == 12) {
        cursor += 1;
    } else if (sampleRateCode == 13 || sampleRateCode == 14) {
        cursor += 2;
    }
    if (cursor >= bytes.size()) {
        return std::nullopt;
    }

    const auto headerWithCrc = bytes.subspan(at, cursor - at + 1);
    if (flacCrc8(headerWithCrc) != 0) {
        return std::nullopt;
    }

    return FlacHeader{
        at,
        variableBlock ? number->value : number->value * blockSize,
        blockSize,
    };
}

struct Mp3Header {
    std::uint32_t sampleRate = 0;
    std::uint32_t frameBytes = 0;
    std::uint32_t samplesPerFrame = 0;
    std::uint8_t version = 0;
    std::uint8_t layer = 0;
    bool mono = false;
};

std::optional<Mp3Header> parseMp3Header(
    std::span<const std::uint8_t> bytes,
    std::size_t at)
{
    if (at + 4 > bytes.size()) {
        return std::nullopt;
    }
    const auto header = readBig32(bytes, at);
    if ((header & 0xFFE00000U) != 0xFFE00000U) {
        return std::nullopt;
    }

    const auto version = static_cast<std::uint8_t>((header >> 19U) & 0x03U);
    const auto layer = static_cast<std::uint8_t>((header >> 17U) & 0x03U);
    const auto bitrateIndex = static_cast<std::uint8_t>((header >> 12U) & 0x0FU);
    const auto rateIndex = static_cast<std::uint8_t>((header >> 10U) & 0x03U);
    const auto padding = static_cast<std::uint8_t>((header >> 9U) & 0x01U);
    const auto channelMode = static_cast<std::uint8_t>((header >> 6U) & 0x03U);
    if (version == 1 || layer == 0 || bitrateIndex == 0 ||
        bitrateIndex == 15 || rateIndex == 3) {
        return std::nullopt;
    }

    static constexpr std::array<std::uint16_t, 16> kMpeg1Layer1{
        0, 32, 64, 96, 128, 160, 192, 224,
        256, 288, 320, 352, 384, 416, 448, 0};
    static constexpr std::array<std::uint16_t, 16> kMpeg1Layer2{
        0, 32, 48, 56, 64, 80, 96, 112,
        128, 160, 192, 224, 256, 320, 384, 0};
    static constexpr std::array<std::uint16_t, 16> kMpeg1Layer3{
        0, 32, 40, 48, 56, 64, 80, 96,
        112, 128, 160, 192, 224, 256, 320, 0};
    static constexpr std::array<std::uint16_t, 16> kMpeg2Layer1{
        0, 32, 48, 56, 64, 80, 96, 112,
        128, 144, 160, 176, 192, 224, 256, 0};
    static constexpr std::array<std::uint16_t, 16> kMpeg2Layer23{
        0, 8, 16, 24, 32, 40, 48, 56,
        64, 80, 96, 112, 128, 144, 160, 0};
    static constexpr std::array<std::uint32_t, 3> kRates{44100, 48000, 32000};

    const bool mpeg1 = version == 3;
    const auto bitrateKbps = mpeg1
        ? (layer == 3 ? kMpeg1Layer1[bitrateIndex]
                      : layer == 2 ? kMpeg1Layer2[bitrateIndex]
                                   : kMpeg1Layer3[bitrateIndex])
        : (layer == 3 ? kMpeg2Layer1[bitrateIndex]
                      : kMpeg2Layer23[bitrateIndex]);
    std::uint32_t sampleRate = kRates[rateIndex];
    if (version == 2) sampleRate /= 2;
    if (version == 0) sampleRate /= 4;

    std::uint32_t frameBytes = 0;
    std::uint32_t samplesPerFrame = 0;
    if (layer == 3) {
        frameBytes = ((12U * bitrateKbps * 1000U) / sampleRate + padding) * 4U;
        samplesPerFrame = 384;
    } else if (layer == 2) {
        frameBytes = (144U * bitrateKbps * 1000U) / sampleRate + padding;
        samplesPerFrame = 1152;
    } else {
        frameBytes = ((mpeg1 ? 144U : 72U) * bitrateKbps * 1000U) /
            sampleRate + padding;
        samplesPerFrame = mpeg1 ? 1152 : 576;
    }
    if (frameBytes < 4 || at + frameBytes > bytes.size()) {
        return std::nullopt;
    }
    return Mp3Header{
        sampleRate,
        frameBytes,
        samplesPerFrame,
        version,
        layer,
        channelMode == 3,
    };
}

bool sameMp3Stream(const Mp3Header& left, const Mp3Header& right)
{
    return left.version == right.version && left.layer == right.layer &&
        left.sampleRate == right.sampleRate;
}

double percentile(std::vector<double> values, double ratio)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(std::floor(
        std::clamp(ratio, 0.0, 1.0) *
        static_cast<double>(values.size() - 1)));
    return values[index];
}

PacketCostLevel aggregateLevel(
    const PacketCostLevel& base,
    std::uint32_t resolutionMilliseconds)
{
    PacketCostLevel output;
    output.resolutionMilliseconds = resolutionMilliseconds;
    if (base.bins.empty()) {
        return output;
    }
    const auto group = std::max<std::size_t>(
        1, resolutionMilliseconds / base.resolutionMilliseconds);
    for (std::size_t at = 0; at < base.bins.size(); at += group) {
        const auto end = std::min(base.bins.size(), at + group);
        double weightedKbps = 0.0;
        double duration = 0.0;
        float peakNormalized = 0.0F;
        for (std::size_t i = at; i < end; ++i) {
            weightedKbps += base.bins[i].kilobitsPerSecond *
                base.bins[i].durationSeconds;
            duration += base.bins[i].durationSeconds;
            peakNormalized = std::max(
                peakNormalized, base.bins[i].normalized);
        }
        output.bins.push_back({
            base.bins[at].startSeconds,
            duration,
            duration > 0.0 ? weightedKbps / duration : 0.0,
            peakNormalized,
        });
    }
    return output;
}

} // namespace

PacketParseResult parseFlacPackets(std::span<const std::uint8_t> bytes)
{
    PacketParseResult result;
    result.codec = PacketCodec::flac;
    std::size_t cursor = skipId3v2(bytes);
    if (cursor + 4 > bytes.size() || bytes[cursor] != 'f' ||
        bytes[cursor + 1] != 'L' || bytes[cursor + 2] != 'a' ||
        bytes[cursor + 3] != 'C') {
        result.error = "FLAC marker not found";
        return result;
    }
    cursor += 4;

    std::uint64_t totalSamples = 0;
    bool last = false;
    while (!last) {
        if (cursor + 4 > bytes.size()) {
            result.error = "Truncated FLAC metadata";
            return result;
        }
        last = (bytes[cursor] & 0x80U) != 0;
        const auto type = bytes[cursor] & 0x7FU;
        const auto length = readBig24(bytes, cursor + 1);
        cursor += 4;
        if (cursor + length > bytes.size()) {
            result.error = "Invalid FLAC metadata length";
            return result;
        }
        if (type == 0 && length >= 34) {
            std::uint64_t packed = 0;
            for (std::size_t i = 10; i < 18; ++i) {
                packed = (packed << 8U) | bytes[cursor + i];
            }
            result.sampleRate = static_cast<std::uint32_t>(packed >> 44U);
            totalSamples = packed & 0x0FFFFFFFFFULL;
        }
        cursor += length;
    }
    if (result.sampleRate == 0) {
        result.error = "FLAC STREAMINFO has no sample rate";
        return result;
    }

    std::vector<FlacHeader> candidates;
    candidates.reserve(4096);
    for (std::size_t at = cursor; at + 7 <= bytes.size(); ++at) {
        if (bytes[at] != 0xFFU) continue;
        if (auto header = parseFlacHeader(bytes, at)) {
            candidates.push_back(*header);
        }
    }
    if (candidates.empty()) {
        result.error = "No valid FLAC frame headers";
        return result;
    }

    std::size_t index = 0;
    while (index < candidates.size()) {
        std::optional<std::size_t> nextIndex;
        for (std::size_t next = index + 1;
             next < candidates.size() && next <= index + 64;
             ++next) {
            const auto begin = candidates[index].offset;
            const auto end = candidates[next].offset;
            if (end > begin + 2 &&
                flacCrc16(bytes.subspan(begin, end - begin)) == 0) {
                nextIndex = next;
                break;
            }
        }

        const auto begin = candidates[index].offset;
        const auto end = nextIndex
            ? candidates[*nextIndex].offset
            : bytes.size();
        const bool validLast = !nextIndex && end > begin + 2 &&
            flacCrc16(bytes.subspan(begin, end - begin)) == 0;
        if (nextIndex || validLast) {
            const auto& header = candidates[index];
            result.packets.push_back({
                static_cast<std::uint64_t>(begin),
                static_cast<std::uint32_t>(end - begin),
                static_cast<double>(header.sampleNumber) /
                    result.sampleRate,
                static_cast<double>(header.blockSize) /
                    result.sampleRate,
            });
        }

        if (!nextIndex) {
            break;
        }
        index = *nextIndex;
    }

    if (result.packets.empty()) {
        result.error = "FLAC frame CRC validation failed";
        return result;
    }
    result.durationSeconds = totalSamples > 0
        ? static_cast<double>(totalSamples) / result.sampleRate
        : result.packets.back().startSeconds +
            result.packets.back().durationSeconds;
    return result;
}

PacketParseResult parseMp3Packets(std::span<const std::uint8_t> bytes)
{
    PacketParseResult result;
    result.codec = PacketCodec::mp3;
    std::size_t cursor = skipId3v2(bytes);
    std::optional<Mp3Header> first;

    for (; cursor + 8 <= bytes.size(); ++cursor) {
        auto candidate = parseMp3Header(bytes, cursor);
        if (!candidate) continue;
        auto next = parseMp3Header(bytes, cursor + candidate->frameBytes);
        if (next && sameMp3Stream(*candidate, *next)) {
            first = candidate;
            break;
        }
    }
    if (!first) {
        result.error = "No consecutive MP3 frames";
        return result;
    }

    result.sampleRate = first->sampleRate;
    double time = 0.0;
    bool isFirst = true;
    while (cursor + 4 <= bytes.size()) {
        const auto header = parseMp3Header(bytes, cursor);
        if (!header || !sameMp3Stream(*first, *header)) {
            break;
        }
        const double duration = static_cast<double>(header->samplesPerFrame) /
            header->sampleRate;
        result.packets.push_back({
            static_cast<std::uint64_t>(cursor),
            header->frameBytes,
            time,
            duration,
        });

        if (isFirst) {
            const std::size_t sideInfo = header->version == 3
                ? (header->mono ? 17 : 32)
                : (header->mono ? 9 : 17);
            const auto xingAt = cursor + 4 + sideInfo;
            const auto vbriAt = cursor + 4 + 32;
            const auto hasWord = [&](std::size_t at, std::string_view word) {
                return at + word.size() <= bytes.size() &&
                    std::equal(word.begin(), word.end(), bytes.begin() + at);
            };
            result.variableBitrateHeader =
                hasWord(xingAt, "Xing") || hasWord(xingAt, "Info") ||
                hasWord(vbriAt, "VBRI");
            isFirst = false;
        }

        time += duration;
        cursor += header->frameBytes;
    }
    result.durationSeconds = time;
    if (result.packets.size() < 2) {
        result.error = "MP3 stream ended before two complete frames";
        result.packets.clear();
    }
    return result;
}

PacketParseResult parseAacAdtsPackets(std::span<const std::uint8_t> bytes)
{
    PacketParseResult result;
    result.codec = PacketCodec::aacAdts;
    static constexpr std::array<std::uint32_t, 13> kRates{
        96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350};

    struct Header {
        std::uint32_t rate = 0;
        std::uint32_t bytes = 0;
        std::uint32_t samples = 0;
    };
    const auto parse = [&](std::size_t at) -> std::optional<Header> {
        if (at + 7U > bytes.size() || bytes[at] != 0xFFU ||
            (bytes[at + 1U] & 0xF6U) != 0xF0U) {
            return std::nullopt;
        }
        const auto rateIndex = (bytes[at + 2U] >> 2U) & 0x0FU;
        if (rateIndex >= kRates.size()) return std::nullopt;
        const auto frameBytes =
            (static_cast<std::uint32_t>(bytes[at + 3U] & 0x03U) << 11U) |
            (static_cast<std::uint32_t>(bytes[at + 4U]) << 3U) |
            (bytes[at + 5U] >> 5U);
        const auto headerBytes = (bytes[at + 1U] & 0x01U) != 0 ? 7U : 9U;
        if (frameBytes < headerBytes || at + frameBytes > bytes.size()) {
            return std::nullopt;
        }
        return Header{kRates[rateIndex], frameBytes,
            1024U * (1U + (bytes[at + 6U] & 0x03U))};
    };

    std::size_t cursor = skipId3v2(bytes);
    std::optional<Header> first;
    for (; cursor + 14U <= bytes.size(); ++cursor) {
        auto candidate = parse(cursor);
        if (!candidate) continue;
        const auto next = parse(cursor + candidate->bytes);
        if (next && next->rate == candidate->rate) {
            first = candidate;
            break;
        }
    }
    if (!first) {
        result.error = "No consecutive AAC ADTS frames";
        return result;
    }
    result.sampleRate = first->rate;
    double time = 0.0;
    while (cursor + 7U <= bytes.size()) {
        const auto header = parse(cursor);
        if (!header || header->rate != result.sampleRate) break;
        const double duration = static_cast<double>(header->samples) /
            header->rate;
        result.packets.push_back({static_cast<std::uint64_t>(cursor),
            header->bytes, time, duration});
        time += duration;
        cursor += header->bytes;
    }
    result.durationSeconds = time;
    if (result.packets.size() < 2U) {
        result.error = "AAC ADTS stream ended before two complete frames";
        result.packets.clear();
    }
    return result;
}

namespace {

struct IsoBox {
    std::uint32_t type = 0;
    std::size_t payload = 0;
    std::size_t end = 0;
};

bool nextIsoBox(std::span<const std::uint8_t> bytes, std::size_t& cursor,
    std::size_t limit, IsoBox& box)
{
    if (cursor > limit || limit - cursor < 8U) return false;
    const auto at = cursor;
    std::uint64_t size = readBig32(bytes, at);
    const auto type = readBig32(bytes, at + 4U);
    std::size_t header = 8U;
    if (size == 1U) {
        if (limit - at < 16U) return false;
        size = readBig64(bytes, at + 8U);
        header = 16U;
    } else if (size == 0U) {
        size = limit - at;
    }
    if (size < header || size > limit - at) return false;
    box = {type, at + header, at + static_cast<std::size_t>(size)};
    cursor = box.end;
    return true;
}

std::optional<IsoBox> findIsoChild(std::span<const std::uint8_t> bytes,
    std::size_t begin, std::size_t end, std::uint32_t type)
{
    std::size_t cursor = begin;
    IsoBox box;
    while (nextIsoBox(bytes, cursor, end, box)) {
        if (box.type == type) return box;
    }
    return std::nullopt;
}

bool readIsoSampleSizes(std::span<const std::uint8_t> bytes,
    const IsoBox& stbl, std::vector<std::uint32_t>& sizes)
{
    constexpr std::size_t kMaxSamples = 10U * 1024U * 1024U;
    if (const auto stsz = findIsoChild(bytes, stbl.payload, stbl.end,
            fourCc('s', 't', 's', 'z'))) {
        if (stsz->end - stsz->payload < 12U) return false;
        const auto uniform = readBig32(bytes, stsz->payload + 4U);
        const auto count = readBig32(bytes, stsz->payload + 8U);
        if (count == 0U || count > kMaxSamples) return false;
        sizes.resize(count, uniform);
        if (uniform == 0U) {
            if (stsz->end - (stsz->payload + 12U) <
                static_cast<std::size_t>(count) * 4U) return false;
            for (std::size_t i = 0; i < count; ++i) {
                sizes[i] = readBig32(bytes, stsz->payload + 12U + i * 4U);
            }
        }
        return std::none_of(sizes.begin(), sizes.end(),
            [](std::uint32_t value) { return value == 0U; });
    }
    const auto stz2 = findIsoChild(bytes, stbl.payload, stbl.end,
        fourCc('s', 't', 'z', '2'));
    if (!stz2 || stz2->end - stz2->payload < 12U) return false;
    const auto field = bytes[stz2->payload + 7U];
    const auto count = readBig32(bytes, stz2->payload + 8U);
    if (count == 0U || count > kMaxSamples ||
        (field != 4U && field != 8U && field != 16U)) return false;
    const std::size_t required = field == 4U ? (count + 1U) / 2U :
        field == 8U ? count : static_cast<std::size_t>(count) * 2U;
    if (stz2->end - (stz2->payload + 12U) < required) return false;
    sizes.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto at = stz2->payload + 12U;
        sizes[i] = field == 4U
            ? ((i & 1U) == 0U ? bytes[at + i / 2U] >> 4U
                               : bytes[at + i / 2U] & 0x0FU)
            : field == 8U ? bytes[at + i]
                          : readBig16(bytes, at + i * 2U);
    }
    return true;
}

PacketParseResult parseIsoAudioTrack(std::span<const std::uint8_t> bytes,
    const IsoBox& trak)
{
    PacketParseResult result;
    result.codec = PacketCodec::isoBmff;
    const auto mdia = findIsoChild(bytes, trak.payload, trak.end,
        fourCc('m', 'd', 'i', 'a'));
    if (!mdia) return result;
    const auto hdlr = findIsoChild(bytes, mdia->payload, mdia->end,
        fourCc('h', 'd', 'l', 'r'));
    if (!hdlr || hdlr->end - hdlr->payload < 12U ||
        readBig32(bytes, hdlr->payload + 8U) != fourCc('s', 'o', 'u', 'n')) {
        return result;
    }
    const auto mdhd = findIsoChild(bytes, mdia->payload, mdia->end,
        fourCc('m', 'd', 'h', 'd'));
    if (!mdhd || mdhd->end - mdhd->payload < 20U) return result;
    const bool version1 = bytes[mdhd->payload] == 1U;
    const auto rateAt = mdhd->payload + (version1 ? 20U : 12U);
    if (rateAt + 4U > mdhd->end) return result;
    const auto timescale = readBig32(bytes, rateAt);
    if (timescale == 0U) return result;

    const auto minf = findIsoChild(bytes, mdia->payload, mdia->end,
        fourCc('m', 'i', 'n', 'f'));
    const auto stbl = minf ? findIsoChild(bytes, minf->payload, minf->end,
        fourCc('s', 't', 'b', 'l')) : std::nullopt;
    if (!stbl) return result;
    std::vector<std::uint32_t> sizes;
    if (!readIsoSampleSizes(bytes, *stbl, sizes)) return result;
    const auto stts = findIsoChild(bytes, stbl->payload, stbl->end,
        fourCc('s', 't', 't', 's'));
    if (!stts || stts->end - stts->payload < 8U) return result;
    const auto entryCount = readBig32(bytes, stts->payload + 4U);
    if (entryCount == 0U ||
        stts->end - (stts->payload + 8U) <
            static_cast<std::size_t>(entryCount) * 8U) return result;

    result.sampleRate = timescale;
    result.packets.reserve(sizes.size());
    std::size_t sample = 0;
    std::uint64_t ticks = 0;
    for (std::size_t entry = 0; entry < entryCount && sample < sizes.size();
         ++entry) {
        const auto count = readBig32(bytes, stts->payload + 8U + entry * 8U);
        const auto delta = readBig32(bytes, stts->payload + 12U + entry * 8U);
        if (count == 0U || delta == 0U || count > sizes.size() - sample) {
            result.packets.clear();
            return result;
        }
        for (std::uint32_t i = 0; i < count; ++i, ++sample) {
            result.packets.push_back({0U, sizes[sample],
                static_cast<double>(ticks) / timescale,
                static_cast<double>(delta) / timescale});
            ticks += delta;
        }
    }
    if (sample != sizes.size()) {
        result.packets.clear();
        return result;
    }
    result.durationSeconds = static_cast<double>(ticks) / timescale;
    return result;
}

struct FragmentedIsoTrack {
    std::uint32_t id = 0;
    std::uint32_t timescale = 0;
    std::uint32_t defaultDuration = 0;
    std::uint32_t defaultSize = 0;
};

std::optional<FragmentedIsoTrack> findFragmentedAudioTrack(
    std::span<const std::uint8_t> bytes, const IsoBox& moov)
{
    FragmentedIsoTrack track;
    std::size_t cursor = moov.payload;
    IsoBox box;
    while (nextIsoBox(bytes, cursor, moov.end, box)) {
        if (box.type != fourCc('t','r','a','k')) continue;
        const auto tkhd = findIsoChild(bytes, box.payload, box.end,
            fourCc('t','k','h','d'));
        const auto mdia = findIsoChild(bytes, box.payload, box.end,
            fourCc('m','d','i','a'));
        if (!tkhd || !mdia) continue;
        const auto hdlr = findIsoChild(bytes, mdia->payload, mdia->end,
            fourCc('h','d','l','r'));
        const auto mdhd = findIsoChild(bytes, mdia->payload, mdia->end,
            fourCc('m','d','h','d'));
        if (!hdlr || !mdhd || hdlr->end - hdlr->payload < 12U ||
            readBig32(bytes, hdlr->payload + 8U) != fourCc('s','o','u','n')) {
            continue;
        }
        const bool tkhdV1 = bytes[tkhd->payload] == 1U;
        const auto idAt = tkhd->payload + (tkhdV1 ? 20U : 12U);
        const bool mdhdV1 = bytes[mdhd->payload] == 1U;
        const auto rateAt = mdhd->payload + (mdhdV1 ? 20U : 12U);
        if (idAt + 4U > tkhd->end || rateAt + 4U > mdhd->end) continue;
        track.id = readBig32(bytes, idAt);
        track.timescale = readBig32(bytes, rateAt);
        if (track.id != 0U && track.timescale != 0U) break;
    }
    if (track.id == 0U || track.timescale == 0U) return std::nullopt;

    const auto mvex = findIsoChild(bytes, moov.payload, moov.end,
        fourCc('m','v','e','x'));
    if (mvex) {
        cursor = mvex->payload;
        while (nextIsoBox(bytes, cursor, mvex->end, box)) {
            if (box.type != fourCc('t','r','e','x') ||
                box.end - box.payload < 24U ||
                readBig32(bytes, box.payload + 4U) != track.id) continue;
            track.defaultDuration = readBig32(bytes, box.payload + 12U);
            track.defaultSize = readBig32(bytes, box.payload + 16U);
            break;
        }
    }
    return track;
}

PacketParseResult parseFragmentedIsoAudio(std::span<const std::uint8_t> bytes,
    const IsoBox& moov)
{
    PacketParseResult result;
    result.codec = PacketCodec::isoBmff;
    const auto selected = findFragmentedAudioTrack(bytes, moov);
    if (!selected) return result;
    result.sampleRate = selected->timescale;
    std::uint64_t runningTicks = 0;
    std::size_t topCursor = 0;
    IsoBox top;
    while (nextIsoBox(bytes, topCursor, bytes.size(), top)) {
        if (top.type != fourCc('m','o','o','f')) continue;
        std::size_t moofCursor = top.payload;
        IsoBox traf;
        while (nextIsoBox(bytes, moofCursor, top.end, traf)) {
            if (traf.type != fourCc('t','r','a','f')) continue;
            const auto tfhd = findIsoChild(bytes, traf.payload, traf.end,
                fourCc('t','f','h','d'));
            if (!tfhd || tfhd->end - tfhd->payload < 8U ||
                readBig32(bytes, tfhd->payload + 4U) != selected->id) continue;
            const auto tfhdFlags = readBig32(bytes, tfhd->payload) & 0x00FFFFFFU;
            std::size_t tfhdAt = tfhd->payload + 8U;
            if ((tfhdFlags & 0x000001U) != 0U) tfhdAt += 8U;
            if ((tfhdFlags & 0x000002U) != 0U) tfhdAt += 4U;
            std::uint32_t defaultDuration = selected->defaultDuration;
            std::uint32_t defaultSize = selected->defaultSize;
            if ((tfhdFlags & 0x000008U) != 0U) {
                if (tfhdAt + 4U > tfhd->end) continue;
                defaultDuration = readBig32(bytes, tfhdAt);
                tfhdAt += 4U;
            }
            if ((tfhdFlags & 0x000010U) != 0U) {
                if (tfhdAt + 4U > tfhd->end) continue;
                defaultSize = readBig32(bytes, tfhdAt);
                tfhdAt += 4U;
            }

            std::uint64_t decodeTicks = runningTicks;
            if (const auto tfdt = findIsoChild(bytes, traf.payload, traf.end,
                    fourCc('t','f','d','t'))) {
                if (tfdt->end - tfdt->payload >= 8U) {
                    decodeTicks = bytes[tfdt->payload] == 1U
                        ? (tfdt->end - tfdt->payload >= 12U
                            ? readBig64(bytes, tfdt->payload + 4U) : decodeTicks)
                        : readBig32(bytes, tfdt->payload + 4U);
                }
            }

            std::size_t trafCursor = traf.payload;
            IsoBox child;
            while (nextIsoBox(bytes, trafCursor, traf.end, child)) {
                if (child.type != fourCc('t','r','u','n') ||
                    child.end - child.payload < 8U) continue;
                const auto flags = readBig32(bytes, child.payload) & 0x00FFFFFFU;
                const auto count = readBig32(bytes, child.payload + 4U);
                if (count == 0U || count > 10U * 1024U * 1024U) continue;
                std::size_t at = child.payload + 8U;
                if ((flags & 0x000001U) != 0U) at += 4U;
                if ((flags & 0x000004U) != 0U) at += 4U;
                const std::size_t fields =
                    ((flags & 0x000100U) != 0U ? 4U : 0U) +
                    ((flags & 0x000200U) != 0U ? 4U : 0U) +
                    ((flags & 0x000400U) != 0U ? 4U : 0U) +
                    ((flags & 0x000800U) != 0U ? 4U : 0U);
                if (at > child.end || fields > 0U &&
                    static_cast<std::uint64_t>(count) * fields > child.end - at) {
                    continue;
                }
                for (std::uint32_t i = 0; i < count; ++i) {
                    std::uint32_t duration = defaultDuration;
                    std::uint32_t sampleBytes = defaultSize;
                    if ((flags & 0x000100U) != 0U) {
                        duration = readBig32(bytes, at); at += 4U;
                    }
                    if ((flags & 0x000200U) != 0U) {
                        sampleBytes = readBig32(bytes, at); at += 4U;
                    }
                    if ((flags & 0x000400U) != 0U) at += 4U;
                    if ((flags & 0x000800U) != 0U) at += 4U;
                    if (duration == 0U || sampleBytes == 0U) {
                        result.packets.clear();
                        return result;
                    }
                    result.packets.push_back({0U, sampleBytes,
                        static_cast<double>(decodeTicks) / selected->timescale,
                        static_cast<double>(duration) / selected->timescale});
                    decodeTicks += duration;
                }
            }
            runningTicks = std::max(runningTicks, decodeTicks);
        }
    }
    result.durationSeconds = static_cast<double>(runningTicks) /
        selected->timescale;
    return result;
}

struct OggPage {
    std::uint32_t serial = 0;
    std::uint64_t granule = 0;
    std::size_t at = 0;
    std::size_t payload = 0;
    std::size_t bodyBytes = 0;
    std::size_t totalBytes = 0;
};

std::vector<OggPage> readOggPages(std::span<const std::uint8_t> bytes)
{
    std::vector<OggPage> pages;
    std::size_t cursor = 0;
    while (cursor + 27U <= bytes.size()) {
        if (!std::equal(bytes.begin() + cursor, bytes.begin() + cursor + 4U,
                "OggS")) break;
        if (bytes[cursor + 4U] != 0U) break;
        const auto segmentCount = bytes[cursor + 26U];
        if (cursor + 27U + segmentCount > bytes.size()) break;
        std::size_t body = 0;
        for (std::size_t i = 0; i < segmentCount; ++i) {
            body += bytes[cursor + 27U + i];
        }
        const auto total = 27U + static_cast<std::size_t>(segmentCount) + body;
        if (total > bytes.size() - cursor) break;
        pages.push_back({readLittle32(bytes, cursor + 14U),
            readLittle64(bytes, cursor + 6U), cursor,
            cursor + 27U + segmentCount, body, total});
        cursor += total;
    }
    return pages;
}

double readExtended80(std::span<const std::uint8_t> bytes, std::size_t at)
{
    const auto signExponent = readBig16(bytes, at);
    const auto mantissa = readBig64(bytes, at + 2U);
    const auto exponent = signExponent & 0x7FFFU;
    if (exponent == 0U && mantissa == 0U) return 0.0;
    if (exponent == 0x7FFFU) return 0.0;
    const auto value = std::ldexp(static_cast<double>(mantissa),
        static_cast<int>(exponent) - 16383 - 63);
    return (signExponent & 0x8000U) != 0U ? -value : value;
}

struct DolbyFrame {
    PacketCodec codec = PacketCodec::unsupported;
    std::uint32_t bytes = 0;
    std::uint32_t rate = 0;
    std::uint32_t samples = 0;
};

std::optional<DolbyFrame> parseAc3Frame(
    std::span<const std::uint8_t> bytes, std::size_t at)
{
    if (at + 7U > bytes.size() || bytes[at] != 0x0BU ||
        bytes[at + 1U] != 0x77U) return std::nullopt;
    static constexpr std::array<std::array<std::uint16_t, 3>, 38> kWords{{
        {64,69,96},{64,70,96},{80,87,120},{80,88,120},
        {96,104,144},{96,105,144},{112,121,168},{112,122,168},
        {128,139,192},{128,140,192},{160,174,240},{160,175,240},
        {192,208,288},{192,209,288},{224,243,336},{224,244,336},
        {256,278,384},{256,279,384},{320,348,480},{320,349,480},
        {384,417,576},{384,418,576},{448,487,672},{448,488,672},
        {512,557,768},{512,558,768},{640,696,960},{640,697,960},
        {768,835,1152},{768,836,1152},{896,975,1344},{896,976,1344},
        {1024,1114,1536},{1024,1115,1536},{1152,1253,1728},
        {1152,1254,1728},{1280,1393,1920},{1280,1394,1920}}};
    static constexpr std::array<std::uint32_t, 3> kRates{48000,44100,32000};
    const auto fscod = bytes[at + 4U] >> 6U;
    const auto sizeCode = bytes[at + 4U] & 0x3FU;
    if (fscod >= 3U || sizeCode >= kWords.size()) return std::nullopt;
    const auto frameBytes = static_cast<std::uint32_t>(kWords[sizeCode][fscod]) * 2U;
    if (frameBytes < 7U || at + frameBytes > bytes.size()) return std::nullopt;
    return DolbyFrame{PacketCodec::ac3, frameBytes, kRates[fscod], 1536U};
}

std::optional<DolbyFrame> parseEac3Frame(
    std::span<const std::uint8_t> bytes, std::size_t at)
{
    if (at + 7U > bytes.size() || bytes[at] != 0x0BU ||
        bytes[at + 1U] != 0x77U) return std::nullopt;
    const auto frameBytes =
        ((((static_cast<std::uint32_t>(bytes[at + 2U]) & 0x07U) << 8U) |
          bytes[at + 3U]) + 1U) * 2U;
    if (frameBytes < 7U || frameBytes > 8192U ||
        at + frameBytes > bytes.size()) return std::nullopt;
    static constexpr std::array<std::uint32_t, 3> kRates{48000,44100,32000};
    const auto fscod = bytes[at + 4U] >> 6U;
    std::uint32_t rate = 0;
    std::uint32_t blocks = 6U;
    if (fscod == 3U) {
        const auto fscod2 = (bytes[at + 4U] >> 4U) & 0x03U;
        if (fscod2 >= 3U) return std::nullopt;
        rate = kRates[fscod2] / 2U;
    } else {
        static constexpr std::array<std::uint32_t, 4> kBlocks{1,2,3,6};
        rate = kRates[fscod];
        blocks = kBlocks[(bytes[at + 4U] >> 4U) & 0x03U];
    }
    return DolbyFrame{PacketCodec::eac3, frameBytes, rate, blocks * 256U};
}

} // namespace

PacketParseResult parseIsoBmffPackets(std::span<const std::uint8_t> bytes)
{
    PacketParseResult best;
    best.codec = PacketCodec::isoBmff;
    if (bytes.size() < 12U) {
        best.error = "ISO BMFF file is truncated";
        return best;
    }
    std::size_t cursor = 0;
    IsoBox box;
    std::optional<IsoBox> moov;
    while (nextIsoBox(bytes, cursor, bytes.size(), box)) {
        if (box.type == fourCc('m', 'o', 'o', 'v')) {
            moov = box;
            break;
        }
    }
    if (!moov) {
        best.error = "ISO BMFF movie metadata was not found";
        return best;
    }
    cursor = moov->payload;
    while (nextIsoBox(bytes, cursor, moov->end, box)) {
        if (box.type != fourCc('t', 'r', 'a', 'k')) continue;
        auto candidate = parseIsoAudioTrack(bytes, box);
        if (candidate.packets.size() > best.packets.size()) {
            best = std::move(candidate);
        }
    }
    if (best.packets.empty()) {
        auto fragmented = parseFragmentedIsoAudio(bytes, *moov);
        if (!fragmented.packets.empty()) return fragmented;
        best.error = "No audio sample table or fragment run was found";
    }
    return best;
}

PacketParseResult parseOggPackets(std::span<const std::uint8_t> bytes)
{
    PacketParseResult result;
    const auto prefix = skipId3v2(bytes);
    bytes = bytes.subspan(prefix);
    const auto pages = readOggPages(bytes);
    if (pages.empty()) {
        result.error = "No complete Ogg pages were found";
        return result;
    }
    std::uint32_t serial = 0;
    bool identified = false;
    unsigned headerPackets = 0;
    std::uint64_t preSkip = 0;
    for (const auto& page : pages) {
        const auto body = bytes.subspan(page.payload, page.bodyBytes);
        if (body.size() >= 19U &&
            std::equal(body.begin(), body.begin() + 8U, "OpusHead")) {
            serial = page.serial;
            identified = true;
            headerPackets = 2;
            result.codec = PacketCodec::oggOpus;
            result.sampleRate = 48000U;
            preSkip = readLittle16(body, 10U);
            break;
        }
        if (body.size() >= 16U && body[0] == 1U &&
            std::equal(body.begin() + 1U, body.begin() + 7U, "vorbis")) {
            serial = page.serial;
            identified = true;
            headerPackets = 3;
            result.codec = PacketCodec::oggVorbis;
            result.sampleRate = readLittle32(body, 12U);
            break;
        }
    }
    if (!identified || result.sampleRate == 0U) {
        result.error = "Ogg stream is not Opus or Vorbis";
        return result;
    }
    std::uint64_t previous = 0;
    std::uint64_t pendingBytes = 0;
    for (const auto& page : pages) {
        if (page.serial != serial) continue;
        // Lacing distinguishes a continued comment/artwork header from an
        // audio packet that spans pages before its first valid granule (RFC
        // 7845 sections 3/4). Neither drop that audio nor count the artwork.
        std::uint64_t audioBytes = 0;
        unsigned audioSegments = 0;
        const auto segmentCount = bytes[page.at + 26U];
        for (unsigned i = 0; i < segmentCount; ++i) {
            const auto length = bytes[page.at + 27U + i];
            if (headerPackets == 0) {
                audioBytes += length;
                ++audioSegments;
            } else if (length < 255U) {
                --headerPackets;
            }
        }
        if (audioSegments == 0) continue;
        pendingBytes += audioBytes + 27U + audioSegments;
        if (page.granule == std::numeric_limits<std::uint64_t>::max()) {
            continue;
        }
        const auto adjusted = page.granule > preSkip ? page.granule - preSkip : 0U;
        if (adjusted < previous) {
            result.error = "Ogg audio granule position moved backwards";
            result.packets.clear();
            return result;
        }
        if (adjusted <= previous) continue;
        const auto bytesForPage = pendingBytes;
        pendingBytes = 0;
        if (bytesForPage >
            std::numeric_limits<std::uint32_t>::max()) continue;
        const double start = static_cast<double>(previous) / result.sampleRate;
        const double duration = static_cast<double>(adjusted - previous) /
            result.sampleRate;
        result.packets.push_back({static_cast<std::uint64_t>(prefix + page.at),
            static_cast<std::uint32_t>(bytesForPage), start, duration});
        previous = adjusted;
    }
    result.durationSeconds = static_cast<double>(previous) / result.sampleRate;
    if (result.packets.empty()) {
        result.error = "Ogg audio pages contain no usable granule positions";
    }
    return result;
}

PacketParseResult parseWavePackets(std::span<const std::uint8_t> bytes)
{
    PacketParseResult result;
    result.codec = PacketCodec::wave;
    if (bytes.size() < 12U ||
        (!std::equal(bytes.begin(), bytes.begin() + 4U, "RIFF") &&
         !std::equal(bytes.begin(), bytes.begin() + 4U, "RF64")) ||
        !std::equal(bytes.begin() + 8U, bytes.begin() + 12U, "WAVE")) {
        result.error = "RIFF/RF64 WAVE header was not found";
        return result;
    }
    std::uint64_t rf64DataSize = 0;
    std::uint32_t byteRate = 0;
    std::uint32_t sampleRate = 0;
    std::size_t dataAt = 0;
    std::uint64_t dataBytes = 0;
    std::size_t cursor = 12U;
    while (cursor + 8U <= bytes.size()) {
        const auto type = readBig32(bytes, cursor);
        const auto size32 = readLittle32(bytes, cursor + 4U);
        const auto payload = cursor + 8U;
        std::uint64_t chunkBytes = size32;
        if (type == fourCc('d','s','6','4') && size32 >= 28U &&
            payload + 28U <= bytes.size()) {
            rf64DataSize = readLittle64(bytes, payload + 8U);
        }
        if (type == fourCc('f','m','t',' ') && size32 >= 16U &&
            payload + 16U <= bytes.size()) {
            sampleRate = readLittle32(bytes, payload + 4U);
            byteRate = readLittle32(bytes, payload + 8U);
        }
        if (type == fourCc('d','a','t','a')) {
            if (size32 == 0xFFFFFFFFU && rf64DataSize > 0U) {
                chunkBytes = rf64DataSize;
            }
            dataAt = payload;
            dataBytes = std::min<std::uint64_t>(chunkBytes,
                bytes.size() - payload);
            break;
        }
        if (chunkBytes > bytes.size() - payload) break;
        cursor = payload + static_cast<std::size_t>(chunkBytes) +
            static_cast<std::size_t>(chunkBytes & 1U);
    }
    if (byteRate == 0U || sampleRate == 0U || dataBytes == 0U ||
        dataBytes > std::numeric_limits<std::uint32_t>::max()) {
        result.error = "WAVE timing or data chunk is unavailable";
        return result;
    }
    result.sampleRate = sampleRate;
    result.durationSeconds = static_cast<double>(dataBytes) / byteRate;
    result.packets.push_back({static_cast<std::uint64_t>(dataAt),
        static_cast<std::uint32_t>(dataBytes), 0.0, result.durationSeconds});
    return result;
}

PacketParseResult parseAiffPackets(std::span<const std::uint8_t> bytes)
{
    PacketParseResult result;
    result.codec = PacketCodec::aiff;
    if (bytes.size() < 12U ||
        !std::equal(bytes.begin(), bytes.begin() + 4U, "FORM") ||
        (!std::equal(bytes.begin() + 8U, bytes.begin() + 12U, "AIFF") &&
         !std::equal(bytes.begin() + 8U, bytes.begin() + 12U, "AIFC"))) {
        result.error = "AIFF/AIFC header was not found";
        return result;
    }
    std::uint32_t frames = 0;
    double rate = 0.0;
    std::size_t audioAt = 0;
    std::uint64_t audioBytes = 0;
    std::size_t cursor = 12U;
    while (cursor + 8U <= bytes.size()) {
        const auto type = readBig32(bytes, cursor);
        const auto chunkBytes = readBig32(bytes, cursor + 4U);
        const auto payload = cursor + 8U;
        if (chunkBytes > bytes.size() - payload) break;
        if (type == fourCc('C','O','M','M') && chunkBytes >= 18U) {
            frames = readBig32(bytes, payload + 2U);
            rate = readExtended80(bytes, payload + 8U);
        } else if (type == fourCc('S','S','N','D') && chunkBytes >= 8U) {
            const auto offset = readBig32(bytes, payload);
            if (offset <= chunkBytes - 8U) {
                audioAt = payload + 8U + offset;
                audioBytes = chunkBytes - 8U - offset;
            }
        }
        cursor = payload + chunkBytes + (chunkBytes & 1U);
    }
    if (frames == 0U || rate <= 0.0 || !std::isfinite(rate) ||
        audioBytes == 0U || audioBytes > std::numeric_limits<std::uint32_t>::max()) {
        result.error = "AIFF timing or sound data is unavailable";
        return result;
    }
    result.sampleRate = static_cast<std::uint32_t>(std::lround(rate));
    result.durationSeconds = frames / rate;
    result.packets.push_back({static_cast<std::uint64_t>(audioAt),
        static_cast<std::uint32_t>(audioBytes), 0.0, result.durationSeconds});
    return result;
}

PacketParseResult parseWavPackPackets(std::span<const std::uint8_t> bytes)
{
    PacketParseResult result;
    result.codec = PacketCodec::wavPack;
    static constexpr std::array<std::uint32_t, 16> kRates{
        6000,8000,9600,11025,12000,16000,22050,24000,
        32000,44100,48000,64000,88200,96000,192000,0};
    std::size_t cursor = 0;
    std::uint32_t groupIndex = 0;
    std::uint32_t groupSamples = 0;
    std::uint64_t groupBytes = 0;
    std::size_t groupAt = 0;
    bool haveGroup = false;
    double time = 0.0;
    const auto flush = [&]() {
        if (!haveGroup || groupSamples == 0U || result.sampleRate == 0U ||
            groupBytes > std::numeric_limits<std::uint32_t>::max()) return;
        const double duration = static_cast<double>(groupSamples) /
            result.sampleRate;
        result.packets.push_back({static_cast<std::uint64_t>(groupAt),
            static_cast<std::uint32_t>(groupBytes), time, duration});
        time += duration;
    };
    while (cursor + 32U <= bytes.size()) {
        if (!std::equal(bytes.begin() + cursor, bytes.begin() + cursor + 4U,
                "wvpk")) break;
        const auto blockBytes = static_cast<std::uint64_t>(
            readLittle32(bytes, cursor + 4U)) + 8U;
        if (blockBytes < 32U || blockBytes > bytes.size() - cursor) break;
        const auto blockIndex = readLittle32(bytes, cursor + 16U);
        const auto blockSamples = readLittle32(bytes, cursor + 20U);
        const auto flags = readLittle32(bytes, cursor + 24U);
        const auto rate = kRates[(flags >> 23U) & 0x0FU];
        if (rate == 0U) break;
        if (result.sampleRate == 0U) result.sampleRate = rate;
        if (rate != result.sampleRate) break;
        if (!haveGroup || blockIndex != groupIndex) {
            flush();
            haveGroup = true;
            groupIndex = blockIndex;
            groupSamples = blockSamples;
            groupBytes = 0;
            groupAt = cursor;
        }
        if (blockSamples != groupSamples) break;
        groupBytes += blockBytes;
        cursor += static_cast<std::size_t>(blockBytes);
    }
    flush();
    result.durationSeconds = time;
    if (result.packets.empty()) result.error = "No complete WavPack blocks were found";
    return result;
}

PacketParseResult parseDolbyPackets(std::span<const std::uint8_t> bytes)
{
    PacketParseResult result;
    std::size_t cursor = 0;
    std::optional<DolbyFrame> first;
    for (; cursor + 14U <= bytes.size(); ++cursor) {
        const auto ac3 = parseAc3Frame(bytes, cursor);
        const auto eac3 = parseEac3Frame(bytes, cursor);
        const auto consecutive = [&](const std::optional<DolbyFrame>& frame) {
            if (!frame) return false;
            const auto nextAt = cursor + frame->bytes;
            const auto next = frame->codec == PacketCodec::ac3
                ? parseAc3Frame(bytes, nextAt) : parseEac3Frame(bytes, nextAt);
            return next && next->rate == frame->rate;
        };
        if (consecutive(ac3)) { first = ac3; break; }
        if (consecutive(eac3)) { first = eac3; break; }
    }
    if (!first) {
        result.error = "No consecutive AC-3/E-AC-3 frames were found";
        return result;
    }
    result.codec = first->codec;
    result.sampleRate = first->rate;
    double time = 0.0;
    while (cursor + 7U <= bytes.size()) {
        const auto frame = result.codec == PacketCodec::ac3
            ? parseAc3Frame(bytes, cursor) : parseEac3Frame(bytes, cursor);
        if (!frame || frame->rate != result.sampleRate) break;
        const double duration = static_cast<double>(frame->samples) / frame->rate;
        result.packets.push_back({static_cast<std::uint64_t>(cursor),
            frame->bytes, time, duration});
        time += duration;
        cursor += frame->bytes;
    }
    result.durationSeconds = time;
    return result;
}

PacketParseResult parseMonkeysAudioPackets(
    std::span<const std::uint8_t> bytes)
{
    PacketParseResult result;
    result.codec = PacketCodec::monkeysAudio;
    if (bytes.size() < 76U ||
        !std::equal(bytes.begin(), bytes.begin() + 4U, "MAC ")) {
        result.error = "Monkey's Audio header was not found";
        return result;
    }
    const auto version = readLittle16(bytes, 4U);
    if (version < 3980U) {
        result.error = "Monkey's Audio versions before 3.98 are unsupported";
        return result;
    }
    const auto descriptorBytes = readLittle16(bytes, 6U);
    const auto headerBytes = readLittle32(bytes, 8U);
    const auto seekBytes = readLittle32(bytes, 12U);
    const auto headerDataBytes = readLittle32(bytes, 16U);
    const auto frameDataBytes = readLittle32(bytes, 20U) |
        (static_cast<std::uint64_t>(readLittle32(bytes, 24U)) << 32U);
    if (descriptorBytes < 52U || headerBytes < 24U ||
        descriptorBytes > bytes.size() || headerBytes > bytes.size() - descriptorBytes) {
        result.error = "Monkey's Audio descriptor is invalid";
        return result;
    }
    const auto headerAt = static_cast<std::size_t>(descriptorBytes);
    const auto blocksPerFrame = readLittle32(bytes, headerAt + 4U);
    const auto finalFrameBlocks = readLittle32(bytes, headerAt + 8U);
    const auto frameCount = readLittle32(bytes, headerAt + 12U);
    const auto sampleRate = readLittle32(bytes, headerAt + 20U);
    if (blocksPerFrame == 0U || finalFrameBlocks == 0U || frameCount == 0U ||
        frameCount > 10U * 1024U * 1024U || sampleRate == 0U ||
        seekBytes < static_cast<std::uint64_t>(frameCount) * 4U) {
        result.error = "Monkey's Audio timing or seek table is invalid";
        return result;
    }
    const auto seekAt64 = static_cast<std::uint64_t>(descriptorBytes) + headerBytes;
    const auto dataAt64 = seekAt64 + seekBytes + headerDataBytes;
    if (seekAt64 > bytes.size() || dataAt64 > bytes.size() ||
        seekAt64 + static_cast<std::uint64_t>(frameCount) * 4U > bytes.size() ||
        frameDataBytes > bytes.size() - dataAt64) {
        result.error = "Monkey's Audio frame data is truncated";
        return result;
    }
    const auto seekAt = static_cast<std::size_t>(seekAt64);
    std::vector<std::uint32_t> offsets(frameCount);
    for (std::size_t i = 0; i < frameCount; ++i) {
        offsets[i] = readLittle32(bytes, seekAt + i * 4U);
        if (i > 0U && offsets[i] <= offsets[i - 1U]) {
            result.error = "Monkey's Audio seek table is not monotonic";
            return result;
        }
    }
    result.sampleRate = sampleRate;
    result.packets.reserve(frameCount);
    std::uint64_t consumed = 0;
    double time = 0.0;
    for (std::size_t i = 0; i < frameCount; ++i) {
        const std::uint64_t frameBytes = i + 1U < frameCount
            ? static_cast<std::uint64_t>(offsets[i + 1U] - offsets[i])
            : frameDataBytes - consumed;
        const auto samples = i + 1U == frameCount
            ? finalFrameBlocks : blocksPerFrame;
        if (frameBytes == 0U || frameBytes >
            std::numeric_limits<std::uint32_t>::max() || consumed > frameDataBytes ||
            frameBytes > frameDataBytes - consumed) {
            result.error = "Monkey's Audio frame sizes are inconsistent";
            result.packets.clear();
            return result;
        }
        const double duration = static_cast<double>(samples) / sampleRate;
        result.packets.push_back({offsets[i], static_cast<std::uint32_t>(frameBytes),
            time, duration});
        consumed += frameBytes;
        time += duration;
    }
    result.durationSeconds = time;
    return result;
}

PacketParseResult parseTtaPackets(std::span<const std::uint8_t> bytes)
{
    PacketParseResult result;
    result.codec = PacketCodec::tta;
    if (bytes.size() < 26U ||
        !std::equal(bytes.begin(), bytes.begin() + 4U, "TTA1")) {
        result.error = "TTA1 header was not found";
        return result;
    }
    const auto sampleRate = readLittle32(bytes, 10U);
    const auto totalSamples = readLittle32(bytes, 14U);
    if (sampleRate == 0U || totalSamples == 0U) {
        result.error = "TTA timing is invalid";
        return result;
    }
    const auto samplesPerFrame = std::max<std::uint64_t>(1U,
        (static_cast<std::uint64_t>(sampleRate) * 256U) / 245U);
    const auto frameCount = static_cast<std::size_t>(
        (totalSamples + samplesPerFrame - 1U) / samplesPerFrame);
    if (frameCount == 0U || frameCount > 10U * 1024U * 1024U ||
        22U + frameCount * 4U + 4U > bytes.size()) {
        result.error = "TTA seek table is truncated";
        return result;
    }
    result.sampleRate = sampleRate;
    result.packets.reserve(frameCount);
    std::size_t dataAt = 22U + frameCount * 4U + 4U;
    std::uint64_t sample = 0;
    for (std::size_t i = 0; i < frameCount; ++i) {
        const auto frameBytes = readLittle32(bytes, 22U + i * 4U);
        const auto frameSamples = std::min<std::uint64_t>(
            samplesPerFrame, totalSamples - sample);
        if (frameBytes == 0U || frameBytes > bytes.size() - dataAt) {
            result.error = "TTA frame data is truncated";
            result.packets.clear();
            return result;
        }
        result.packets.push_back({static_cast<std::uint64_t>(dataAt), frameBytes,
            static_cast<double>(sample) / sampleRate,
            static_cast<double>(frameSamples) / sampleRate});
        sample += frameSamples;
        dataAt += frameBytes;
    }
    result.durationSeconds = static_cast<double>(totalSamples) / sampleRate;
    return result;
}

PacketParseResult parseDsdPackets(std::span<const std::uint8_t> bytes)
{
    PacketParseResult result;
    result.codec = PacketCodec::dsd;
    if (bytes.size() >= 28U &&
        std::equal(bytes.begin(), bytes.begin() + 4U, "DSD ")) {
        std::size_t cursor = 28U;
        std::uint32_t channels = 0;
        std::uint32_t sampleRate = 0;
        std::uint64_t sampleCount = 0;
        std::size_t dataAt = 0;
        std::uint64_t dataBytes = 0;
        while (cursor + 12U <= bytes.size()) {
            const auto type = readBig32(bytes, cursor);
            const auto chunkBytes = readLittle64(bytes, cursor + 4U);
            if (chunkBytes < 12U || chunkBytes > bytes.size() - cursor) break;
            if (type == fourCc('f','m','t',' ') && chunkBytes >= 52U) {
                channels = readLittle32(bytes, cursor + 24U);
                sampleRate = readLittle32(bytes, cursor + 28U);
                sampleCount = readLittle64(bytes, cursor + 36U);
            } else if (type == fourCc('d','a','t','a')) {
                dataAt = cursor + 12U;
                dataBytes = chunkBytes - 12U;
            }
            cursor += static_cast<std::size_t>(chunkBytes);
        }
        if (channels > 0U && sampleRate > 0U && sampleCount > 0U &&
            dataBytes > 0U && dataBytes <= std::numeric_limits<std::uint32_t>::max()) {
            result.sampleRate = sampleRate;
            result.durationSeconds = static_cast<double>(sampleCount) / sampleRate;
            result.packets.push_back({static_cast<std::uint64_t>(dataAt),
                static_cast<std::uint32_t>(dataBytes), 0.0, result.durationSeconds});
            return result;
        }
        result.error = "DSF timing or audio data is unavailable";
        return result;
    }

    if (bytes.size() >= 16U &&
        std::equal(bytes.begin(), bytes.begin() + 4U, "FRM8") &&
        std::equal(bytes.begin() + 12U, bytes.begin() + 16U, "DSD ")) {
        std::uint32_t channels = 0;
        std::uint32_t sampleRate = 0;
        std::size_t dataAt = 0;
        std::uint64_t dataBytes = 0;
        for (std::size_t at = 16U; at + 12U <= bytes.size();) {
            const auto type = readBig32(bytes, at);
            const auto chunkBytes = readBig64(bytes, at + 4U);
            const auto payload = at + 12U;
            if (chunkBytes > bytes.size() - payload) break;
            if (type == fourCc('F','S',' ',' ') && chunkBytes >= 4U) {
                sampleRate = readBig32(bytes, payload);
            } else if (type == fourCc('C','H','N','L') && chunkBytes >= 2U) {
                channels = readBig16(bytes, payload);
            } else if (type == fourCc('P','R','O','P') && chunkBytes >= 4U &&
                readBig32(bytes, payload) == fourCc('S','N','D',' ')) {
                std::size_t propertyAt = payload + 4U;
                const auto propertyEnd = payload + static_cast<std::size_t>(chunkBytes);
                while (propertyAt + 12U <= propertyEnd) {
                    const auto propertyType = readBig32(bytes, propertyAt);
                    const auto propertyBytes = readBig64(bytes, propertyAt + 4U);
                    const auto propertyPayload = propertyAt + 12U;
                    if (propertyBytes > propertyEnd - propertyPayload) break;
                    if (propertyType == fourCc('F','S',' ',' ') &&
                        propertyBytes >= 4U) {
                        sampleRate = readBig32(bytes, propertyPayload);
                    } else if (propertyType == fourCc('C','H','N','L') &&
                        propertyBytes >= 2U) {
                        channels = readBig16(bytes, propertyPayload);
                    }
                    propertyAt = propertyPayload +
                        static_cast<std::size_t>(propertyBytes) +
                        static_cast<std::size_t>(propertyBytes & 1U);
                }
            } else if (type == fourCc('D','S','D',' ')) {
                dataAt = payload;
                dataBytes = chunkBytes;
            }
            at = payload + static_cast<std::size_t>(chunkBytes) + (chunkBytes & 1U);
        }
        if (channels > 0U && sampleRate > 0U && dataBytes > 0U &&
            dataBytes <= std::numeric_limits<std::uint32_t>::max()) {
            result.sampleRate = sampleRate;
            result.durationSeconds = static_cast<double>(dataBytes) * 8.0 /
                (sampleRate * static_cast<double>(channels));
            result.packets.push_back({static_cast<std::uint64_t>(dataAt),
                static_cast<std::uint32_t>(dataBytes), 0.0, result.durationSeconds});
            return result;
        }
        result.error = "DSDIFF timing or audio data is unavailable";
        return result;
    }
    result.error = "DSF/DSDIFF header was not found";
    return result;
}

PacketParseResult parseCafPackets(std::span<const std::uint8_t> bytes)
{
    PacketParseResult result;
    result.codec = PacketCodec::caf;
    if (bytes.size() < 8U ||
        !std::equal(bytes.begin(), bytes.begin() + 4U, "caff")) {
        result.error = "CAF header was not found";
        return result;
    }
    double sampleRate = 0.0;
    std::uint32_t bytesPerPacket = 0;
    std::uint32_t framesPerPacket = 0;
    std::size_t dataAt = 0;
    std::uint64_t dataBytes = 0;
    std::size_t cursor = 8U;
    while (cursor + 12U <= bytes.size()) {
        const auto type = readBig32(bytes, cursor);
        auto chunkBytes = readBig64(bytes, cursor + 4U);
        const auto payload = cursor + 12U;
        if (chunkBytes == std::numeric_limits<std::uint64_t>::max()) {
            chunkBytes = bytes.size() - payload;
        }
        if (chunkBytes > bytes.size() - payload) break;
        if (type == fourCc('d','e','s','c') && chunkBytes >= 32U) {
            const auto bits = readBig64(bytes, payload);
            std::memcpy(&sampleRate, &bits, sizeof(sampleRate));
            bytesPerPacket = readBig32(bytes, payload + 16U);
            framesPerPacket = readBig32(bytes, payload + 20U);
        } else if (type == fourCc('d','a','t','a') && chunkBytes >= 4U) {
            dataAt = payload + 4U;
            dataBytes = chunkBytes - 4U;
        }
        cursor = payload + static_cast<std::size_t>(chunkBytes);
    }
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 ||
        bytesPerPacket == 0U || framesPerPacket == 0U || dataBytes == 0U ||
        dataBytes > std::numeric_limits<std::uint32_t>::max()) {
        result.error = "CAF variable packet tables are not supported";
        return result;
    }
    const auto packetCount = dataBytes / bytesPerPacket;
    result.sampleRate = static_cast<std::uint32_t>(std::lround(sampleRate));
    result.durationSeconds = static_cast<double>(packetCount) * framesPerPacket /
        sampleRate;
    result.packets.push_back({static_cast<std::uint64_t>(dataAt),
        static_cast<std::uint32_t>(dataBytes), 0.0, result.durationSeconds});
    return result;
}

PacketCostResult parsePacketCost(
    const std::filesystem::path& path,
    std::uint32_t baseResolutionMilliseconds)
{
    PacketCostResult output;
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        output.error = "File could not be opened";
        return output;
    }
    const auto end = stream.tellg();
    if (end <= 0) {
        output.error = "File is empty";
        return output;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    stream.seekg(0, std::ios::beg);
    stream.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::size_t>(stream.gcount()) != bytes.size()) {
        output.error = "File read was incomplete";
        return output;
    }

    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    const auto startsWith = [&](std::string_view marker) {
        return bytes.size() >= marker.size() &&
            std::equal(marker.begin(), marker.end(), bytes.begin());
    };
    const auto id3At = skipId3v2(bytes);
    const auto markerAt = [&](std::size_t at, std::string_view marker) {
        return at <= bytes.size() && bytes.size() - at >= marker.size() &&
            std::equal(marker.begin(), marker.end(), bytes.begin() + at);
    };

    if (extension == L".flac" || markerAt(id3At, "fLaC")) {
        return makePacketCost(parseFlacPackets(bytes), baseResolutionMilliseconds);
    }
    if (extension == L".mp3") {
        return makePacketCost(parseMp3Packets(bytes), baseResolutionMilliseconds);
    }
    if (extension == L".aac" || extension == L".adts" ||
        (id3At + 2U <= bytes.size() && bytes[id3At] == 0xFFU &&
         (bytes[id3At + 1U] & 0xF6U) == 0xF0U)) {
        return makePacketCost(parseAacAdtsPackets(bytes),
            baseResolutionMilliseconds);
    }
    if (extension == L".m4a" || extension == L".m4b" ||
        extension == L".mp4" || extension == L".mov" ||
        extension == L".3gp" || extension == L".3g2" ||
        (bytes.size() >= 12U && readBig32(bytes, 4U) ==
            fourCc('f','t','y','p'))) {
        return makePacketCost(parseIsoBmffPackets(bytes),
            baseResolutionMilliseconds);
    }
    if (extension == L".ogg" || extension == L".oga" ||
        extension == L".opus" || markerAt(id3At, "OggS")) {
        return makePacketCost(parseOggPackets(bytes), baseResolutionMilliseconds);
    }
    if (extension == L".wav" || extension == L".wave" ||
        extension == L".rf64" || startsWith("RIFF") || startsWith("RF64")) {
        return makePacketCost(parseWavePackets(bytes), baseResolutionMilliseconds);
    }
    if (extension == L".aif" || extension == L".aiff" ||
        extension == L".aifc" || startsWith("FORM")) {
        return makePacketCost(parseAiffPackets(bytes), baseResolutionMilliseconds);
    }
    if (extension == L".wv" || startsWith("wvpk")) {
        return makePacketCost(parseWavPackPackets(bytes),
            baseResolutionMilliseconds);
    }
    if (extension == L".ac3" || extension == L".eac3" ||
        extension == L".ec3" ||
        (bytes.size() >= 2U && bytes[0] == 0x0BU && bytes[1] == 0x77U)) {
        return makePacketCost(parseDolbyPackets(bytes),
            baseResolutionMilliseconds);
    }
    if (extension == L".ape" || startsWith("MAC ")) {
        return makePacketCost(parseMonkeysAudioPackets(bytes),
            baseResolutionMilliseconds);
    }
    if (extension == L".tta" || startsWith("TTA1")) {
        return makePacketCost(parseTtaPackets(bytes), baseResolutionMilliseconds);
    }
    if (extension == L".dsf" || extension == L".dff" ||
        startsWith("DSD ") || startsWith("FRM8")) {
        return makePacketCost(parseDsdPackets(bytes), baseResolutionMilliseconds);
    }
    if (extension == L".caf" || startsWith("caff")) {
        return makePacketCost(parseCafPackets(bytes), baseResolutionMilliseconds);
    }
    output.error = "unsupported";
    return output;
}

PacketCostResult makePacketCost(
    const PacketParseResult& parsed,
    std::uint32_t baseResolutionMilliseconds)
{
    PacketCostResult output;
    output.codec = parsed.codec;
    output.durationSeconds = parsed.durationSeconds;
    output.error = parsed.error;
    if (!parsed.error.empty() || parsed.packets.empty() ||
        parsed.durationSeconds <= 0.0 || baseResolutionMilliseconds == 0) {
        return output;
    }

    output.supported = true;
    PacketCostLevel base;
    base.resolutionMilliseconds = baseResolutionMilliseconds;
    const double binSeconds = baseResolutionMilliseconds / 1000.0;
    const auto binCount = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::ceil(
               parsed.durationSeconds / binSeconds)));
    std::vector<double> binBytes(binCount, 0.0);

    for (const auto& packet : parsed.packets) {
        const double packetStart = packet.startSeconds;
        const double packetEnd = packetStart + packet.durationSeconds;
        if (packet.durationSeconds <= 0.0) continue;
        const auto first = std::min(
            binCount - 1,
            static_cast<std::size_t>(std::floor(packetStart / binSeconds)));
        const auto last = std::min(
            binCount - 1,
            static_cast<std::size_t>(std::floor(
                std::max(packetStart, packetEnd - 1e-9) / binSeconds)));
        for (std::size_t i = first; i <= last; ++i) {
            const double start = i * binSeconds;
            const double end = start + binSeconds;
            const double overlap = std::max(
                0.0, std::min(packetEnd, end) - std::max(packetStart, start));
            binBytes[i] += packet.bytes * (overlap / packet.durationSeconds);
        }
    }

    std::vector<double> values;
    values.reserve(binCount);
    for (std::size_t i = 0; i < binCount; ++i) {
        const double duration = std::min(
            binSeconds,
            std::max(0.0, parsed.durationSeconds - i * binSeconds));
        const double kbps = duration > 0.0
            ? binBytes[i] * 8.0 / duration / 1000.0
            : 0.0;
        values.push_back(kbps);
        base.bins.push_back({i * binSeconds, duration, kbps, 0.0F});
    }

    const auto p05 = percentile(values, 0.05);
    const auto p95 = percentile(values, 0.95);
    const double denominator = std::max(1.0, p95 - p05);
    for (auto& bin : base.bins) {
        bin.normalized = static_cast<float>(std::clamp(
            (bin.kilobitsPerSecond - p05) / denominator, 0.0, 1.0));
    }

    const double average = std::accumulate(binBytes.begin(), binBytes.end(), 0.0)
        * 8.0 / parsed.durationSeconds / 1000.0;
    output.stats = {
        *std::min_element(values.begin(), values.end()),
        p05,
        percentile(values, 0.5),
        average,
        percentile(values, 0.90),
        p95,
        *std::max_element(values.begin(), values.end()),
    };
    output.levels.push_back(base);
    output.levels.push_back(aggregateLevel(base, 500));
    output.levels.push_back(aggregateLevel(base, 2000));
    return output;
}

namespace {

constexpr std::uint32_t kPacketCostSupported = 1U;
constexpr std::size_t kMaxPacketCostLevels = 16U;
constexpr std::size_t kMaxPacketCostBins = 4U * 1024U * 1024U;
constexpr std::size_t kMaxPacketCostBytes = 128U * 1024U * 1024U;

#pragma pack(push, 1)
struct PacketCostBinaryHeaderV1 {
    std::uint32_t magic = kPacketCostBinaryMagic;
    std::uint16_t version = kPacketCostBinaryVersion;
    std::uint16_t codec = 0;
    std::uint32_t flags = 0;
    double durationSeconds = 0.0;
    std::array<double, 7> stats{};
    std::uint32_t errorBytes = 0;
    std::uint32_t levelCount = 0;
    std::uint32_t totalBinCount = 0;
};

struct PacketCostBinaryLevelV1 {
    std::uint32_t resolutionMilliseconds = 0;
    std::uint32_t binCount = 0;
};

struct PacketCostBinaryBinV1 {
    double startSeconds = 0.0;
    double durationSeconds = 0.0;
    double kilobitsPerSecond = 0.0;
    float normalized = 0.0F;
    std::uint32_t reserved = 0;
};
#pragma pack(pop)

static_assert(sizeof(PacketCostBinaryHeaderV1) == 88);
static_assert(sizeof(PacketCostBinaryLevelV1) == 8);
static_assert(sizeof(PacketCostBinaryBinV1) == 32);

bool finiteStats(const PacketCostStats& stats) noexcept
{
    return std::isfinite(stats.minimumKbps) && std::isfinite(stats.p05Kbps) &&
        std::isfinite(stats.medianKbps) && std::isfinite(stats.averageKbps) &&
        std::isfinite(stats.p90Kbps) && std::isfinite(stats.p95Kbps) &&
        std::isfinite(stats.peakKbps);
}

template<typename T>
void appendObject(std::vector<std::byte>& output, const T& value)
{
    const auto oldSize = output.size();
    output.resize(oldSize + sizeof(value));
    std::memcpy(output.data() + oldSize, &value, sizeof(value));
}

template<typename T>
bool readObject(std::span<const std::byte> input, std::size_t& cursor,
    T& value) noexcept
{
    if (cursor > input.size() || input.size() - cursor < sizeof(value)) {
        return false;
    }
    std::memcpy(&value, input.data() + cursor, sizeof(value));
    cursor += sizeof(value);
    return true;
}

} // namespace

bool serializePacketCost(const PacketCostResult& cost,
    std::vector<std::byte>& output, std::string& error)
{
    output.clear();
    error.clear();
    if (!std::isfinite(cost.durationSeconds) || cost.durationSeconds < 0.0 ||
        !finiteStats(cost.stats) || cost.levels.size() > kMaxPacketCostLevels ||
        cost.error.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = "packet-cost data is outside the binary format limits";
        return false;
    }

    std::size_t totalBins = 0;
    std::size_t required = sizeof(PacketCostBinaryHeaderV1) + cost.error.size();
    if (required > kMaxPacketCostBytes) {
        error = "packet-cost payload exceeds the binary size limit";
        return false;
    }
    for (const auto& level : cost.levels) {
        if (level.resolutionMilliseconds == 0 ||
            level.bins.size() > kMaxPacketCostBins - totalBins) {
            error = "packet-cost level is outside the binary format limits";
            return false;
        }
        totalBins += level.bins.size();
        if (required > kMaxPacketCostBytes - sizeof(PacketCostBinaryLevelV1) ||
            level.bins.size() >
            (kMaxPacketCostBytes - required - sizeof(PacketCostBinaryLevelV1)) /
                sizeof(PacketCostBinaryBinV1)) {
            error = "packet-cost payload exceeds the binary size limit";
            return false;
        }
        required += sizeof(PacketCostBinaryLevelV1) +
            level.bins.size() * sizeof(PacketCostBinaryBinV1);
        for (const auto& bin : level.bins) {
            if (!std::isfinite(bin.startSeconds) ||
                !std::isfinite(bin.durationSeconds) ||
                !std::isfinite(bin.kilobitsPerSecond) ||
                !std::isfinite(bin.normalized) || bin.startSeconds < 0.0 ||
                bin.durationSeconds < 0.0 || bin.kilobitsPerSecond < 0.0) {
                error = "packet-cost bin contains an invalid value";
                return false;
            }
        }
    }
    if (required > kMaxPacketCostBytes ||
        totalBins > std::numeric_limits<std::uint32_t>::max()) {
        error = "packet-cost payload exceeds the binary size limit";
        return false;
    }
    if (cost.supported && (cost.codec == PacketCodec::unsupported ||
                           cost.durationSeconds <= 0.0 || cost.levels.empty())) {
        error = "supported packet-cost data is incomplete";
        return false;
    }

    PacketCostBinaryHeaderV1 header;
    header.codec = static_cast<std::uint16_t>(cost.codec);
    header.flags = cost.supported ? kPacketCostSupported : 0U;
    header.durationSeconds = cost.durationSeconds;
    header.stats = {cost.stats.minimumKbps, cost.stats.p05Kbps,
        cost.stats.medianKbps, cost.stats.averageKbps, cost.stats.p90Kbps,
        cost.stats.p95Kbps, cost.stats.peakKbps};
    header.errorBytes = static_cast<std::uint32_t>(cost.error.size());
    header.levelCount = static_cast<std::uint32_t>(cost.levels.size());
    header.totalBinCount = static_cast<std::uint32_t>(totalBins);

    output.reserve(required);
    appendObject(output, header);
    if (!cost.error.empty()) {
        output.insert(output.end(),
            reinterpret_cast<const std::byte*>(cost.error.data()),
            reinterpret_cast<const std::byte*>(
                cost.error.data() + cost.error.size()));
    }
    for (const auto& level : cost.levels) {
        const PacketCostBinaryLevelV1 levelHeader{
            level.resolutionMilliseconds,
            static_cast<std::uint32_t>(level.bins.size())};
        appendObject(output, levelHeader);
        for (const auto& bin : level.bins) {
            const PacketCostBinaryBinV1 binaryBin{bin.startSeconds,
                bin.durationSeconds, bin.kilobitsPerSecond, bin.normalized, 0};
            appendObject(output, binaryBin);
        }
    }
    return true;
}

bool deserializePacketCost(std::span<const std::byte> input,
    PacketCostResult& output, std::string& error)
{
    output = {};
    error.clear();
    if (input.size() > kMaxPacketCostBytes) {
        error = "packet-cost payload exceeds the binary size limit";
        return false;
    }
    std::size_t cursor = 0;
    PacketCostBinaryHeaderV1 header;
    if (!readObject(input, cursor, header) ||
        header.magic != kPacketCostBinaryMagic ||
        header.version != kPacketCostBinaryVersion ||
        header.codec > static_cast<std::uint16_t>(PacketCodec::caf) ||
        (header.flags & ~kPacketCostSupported) != 0 ||
        header.levelCount > kMaxPacketCostLevels ||
        header.totalBinCount > kMaxPacketCostBins ||
        !std::isfinite(header.durationSeconds) || header.durationSeconds < 0.0 ||
        cursor > input.size() || input.size() - cursor < header.errorBytes) {
        error = "packet-cost binary header is invalid";
        return false;
    }

    output.codec = static_cast<PacketCodec>(header.codec);
    output.supported = (header.flags & kPacketCostSupported) != 0;
    output.durationSeconds = header.durationSeconds;
    output.stats = {header.stats[0], header.stats[1], header.stats[2],
        header.stats[3], header.stats[4], header.stats[5], header.stats[6]};
    if (!finiteStats(output.stats)) {
        error = "packet-cost binary statistics are invalid";
        return false;
    }
    output.error.assign(
        reinterpret_cast<const char*>(input.data() + cursor),
        header.errorBytes);
    cursor += header.errorBytes;

    std::size_t totalBins = 0;
    output.levels.reserve(header.levelCount);
    for (std::uint32_t levelIndex = 0; levelIndex < header.levelCount;
         ++levelIndex) {
        PacketCostBinaryLevelV1 levelHeader;
        if (!readObject(input, cursor, levelHeader) ||
            levelHeader.resolutionMilliseconds == 0 ||
            levelHeader.binCount > kMaxPacketCostBins - totalBins) {
            error = "packet-cost binary level is invalid";
            return false;
        }
        PacketCostLevel level;
        level.resolutionMilliseconds = levelHeader.resolutionMilliseconds;
        level.bins.reserve(levelHeader.binCount);
        for (std::uint32_t binIndex = 0; binIndex < levelHeader.binCount;
             ++binIndex) {
            PacketCostBinaryBinV1 binaryBin;
            if (!readObject(input, cursor, binaryBin) ||
                binaryBin.reserved != 0 ||
                !std::isfinite(binaryBin.startSeconds) ||
                !std::isfinite(binaryBin.durationSeconds) ||
                !std::isfinite(binaryBin.kilobitsPerSecond) ||
                !std::isfinite(binaryBin.normalized) ||
                binaryBin.startSeconds < 0.0 ||
                binaryBin.durationSeconds < 0.0 ||
                binaryBin.kilobitsPerSecond < 0.0) {
                error = "packet-cost binary bin is invalid";
                return false;
            }
            level.bins.push_back({binaryBin.startSeconds,
                binaryBin.durationSeconds, binaryBin.kilobitsPerSecond,
                binaryBin.normalized});
        }
        totalBins += levelHeader.binCount;
        output.levels.push_back(std::move(level));
    }
    if (cursor != input.size() || totalBins != header.totalBinCount ||
        (output.supported && (output.codec == PacketCodec::unsupported ||
                              output.durationSeconds <= 0.0 ||
                              output.levels.empty()))) {
        output = {};
        error = "packet-cost binary payload is inconsistent";
        return false;
    }
    return true;
}

} // namespace nekorox
