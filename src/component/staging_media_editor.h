#pragma once

#include "physical_file_edit_plan.h"

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

namespace nekorox::component {

struct StagingMediaEdits {
    std::vector<nekorox::TagMutation> tags;
    std::vector<ArtworkMutation> artwork;
};

struct ValidatedArtwork {
    std::vector<std::uint8_t> originalBytes;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::wstring format;
    std::wstring mime;
    std::uint32_t previewWidth = 0;
    std::uint32_t previewHeight = 0;
    std::vector<std::uint8_t> previewBgra;
};

// Decodes enough of the image through WIC to reject corrupt/bomb inputs while
// retaining the exact original bytes that will be embedded.
[[nodiscard]] bool loadAndValidateArtwork(const std::filesystem::path& path,
    ValidatedArtwork& output, std::wstring& error,
    std::stop_token stop = {});

// Low-level writer used only on a private staging copy. It takes the SDK write
// lock, preserves every unselected field/role and performs a full readback.
[[nodiscard]] bool applyAndVerifyStagingEdits(const std::filesystem::path& path,
    const StagingMediaEdits& edits, std::wstring& error);

// Exact physical-file writer used by TAG Apply. Metadata deltas are scoped by
// subsong while embedded artwork is explicitly shared by the whole file.
[[nodiscard]] bool applyAndVerifyStagingEdits(const std::filesystem::path& path,
    const PhysicalFileEditPlan& plan, std::wstring& error);

} // namespace nekorox::component
