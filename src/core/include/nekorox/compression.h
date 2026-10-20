#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace nekorox {

inline constexpr std::uint32_t kCompressedBlobMagic = 0x315A524EU; // NRZ1
inline constexpr std::uint16_t kCompressedBlobVersion = 1;
inline constexpr std::size_t kMaximumCompressedBlobInput = 128U * 1024U * 1024U;

bool compressZstdBlob(
    std::span<const std::byte> input,
    std::vector<std::byte>& output,
    std::string& error,
    int level = 3);

bool decompressZstdBlob(
    std::span<const std::byte> input,
    std::vector<std::byte>& output,
    std::string& error,
    std::size_t maximumOutputBytes = kMaximumCompressedBlobInput);

} // namespace nekorox
