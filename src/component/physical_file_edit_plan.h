#pragma once

#include <nekorox/tag_mutation.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nekorox::component {

using NativePath = std::filesystem::path;

enum class ArtworkRole : unsigned char { front, back, artist, other };
enum class ArtworkMutationMode : unsigned char { keep, set, remove, setSidecar };

using ValidatedBytes = std::vector<std::uint8_t>;

struct ArtworkMutation {
    ArtworkRole role = ArtworkRole::front;
    ArtworkMutationMode mode = ArtworkMutationMode::keep;
    std::shared_ptr<const ValidatedBytes> originalBytes;
    std::wstring sourceExtension;

    [[nodiscard]] bool operator==(const ArtworkMutation& other) const
    {
        const auto sameContent = originalBytes == other.originalBytes ||
            (originalBytes && other.originalBytes &&
                *originalBytes == *other.originalBytes);
        return role == other.role && mode == other.mode &&
            sourceExtension == other.sourceExtension && sameContent;
    }
};

struct SidecarDelta {
    // A filename stem only (for example folder/back/artist/disc), never a path.
    std::wstring stem;
    std::wstring extension;
    std::shared_ptr<const ValidatedBytes> bytes;

    [[nodiscard]] bool operator==(const SidecarDelta& other) const
    {
        const auto sameContent = bytes == other.bytes ||
            (bytes && other.bytes && *bytes == *other.bytes);
        return stem == other.stem && extension == other.extension &&
            sameContent;
    }
};

// A strong precondition for a physical write. SHA-256 is authoritative; size
// and timestamp make inexpensive conflict screening possible before hashing.
struct FileRevision {
    std::uintmax_t size = 0;
    std::filesystem::file_time_type lastWrite{};
    std::string sha256;

    [[nodiscard]] bool operator==(const FileRevision&) const = default;
};

using TagDelta = std::vector<nekorox::TagMutation>;
using ArtworkDelta = std::vector<ArtworkMutation>;

// One plan per physical file. Per-subsong metadata is never collapsed into a
// last-edit-wins map; embedded artwork remains explicitly file-scoped.
struct PhysicalFileEditPlan {
    NativePath path;
    FileRevision expectedBefore;
    std::map<std::uint32_t, TagDelta> bySubsong;
    ArtworkDelta fileArtwork;
    std::vector<SidecarDelta> sidecars;
    std::uint64_t operationSeq = 0;
};

} // namespace nekorox::component
