#include "nekorox/compression.h"

#include "zstd.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace nekorox {
namespace {

enum class BlobCodec : std::uint16_t {
    zstd = 1,
};

#pragma pack(push, 1)
struct BlobHeaderV1 {
    std::uint32_t magic = kCompressedBlobMagic;
    std::uint16_t version = kCompressedBlobVersion;
    BlobCodec codec = BlobCodec::zstd;
    std::uint64_t uncompressedBytes = 0;
    std::uint64_t checksum = 0;
    std::uint64_t payloadBytes = 0;
};
#pragma pack(pop)

static_assert(sizeof(BlobHeaderV1) == 32);

std::uint64_t contentChecksum(std::span<const std::byte> input) noexcept
{
    std::uint64_t value = 14695981039346656037ULL;
    for (const auto byte : input) {
        value ^= static_cast<std::uint8_t>(byte);
        value *= 1099511628211ULL;
    }
    return value;
}

std::string zstdError(const char* operation, std::size_t code)
{
    std::string error(operation);
    error += ": ";
    error += ZSTD_getErrorName(code);
    return error;
}

} // namespace

bool compressZstdBlob(std::span<const std::byte> input,
    std::vector<std::byte>& output, std::string& error, int level)
{
    output.clear();
    error.clear();
    if (input.size() > kMaximumCompressedBlobInput) {
        error = "compression input exceeds the NekoRoX blob limit";
        return false;
    }
    const auto bound = ZSTD_compressBound(input.size());
    if (ZSTD_isError(bound) ||
        bound > std::numeric_limits<std::size_t>::max() - sizeof(BlobHeaderV1)) {
        error = "compression bound is invalid";
        return false;
    }
    output.resize(sizeof(BlobHeaderV1) + bound);
    auto* payload = output.data() + sizeof(BlobHeaderV1);
    const auto written = ZSTD_compress(payload, bound,
        input.empty() ? nullptr : input.data(), input.size(), level);
    if (ZSTD_isError(written)) {
        error = zstdError("zstd compression failed", written);
        output.clear();
        return false;
    }
    BlobHeaderV1 header;
    header.uncompressedBytes = input.size();
    header.checksum = contentChecksum(input);
    header.payloadBytes = written;
    std::memcpy(output.data(), &header, sizeof(header));
    output.resize(sizeof(header) + written);
    return true;
}

bool decompressZstdBlob(std::span<const std::byte> input,
    std::vector<std::byte>& output, std::string& error,
    std::size_t maximumOutputBytes)
{
    output.clear();
    error.clear();
    if (input.size() < sizeof(BlobHeaderV1)) {
        error = "compressed blob header is truncated";
        return false;
    }
    BlobHeaderV1 header;
    std::memcpy(&header, input.data(), sizeof(header));
    if (header.magic != kCompressedBlobMagic ||
        header.version != kCompressedBlobVersion ||
        header.codec != BlobCodec::zstd) {
        error = "compressed blob header is unsupported";
        return false;
    }
    if (header.uncompressedBytes > maximumOutputBytes ||
        header.uncompressedBytes > std::numeric_limits<std::size_t>::max()) {
        error = "compressed blob expands beyond the configured limit";
        return false;
    }
    if (header.payloadBytes != input.size() - sizeof(header)) {
        error = "compressed blob payload length does not match its header";
        return false;
    }
    output.resize(static_cast<std::size_t>(header.uncompressedBytes));
    const auto* payload = input.data() + sizeof(header);
    const auto decoded = ZSTD_decompress(
        output.empty() ? nullptr : output.data(), output.size(),
        payload, static_cast<std::size_t>(header.payloadBytes));
    if (ZSTD_isError(decoded)) {
        error = zstdError("zstd decompression failed", decoded);
        output.clear();
        return false;
    }
    if (decoded != output.size() || contentChecksum(output) != header.checksum) {
        error = "compressed blob integrity check failed";
        output.clear();
        return false;
    }
    return true;
}

} // namespace nekorox
