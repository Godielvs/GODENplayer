#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nekorox {

enum class PacketCodec {
    unsupported,
    flac,
    mp3,
    aacAdts,
    isoBmff,
    oggVorbis,
    oggOpus,
    wave,
    aiff,
    wavPack,
    ac3,
    eac3,
    monkeysAudio,
    tta,
    dsd,
    caf,
};

struct EncodedPacket {
    // Ogg entries represent a timed group of audio-bearing pages. Their byte
    // cost excludes metadata; offset is an anchor, not a contiguous byte range.
    std::uint64_t offset = 0;
    std::uint32_t bytes = 0;
    double startSeconds = 0.0;
    double durationSeconds = 0.0;
};

struct PacketParseResult {
    PacketCodec codec = PacketCodec::unsupported;
    std::uint32_t sampleRate = 0;
    double durationSeconds = 0.0;
    bool variableBitrateHeader = false;
    std::string error;
    std::vector<EncodedPacket> packets;
};

struct PacketCostBin {
    double startSeconds = 0.0;
    double durationSeconds = 0.0;
    double kilobitsPerSecond = 0.0;
    float normalized = 0.0F;
};

struct PacketCostLevel {
    std::uint32_t resolutionMilliseconds = 100;
    std::vector<PacketCostBin> bins;
};

struct PacketCostStats {
    double minimumKbps = 0.0;
    double p05Kbps = 0.0;
    double medianKbps = 0.0;
    double averageKbps = 0.0;
    double p90Kbps = 0.0;
    double p95Kbps = 0.0;
    double peakKbps = 0.0;
};

struct PacketCostResult {
    PacketCodec codec = PacketCodec::unsupported;
    bool supported = false;
    std::string error;
    double durationSeconds = 0.0;
    PacketCostStats stats;
    std::vector<PacketCostLevel> levels;
};

inline constexpr std::uint32_t kPacketCostBinaryMagic = 0x43504B4E;
inline constexpr std::uint16_t kPacketCostBinaryVersion = 1;
inline constexpr std::string_view kPacketCostAnalyzerVersion =
    // Older cache writes did not retain the analyzed file's identity and may
    // have stored a valid old payload under a newer revision. Rebuild lazily.
    "packet-cost-2.2.0";

// Packet timing belongs to the whole file. Subsong/chained or malformed timing
// must fall back to the decoder's rate instead of presenting unrelated bins.
inline bool packetCostDurationMatches(double measured, double playing) noexcept
{
    return std::isfinite(measured) && std::isfinite(playing) &&
        measured > 0.0 && playing > 0.0 &&
        std::abs(measured - playing) <= std::max(0.5, playing * 0.01);
}

PacketParseResult parseFlacPackets(std::span<const std::uint8_t> bytes);
PacketParseResult parseMp3Packets(std::span<const std::uint8_t> bytes);
PacketParseResult parseAacAdtsPackets(std::span<const std::uint8_t> bytes);
PacketParseResult parseIsoBmffPackets(std::span<const std::uint8_t> bytes);
PacketParseResult parseOggPackets(std::span<const std::uint8_t> bytes);
PacketParseResult parseWavePackets(std::span<const std::uint8_t> bytes);
PacketParseResult parseAiffPackets(std::span<const std::uint8_t> bytes);
PacketParseResult parseWavPackPackets(std::span<const std::uint8_t> bytes);
PacketParseResult parseDolbyPackets(std::span<const std::uint8_t> bytes);
PacketParseResult parseMonkeysAudioPackets(std::span<const std::uint8_t> bytes);
PacketParseResult parseTtaPackets(std::span<const std::uint8_t> bytes);
PacketParseResult parseDsdPackets(std::span<const std::uint8_t> bytes);
PacketParseResult parseCafPackets(std::span<const std::uint8_t> bytes);
PacketCostResult parsePacketCost(
    const std::filesystem::path& path,
    std::uint32_t baseResolutionMilliseconds = 100);
PacketCostResult makePacketCost(
    const PacketParseResult& parsed,
    std::uint32_t baseResolutionMilliseconds = 100);
bool serializePacketCost(
    const PacketCostResult& cost,
    std::vector<std::byte>& output,
    std::string& error);
bool deserializePacketCost(
    std::span<const std::byte> input,
    PacketCostResult& output,
    std::string& error);

} // namespace nekorox
