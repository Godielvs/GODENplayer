#include "../src/component/media_file_operations.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <windows.h>

namespace {
std::string readBytes(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
}

void runCorrectiveR2TagMediaTests()
{
    using namespace nekorox::component;
    wchar_t temporary[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temporary) == 0)
        throw std::runtime_error("C06 temporary path unavailable");
    const auto root = std::filesystem::path(temporary) /
        (L"goden-c06-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()));
    const auto media = root / L"album" / L"track.flac";
    const auto recovery = root / L"recovery";
    const auto journal = root / L"journal.json";
    std::filesystem::create_directories(media.parent_path());
    {
        std::ofstream stream(media, std::ios::binary);
        stream << "ORIGINAL-AUDIO";
    }
    const auto original = readBytes(media);
    auto plan = buildMediaPlan({}, {{media, {}}},
        MediaOrganizationMode::inPlace, MediaCollisionPolicy::replace);
    if (plan.items.size() != 1 ||
        plan.items[0].disposition != MediaPlanDisposition::ready)
        throw std::runtime_error("C06 in-place plan was rejected");
    MediaCopyExecutionOptions options;
    options.journalPath = journal;
    options.backupRoot = recovery;
    options.mutateStagingForSource = [](const auto&, const auto& staging,
            std::wstring&) {
        std::ofstream stream(staging, std::ios::binary | std::ios::app);
        stream << "-EMBEDDED-FRONT";
        return static_cast<bool>(stream);
    };
    options.verifyStaging = [](const auto& staging, std::wstring&) {
        return readBytes(staging).ends_with("-EMBEDDED-FRONT");
    };
    const auto result = executeMediaPlan(plan, {}, options);
    if (result.size() != 1 || result[0].outcome != MediaCopyOutcome::copied ||
        !readBytes(media).ends_with("-EMBEDDED-FRONT"))
        throw std::runtime_error("C06 verified in-place replacement failed");
    if (result[0].backup.empty() || readBytes(result[0].backup) != original)
        throw std::runtime_error("C06 byte-exact recovery backup was not retained");
    const auto changed = readBytes(media);
    const auto requireUnchanged = [&] {
        if (readBytes(media) != changed || readBytes(result[0].backup) != original)
            throw std::runtime_error("C06 protected Undo changed target or backup");
        for (const auto& entry : std::filesystem::directory_iterator(media.parent_path()))
            if (entry.path() != media)
                throw std::runtime_error("C06 protected Undo leaked staging");
    };
    MediaRestoreExecutionOptions restoreOptions;
    unsigned checks = 0;
    restoreOptions.allowRestore = [&](const auto& target) {
        if (target != media) throw std::runtime_error("C06 Undo guard has wrong target");
        ++checks;
        return false;
    };
    auto protectedUndo = restoreInPlaceBackups(result, {}, restoreOptions);
    if (checks != 1 || protectedUndo[0].outcome != MediaCopyOutcome::skipped)
        throw std::runtime_error("C06 Undo admission protection failed");
    requireUnchanged();

    checks = 0;
    restoreOptions.allowRestore = [&](const auto&) { return ++checks == 1; };
    protectedUndo = restoreInPlaceBackups(result, {}, restoreOptions);
    if (checks != 2 || protectedUndo[0].outcome != MediaCopyOutcome::skipped)
        throw std::runtime_error("C06 Undo ignored protection acquired during preparation");
    requireUnchanged();

    checks = 0;
    std::stop_source cancel;
    restoreOptions.allowRestore = [&](const auto&) {
        if (++checks == 2) cancel.request_stop();
        return true;
    };
    protectedUndo = restoreInPlaceBackups(result, cancel.get_token(), restoreOptions);
    if (checks != 2 || protectedUndo[0].outcome != MediaCopyOutcome::cancelled)
        throw std::runtime_error("C06 Undo ignored cancellation at replacement boundary");
    requireUnchanged();

    // Releasing playback protection permits retry using the same verified
    // backup and the original post-Apply hash, without another Apply.
    auto restored = restoreInPlaceBackups(result);
    if (restored.size() != 1 || restored[0].outcome != MediaCopyOutcome::moved ||
        readBytes(media) != original)
        throw std::runtime_error("C06 verified in-place undo did not restore original bytes");

    // Restore the committed state, then prove stale undo cannot overwrite an
    // unrelated edit made after Apply.
    std::ofstream changedAgain(media, std::ios::binary | std::ios::trunc);
    changedAgain << changed;
    changedAgain.close();
    std::ofstream external(media, std::ios::binary | std::ios::app);
    external << "-EXTERNAL";
    external.close();
    auto refused = restoreInPlaceBackups(result);
    if (refused.size() != 1 || refused[0].outcome != MediaCopyOutcome::conflict ||
        !readBytes(media).ends_with("-EXTERNAL"))
        throw std::runtime_error("C06 in-place undo overwrote an external modification");
    if (!std::filesystem::exists(journal))
        throw std::runtime_error("C06 durable journal was not written");
    std::error_code error;
    std::filesystem::remove_all(root, error);
}
