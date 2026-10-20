#include "../src/component/media_file_operations.h"

#include <fstream>
#include <stdexcept>

void runWorkpackImportTests()
{
    namespace fs = std::filesystem;
    using namespace nekorox::component;
    const auto root = fs::temp_directory_path() / L"goden-wp11-plan-fixture";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root / L"source", error);
    fs::create_directories(root / L"library", error);
    const auto sourceA = root / L"source" / L"a.flac";
    const auto sourceB = root / L"source" / L"b.flac";
    { std::ofstream(sourceA, std::ios::binary) << "a"; }
    { std::ofstream(sourceB, std::ios::binary) << "b"; }
    const auto named = buildDefaultRelativeDestination(sourceA,
        {L"Album/Artist", L"", L"CON", L"2026", L"2", L"03",
            L"Title: Live?", 2});
    if (named != fs::path(L"Album_Artist") / L"_CON (2026)" /
            L"2-03 - Title_ Live_.flac") {
        throw std::runtime_error("WP11 default destination scheme/sanitization failed");
    }

    const std::vector<MediaPlanRequest> duplicateNames{
        {sourceA, L"Artist/Album/song.flac"},
        {sourceB, L"Artist/Album/song.flac"},
    };
    const auto rename = buildMediaPlan(root / L"library", duplicateNames,
        MediaOrganizationMode::copy, MediaCollisionPolicy::rename);
    if (rename.items.size() != 2 ||
        rename.items[0].disposition != MediaPlanDisposition::ready ||
        rename.items[1].disposition != MediaPlanDisposition::ready ||
        rename.items[0].destination == rename.items[1].destination ||
        rename.items[1].destination.filename() != L"song (2).flac") {
        throw std::runtime_error("WP11 batch collision reservation failed");
    }
    const auto escape = buildMediaPlan(root / L"library",
        {{sourceA, L"../outside.flac"}}, MediaOrganizationMode::copy,
        MediaCollisionPolicy::skip);
    if (escape.items[0].disposition != MediaPlanDisposition::rejected) {
        throw std::runtime_error("WP11 allowed destination root escape");
    }
    const auto reserved = buildMediaPlan(root / L"library",
        {{sourceA, L"CON.flac"}}, MediaOrganizationMode::copy,
        MediaCollisionPolicy::skip);
    if (reserved.items[0].disposition != MediaPlanDisposition::rejected) {
        throw std::runtime_error("WP11 accepted reserved Windows name");
    }
    const auto extension = buildMediaPlan(root / L"library",
        {{sourceA, L"song.mp3"}}, MediaOrganizationMode::copy,
        MediaCollisionPolicy::skip);
    if (extension.items[0].disposition != MediaPlanDisposition::rejected) {
        throw std::runtime_error("WP11 allowed codec-extension substitution");
    }
    const auto first = rename.items[0].destination;
    fs::create_directories(first.parent_path(), error);
    { std::ofstream(first, std::ios::binary) << "occupied"; }
    const auto skip = buildMediaPlan(root / L"library", {duplicateNames[0]},
        MediaOrganizationMode::copy, MediaCollisionPolicy::skip);
    if (skip.items[0].disposition != MediaPlanDisposition::skipped) {
        throw std::runtime_error("WP11 Skip collision policy failed");
    }
    const auto copyPlan = buildMediaPlan(root / L"library",
        {{sourceA, L"Artist/New Album/copied.flac"}},
        MediaOrganizationMode::copy, MediaCollisionPolicy::skip);
    const auto sourceBefore = fs::last_write_time(sourceA, error);
    const auto journal = root / L"copy-journal.json";
    const auto copyResult = executeCopyPlan(copyPlan, {}, {journal});
    if (copyResult.size() != 1 ||
        copyResult[0].outcome != MediaCopyOutcome::copied ||
        !fs::exists(copyPlan.items[0].destination) ||
        fs::file_size(sourceA) != fs::file_size(copyPlan.items[0].destination) ||
        fs::last_write_time(sourceA, error) != sourceBefore ||
        !fs::exists(journal)) {
        throw std::runtime_error("WP11 verified Copy transaction failed");
    }
    std::ifstream journalStream(journal, std::ios::binary);
    const std::string journalText((std::istreambuf_iterator<char>(journalStream)), {});
    if (journalText.find("\"outcome\":\"Copied\"") == std::string::npos ||
        journalText.find("\"staging\":\"\"") == std::string::npos ||
        journalText.find("\"sourceSha256\":\"") == std::string::npos ||
        copyResult[0].sourceSha256.empty()) {
        throw std::runtime_error("WP11 Copy journal did not reach terminal state");
    }
    const auto replacementPlan = buildMediaPlan(root / L"library",
        {{sourceB, L"Artist/New Album/copied.flac"}},
        MediaOrganizationMode::copy, MediaCollisionPolicy::replace);
    const auto replacement = executeCopyPlan(replacementPlan);
    if (replacement[0].outcome != MediaCopyOutcome::rejected) {
        throw std::runtime_error("WP11 executed Replace without destination backup");
    }
    const auto replacementJournal = root / L"replace-journal.json";
    const auto replacementResult = executeCopyPlan(replacementPlan, {},
        {replacementJournal, root / L"recovery"});
    if (replacementResult[0].outcome != MediaCopyOutcome::copied ||
        !fs::exists(replacementResult[0].backup) ||
        fs::file_size(replacementResult[0].backup) != 1 ||
        fs::file_size(replacementPlan.items[0].destination) != 1) {
        throw std::runtime_error("WP11 Replace backup transaction failed");
    }
    const auto duplicateReplace = buildMediaPlan(root / L"library",
        {{sourceA, L"Artist/New Album/copied.flac"},
         {sourceB, L"Artist/New Album/copied.flac"}},
        MediaOrganizationMode::copy, MediaCollisionPolicy::replace);
    if (duplicateReplace.items[0].disposition != MediaPlanDisposition::ready ||
        duplicateReplace.items[1].disposition != MediaPlanDisposition::rejected) {
        throw std::runtime_error("WP11 allowed two Replace writers for one resource");
    }
    const auto guardedReplace = buildMediaPlan(root / L"library",
        {{sourceA, L"Artist/New Album/copied.flac"}},
        MediaOrganizationMode::copy, MediaCollisionPolicy::replace);
    MediaCopyExecutionOptions guardedOptions;
    guardedOptions.journalPath = root / L"guarded-replace-journal.json";
    guardedOptions.backupRoot = root / L"guarded-recovery";
    guardedOptions.mutateStaging = [destination = guardedReplace.items[0].destination](
            const fs::path&, std::wstring&) {
        std::ofstream external(destination, std::ios::binary | std::ios::app);
        external << "external";
        return static_cast<bool>(external);
    };
    const auto guardedResult = executeMediaPlan(guardedReplace, {}, guardedOptions);
    if (guardedResult[0].outcome != MediaCopyOutcome::conflict ||
        fs::file_size(guardedReplace.items[0].destination) != 9) {
        throw std::runtime_error("WP11 Replace overwrote a post-backup external change");
    }

    const auto sourceMove = root / L"source" / L"move.flac";
    { std::ofstream(sourceMove, std::ios::binary) << "move-me"; }
    const auto movePlan = buildMediaPlan(root / L"library",
        {{sourceMove, L"Artist/Moved/move.flac"}},
        MediaOrganizationMode::move, MediaCollisionPolicy::skip);
    const auto moveResult = executeMediaPlan(movePlan, {},
        {root / L"move-journal.json"});
    if (moveResult[0].outcome != MediaCopyOutcome::moved ||
        fs::exists(sourceMove) || !fs::exists(movePlan.items[0].destination)) {
        throw std::runtime_error("WP11 same-volume Move failed");
    }

    const auto caseRoot = root / L"case-move";
    fs::create_directories(caseRoot, error);
    const auto sourceCase = caseRoot / L"Case.FLAC";
    { std::ofstream(sourceCase, std::ios::binary) << "case-only"; }
    const auto casePlan = buildMediaPlan(caseRoot,
        {{sourceCase, L"case.flac"}}, MediaOrganizationMode::move,
        MediaCollisionPolicy::skip);
    if (!casePlan.items[0].caseOnlyRename ||
        casePlan.items[0].disposition != MediaPlanDisposition::ready) {
        throw std::runtime_error("WP11 case-only Move was not planned explicitly");
    }
    const auto caseResult = executeMediaPlan(casePlan, {},
        {root / L"case-journal.json"});
    if (caseResult[0].outcome != MediaCopyOutcome::moved ||
        !fs::exists(casePlan.items[0].destination)) {
        throw std::runtime_error("WP11 case-only two-step Move failed");
    }

    const auto sourceCross = root / L"source" / L"cross.flac";
    { std::ofstream(sourceCross, std::ios::binary) << "cross-volume"; }
    const auto crossPlan = buildMediaPlan(root / L"library",
        {{sourceCross, L"Artist/Moved/cross.flac"}},
        MediaOrganizationMode::move, MediaCollisionPolicy::skip);
    MediaCopyExecutionOptions crossOptions;
    crossOptions.journalPath = root / L"cross-journal.json";
    crossOptions.sameVolumeOverride = false;
    crossOptions.allowStep = [](std::wstring_view step) {
        return step != L"before-source-delete";
    };
    const auto crossResult = executeMediaPlan(crossPlan, {}, crossOptions);
    if (crossResult[0].outcome != MediaCopyOutcome::duplicateRecoverable ||
        !fs::exists(sourceCross) || !fs::exists(crossPlan.items[0].destination)) {
        throw std::runtime_error("WP11 cross-volume recovery boundary failed");
    }

    const auto sourceEdited = root / L"source" / L"edited.flac";
    { std::ofstream(sourceEdited, std::ios::binary) << "audio"; }
    const auto editedPlan = buildMediaPlan(root / L"library",
        {{sourceEdited, L"Artist/Edited/edited.flac"}},
        MediaOrganizationMode::copy, MediaCollisionPolicy::skip);
    MediaCopyExecutionOptions editOptions;
    editOptions.journalPath = root / L"edit-journal.json";
    editOptions.mutateStaging = [](const fs::path& path, std::wstring&) {
        std::ofstream stream(path, std::ios::binary | std::ios::app);
        stream << "-tags-art";
        return static_cast<bool>(stream);
    };
    editOptions.verifyStaging = [](const fs::path& path, std::wstring&) {
        return fs::file_size(path) == 14;
    };
    const auto editedResult = executeMediaPlan(editedPlan, {}, editOptions);
    if (editedResult[0].outcome != MediaCopyOutcome::copied ||
        editedResult[0].bytes != 14 ||
        fs::file_size(sourceEdited) != 5 ||
        fs::file_size(editedPlan.items[0].destination) != 14) {
        throw std::runtime_error("WP11 staging mutation/readback failed");
    }
    const auto sourceEditedMove = root / L"source" / L"edited-move.flac";
    { std::ofstream(sourceEditedMove, std::ios::binary) << "move-audio"; }
    const auto editedMovePlan = buildMediaPlan(root / L"library",
        {{sourceEditedMove, L"Artist/Edited/edited-move.flac"}},
        MediaOrganizationMode::move, MediaCollisionPolicy::skip);
    MediaCopyExecutionOptions editedMoveOptions;
    editedMoveOptions.journalPath = root / L"edited-move-journal.json";
    editedMoveOptions.backupRoot = root / L"edited-move-recovery";
    editedMoveOptions.mutateStaging = [](const fs::path& path, std::wstring&) {
        std::ofstream stream(path, std::ios::binary | std::ios::app);
        stream << "-edited";
        return static_cast<bool>(stream);
    };
    const auto editedMoveResult = executeMediaPlan(
        editedMovePlan, {}, editedMoveOptions);
    if (editedMoveResult[0].outcome != MediaCopyOutcome::moved ||
        fs::exists(sourceEditedMove) ||
        !fs::exists(editedMoveResult[0].sourceBackup) ||
        fs::file_size(editedMoveResult[0].sourceBackup) != 10 ||
        fs::file_size(editedMovePlan.items[0].destination) != 17) {
        throw std::runtime_error("WP11 edited Move lacks verified source recovery backup");
    }

    const auto sourceCrash = root / L"source" / L"crash.flac";
    { std::ofstream(sourceCrash, std::ios::binary) << "crash-safe"; }
    const auto crashPlan = buildMediaPlan(root / L"library",
        {{sourceCrash, L"Artist/Crash/crash.flac"}},
        MediaOrganizationMode::move, MediaCollisionPolicy::skip);
    MediaCopyExecutionOptions crashOptions;
    crashOptions.journalPath = root / L"crash-journal.json";
    crashOptions.sameVolumeOverride = false;
    crashOptions.allowStep = [](std::wstring_view step) {
        return step != L"before-final-rename";
    };
    const auto crashResult = executeMediaPlan(crashPlan, {}, crashOptions);
    if (crashResult[0].outcome != MediaCopyOutcome::conflict ||
        !fs::exists(sourceCrash) || fs::exists(crashPlan.items[0].destination)) {
        throw std::runtime_error("WP11 pre-commit fault crossed destructive boundary");
    }

    const auto sourceFull = root / L"source" / L"full.flac";
    { std::ofstream(sourceFull, std::ios::binary) << "disk-full"; }
    const auto fullPlan = buildMediaPlan(root / L"library",
        {{sourceFull, L"Artist/Full/full.flac"}},
        MediaOrganizationMode::copy, MediaCollisionPolicy::skip);
    MediaCopyExecutionOptions fullOptions;
    fullOptions.journalPath = root / L"full-journal.json";
    fullOptions.mutateStaging = [](const fs::path&, std::wstring& failure) {
        failure = L"Injected disk-full staging failure";
        return false;
    };
    const auto fullResult = executeMediaPlan(fullPlan, {}, fullOptions);
    if (fullResult[0].outcome != MediaCopyOutcome::failed ||
        !fs::exists(sourceFull) || fs::exists(fullPlan.items[0].destination)) {
        throw std::runtime_error("WP11 staging write failure left committed output");
    }

    const auto sidecarSource = root / L"source" / L"sidecar.flac";
    { std::ofstream(sidecarSource, std::ios::binary) << "sidecar-audio"; }
    const auto sidecarPlan = buildMediaPlan(root / L"library",
        {{sidecarSource, L"Artist/Sidecar/song.flac"}},
        MediaOrganizationMode::copy, MediaCollisionPolicy::skip);
    MediaCopyExecutionOptions sidecarOptions;
    sidecarOptions.journalPath = root / L"sidecar-journal.json";
    sidecarOptions.sidecars.push_back({L"folder", L".png",
        std::make_shared<const ValidatedBytes>(
            ValidatedBytes{1, 2, 3, 4, 5})});
    const auto sidecarResult = executeMediaPlan(sidecarPlan, {}, sidecarOptions);
    const auto sidecarPath = sidecarPlan.items[0].destination.parent_path() /
        L"folder.png";
    if (sidecarResult[0].outcome != MediaCopyOutcome::copied ||
        sidecarResult[0].sidecarsCommitted != 1 ||
        !fs::exists(sidecarPath) || fs::file_size(sidecarPath) != 5) {
        throw std::runtime_error("WP11 explicit sidecar transaction failed");
    }
    const auto sidecarReplaceSource = root / L"source" / L"sidecar-replace.flac";
    { std::ofstream(sidecarReplaceSource, std::ios::binary) << "replace-audio"; }
    const auto sidecarReplacePlan = buildMediaPlan(root / L"library",
        {{sidecarReplaceSource, L"Artist/Sidecar/song-2.flac"}},
        MediaOrganizationMode::copy, MediaCollisionPolicy::replace);
    MediaCopyExecutionOptions sidecarReplaceOptions;
    sidecarReplaceOptions.journalPath = root / L"sidecar-replace-journal.json";
    sidecarReplaceOptions.backupRoot = root / L"sidecar-recovery";
    sidecarReplaceOptions.sidecars.push_back(
        {L"folder", L".png", std::make_shared<const ValidatedBytes>(
            ValidatedBytes{9, 8, 7, 6})});
    const auto sidecarReplaceResult = executeMediaPlan(
        sidecarReplacePlan, {}, sidecarReplaceOptions);
    if (sidecarReplaceResult[0].outcome != MediaCopyOutcome::copied ||
        sidecarReplaceResult[0].sidecarsCommitted != 1 ||
        sidecarReplaceResult[0].sidecarBackups.size() != 1 ||
        !fs::exists(sidecarReplaceResult[0].sidecarBackups[0]) ||
        fs::file_size(sidecarReplaceResult[0].sidecarBackups[0]) != 5 ||
        fs::file_size(sidecarPath) != 4) {
        throw std::runtime_error("WP11 sidecar Replace lacks verified recovery");
    }
    std::stop_source cancelled;
    cancelled.request_stop();
    const auto cancelledPlan = buildMediaPlan(root / L"library",
        {{sourceB, L"cancelled.flac"}}, MediaOrganizationMode::copy,
        MediaCollisionPolicy::skip);
    const auto cancelledResult = executeCopyPlan(cancelledPlan,
        cancelled.get_token(), {root / L"cancelled-journal.json"});
    if (cancelledResult[0].outcome != MediaCopyOutcome::cancelled ||
        fs::exists(cancelledPlan.items[0].destination)) {
        throw std::runtime_error("WP11 cancellation wrote a destination");
    }
    fs::remove_all(root, error);
}
