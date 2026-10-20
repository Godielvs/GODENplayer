#pragma once

#include "media_file_operations.h"

namespace nekorox::component {

// All disk work runs on the file worker. The frozen revisions, never paths to
// identify later, cross the asynchronous boundary to the database worker.
struct MediaBindingRecoveryItem {
    std::filesystem::path journalPath;
    std::string journalSha256;
    std::vector<MediaRevisionBinding> revisions;
};

struct MediaBindingRecoveryScan {
    std::vector<MediaBindingRecoveryItem> pending;
    std::vector<std::filesystem::path> blockedPaths;
    bool limited = false;
};

[[nodiscard]] bool mediaFileSha256(const std::filesystem::path& path,
    std::string& output, std::stop_token stop);
[[nodiscard]] bool freezeMediaBindingRevisions(const std::filesystem::path& path,
    std::vector<MediaRevisionBinding>& revisions, std::stop_token stop,
    const std::string& expectedSha256 = {});
[[nodiscard]] MediaBindingRecoveryScan scanMediaBindingRecovery(
    const std::filesystem::path& directory, std::stop_token stop);
// Caller must first receive a successful DB commit. Confirmation is tied to
// the exact journal bytes, so a stale receipt cannot hide a different edit.
[[nodiscard]] bool acknowledgeMediaBindingRecovery(
    const MediaBindingRecoveryItem& item, std::stop_token stop);

} // namespace nekorox::component
