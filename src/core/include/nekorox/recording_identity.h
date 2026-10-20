#pragma once

#include <cstdint>
#include <string>

namespace nekorox {

// Persistent media identity is deliberately independent from display metadata.
// `mediaRevision` changes only when the underlying media/content revision does.
struct RecordingKey {
    std::string entityId;
    std::uint32_t subsong = 0;
    std::string mediaRevision;

    [[nodiscard]] bool operator==(const RecordingKey&) const = default;
};

struct MetadataRevision {
    std::uint64_t value = 0;

    [[nodiscard]] bool operator==(const MetadataRevision&) const = default;
};

struct RequestToken {
    RecordingKey recording;
    std::uint64_t generation = 0;

    [[nodiscard]] bool operator==(const RequestToken&) const = default;
};

struct SourceVersion {
    std::string provider;
    std::string recordId;
    std::string revision;

    [[nodiscard]] bool operator==(const SourceVersion&) const = default;
};

} // namespace nekorox
