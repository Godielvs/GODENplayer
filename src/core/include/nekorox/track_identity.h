#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace nekorox {

struct TrackIdentityInput {
    std::filesystem::path path;
    std::uint32_t subsong = 0;
    std::uint64_t sizeBytes = 0;
    std::uint64_t modifiedTicks = 0;
};

struct NekoTrackId {
    std::string value;
    std::string normalizedPathUtf8;
    std::string partialFingerprint;
};

std::wstring normalizeWindowsPath(const std::filesystem::path& path);
NekoTrackId makeTrackId(const TrackIdentityInput& input);

} // namespace nekorox

