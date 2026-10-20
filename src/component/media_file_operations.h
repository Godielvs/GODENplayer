#pragma once

#include "physical_file_edit_plan.h"
#include "nekorox/database.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace nekorox::component {

enum class MediaOrganizationMode : unsigned char { inPlace, copy, move };
enum class MediaCollisionPolicy : unsigned char { skip, rename, replace };
enum class MediaPlanDisposition : unsigned char { ready, skipped, rejected };

struct MediaPlanRequest {
    std::filesystem::path source;
    std::filesystem::path relativeDestination;
};

struct MediaNamingFields {
    std::wstring albumArtist;
    std::wstring artist;
    std::wstring album;
    std::wstring year;
    std::wstring disc;
    std::wstring track;
    std::wstring title;
    unsigned discCount = 1;
};

struct MediaPlanItem {
    std::filesystem::path source;
    std::filesystem::path destination;
    MediaPlanDisposition disposition = MediaPlanDisposition::rejected;
    bool replaceExisting = false;
    bool caseOnlyRename = false;
    std::wstring detail;
};

struct MediaPlan {
    std::filesystem::path approvedRoot;
    MediaOrganizationMode mode = MediaOrganizationMode::inPlace;
    MediaCollisionPolicy collision = MediaCollisionPolicy::skip;
    std::vector<MediaPlanItem> items;
};

struct MediaRevisionBinding {
    std::string previousRevisionTrackId;
    PersistedTrack current;
};

enum class MediaCopyOutcome : unsigned char {
    staging, verified, backedUp, copied, moved, duplicateRecoverable,
    skipped, rejected, conflict, failed, cancelled
};

struct MediaCopyResult {
    std::filesystem::path source;
    std::filesystem::path destination;
    std::filesystem::path staging;
    std::filesystem::path backup;
    std::filesystem::path sourceBackup;
    MediaCopyOutcome outcome = MediaCopyOutcome::rejected;
    std::uintmax_t bytes = 0;
    std::string sourceSha256;
    std::string sha256;
    std::vector<std::filesystem::path> sidecarPaths;
    std::vector<std::filesystem::path> sidecarBackups;
    std::size_t sidecarsExpected = 0;
    std::size_t sidecarsCommitted = 0;
    std::wstring detail;
    // Cache publication is separate from the verified physical outcome. A
    // refresh failure must never turn a saved file into a failed write/Undo.
    bool metadataRefreshFailed = false;
    bool identityRefreshFailed = false;
    // Captured on the file worker. Retained with Undo so a later restoration
    // remains attached to the original entity even if its timestamp changes.
    std::vector<MediaRevisionBinding> revisionBindings;
    std::filesystem::path bindingJournalPath;
    std::string bindingJournalSha256;
    // Independent of success/failure: a rename may have happened even if its
    // final readback failed. Such a path stays protected until DB + receipt.
    bool physicalCommitNeedsBinding = false;
};

using MediaSidecarWrite = SidecarDelta;

struct MediaCopyExecutionOptions {
    // Production callers must provide a path outside scanned library roots.
    // Empty is accepted only for isolated pure tests/tools.
    std::filesystem::path journalPath;
    // Mandatory for Replace. The directory must be outside approvedRoot.
    std::filesystem::path backupRoot;
    // Optional staging edit/readback hook. The component service uses this to
    // write tags/artwork through foobar2000 SDK before the final rename.
    std::function<bool(const std::filesystem::path&, std::wstring&)> mutateStaging;
    std::function<bool(const std::filesystem::path& source,
        const std::filesystem::path& staging, std::wstring&)>
        mutateStagingForSource;
    std::function<bool(const std::filesystem::path&, std::wstring&)> verifyStaging;
    // Capture every physical subsong before mutation, for durable identity
    // recovery. Production supplies this for its one-file InPlace item plans.
    std::function<std::vector<MediaRevisionBinding>(const std::filesystem::path&)>
        captureSourceRevisions;
    // QA-only deterministic fault/volume injection for destructive-boundary tests.
    std::function<bool(std::wstring_view)> allowStep;
    std::optional<bool> sameVolumeOverride;
    // Explicitly separate from embedded artwork. Sidecars are committed next
    // to the verified destination and participate in the same durable journal.
    std::vector<MediaSidecarWrite> sidecars;
};

struct MediaRestoreExecutionOptions {
    // A fresh operation name; the engine adds an item index for each Undo.
    std::filesystem::path journalPath;
    std::function<bool(std::wstring_view)> allowStep;
    // Checked before reading a target and again immediately before replacing
    // it. The service uses this to protect the current playback resource.
    std::function<bool(const std::filesystem::path&)> allowRestore;
};

// Builds a fail-closed, deterministic batch plan. It never creates directories
// or files and never follows a reparse point outside the approved root.
[[nodiscard]] MediaPlan buildMediaPlan(
    const std::filesystem::path& approvedRoot,
    const std::vector<MediaPlanRequest>& requests,
    MediaOrganizationMode mode,
    MediaCollisionPolicy collision, std::stop_token stop = {});

[[nodiscard]] std::filesystem::path buildDefaultRelativeDestination(
    const std::filesystem::path& source, const MediaNamingFields& fields);

// Executes only ready Copy items. Replace requires an out-of-library recovery
// root. Each file is copied to same-folder staging, fully hashed, then renamed
// only after the durable intent and collision guards succeed.
[[nodiscard]] std::vector<MediaCopyResult> executeCopyPlan(
    const MediaPlan& plan, std::stop_token stop = {},
    const MediaCopyExecutionOptions& options = {});

// Executes validated Copy or explicit Move plans. Move never removes a source
// until the destination is fully hashed and a durable journal state exists.
[[nodiscard]] std::vector<MediaCopyResult> executeMediaPlan(
    const MediaPlan& plan, std::stop_token stop = {},
    const MediaCopyExecutionOptions& options = {});

[[nodiscard]] std::vector<MediaCopyResult> restoreInPlaceBackups(
    const std::vector<MediaCopyResult>& completed, std::stop_token stop = {},
    const MediaRestoreExecutionOptions& options = {});

} // namespace nekorox::component
