#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace nekorox::component {

// Variants share the source content hash, so dimensions and the rounded DPI
// are also part of bitmap identity. Own the values, not the ArtPixels object.
struct ArtworkBitmapKey {
    std::uint64_t contentHash;
    std::uint32_t width;
    std::uint32_t height;
    unsigned dpi;

    bool operator==(const ArtworkBitmapKey& other) const noexcept
    {
        return contentHash == other.contentHash && width == other.width &&
            height == other.height && dpi == other.dpi;
    }
};

struct ArtworkBitmapKeyHash {
    std::size_t operator()(const ArtworkBitmapKey& key) const noexcept
    {
        // Combine fields explicitly: struct padding is not part of identity.
        auto hash = key.contentHash;
        for (const auto value : {key.width, key.height, key.dpi}) {
            hash ^= static_cast<std::uint64_t>(value) +
                0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
        }
        return static_cast<std::size_t>(hash);
    }
};

} // namespace nekorox::component
