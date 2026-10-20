#pragma once

#include "track_identity.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>

namespace nekorox {

// All reads below belong on the analysis reader, outside the UI state lock.
struct AnalysisSourceRevision {
    std::uint64_t bytes = 0;
    std::uint64_t modifiedTicks = 0;
    bool operator==(const AnalysisSourceRevision&) const = default;
};

inline std::optional<AnalysisSourceRevision> analysisSourceRevision(
    const std::filesystem::path& path) noexcept
{
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    if (error) return {};
    const auto modified = std::filesystem::last_write_time(path, error);
    if (error) return {};
    return AnalysisSourceRevision{bytes,
        static_cast<std::uint64_t>(modified.time_since_epoch().count())};
}

inline std::string analysisSourceIdentity(const std::filesystem::path& path) noexcept
{
    try {
        const auto before = analysisSourceRevision(path);
        if (!before) return {};
        auto identity = makeTrackId({path, 0, before->bytes, before->modifiedTicks});
        if (analysisSourceRevision(path) != before) return {};
        return std::move(identity.value);
    } catch (...) { return {}; }
}

} // namespace nekorox
