#include "media_file_operations.h"
#include "nekorox/track_identity.h"
#ifndef EXPECT_NO_MEDIA_RECOVERY
#include "media_binding_recovery.h"
#endif
#include <windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace nekorox;
using namespace nekorox::component;
namespace {
void require(bool v, const char* label) { if (!v) throw std::runtime_error(label); }
std::string bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
PersistedTrack identify(const std::filesystem::path& path, std::uint32_t subsong = 0) {
    PersistedTrack track;
    track.subsong = subsong;
    track.fileSize = std::filesystem::file_size(path);
    track.modifiedTicks = static_cast<std::uint64_t>(
        std::filesystem::last_write_time(path).time_since_epoch().count());
    auto id = makeTrackId({path, subsong, track.fileSize, track.modifiedTicks});
    track.trackId = id.value; track.path = id.normalizedPathUtf8;
    track.partialFingerprint = id.partialFingerprint;
    return track;
}
}
int wmain(int argc, wchar_t** argv) { try {
    require(argc == 3, "mode and fresh fixture root required");
    const std::wstring mode(argv[1]); const std::filesystem::path root(argv[2]);
    require(root.is_absolute() && root.wstring().find(L"continuous-20261014-media-recovery") != std::wstring::npos,
        "Only this round's disposable fixtures allowed");
    const auto file = root / L"media" / L"track.flac";
    const auto journal = root / L"journals" / L"item.json";
    std::string error;
    if (mode == L"produce" || mode == L"produce-intent" || mode == L"produce-dbcommit" || mode == L"produce-undo") {
        require(!std::filesystem::exists(root), "Refusing to overwrite fixture");
        std::filesystem::create_directories(file.parent_path());
        { std::ofstream out(file, std::ios::binary); out << std::string(180000, 'a'); }
        auto old = identify(file);
        NekoDatabase db; require(db.open(root / L"state.db", error), error.c_str());
        require(db.setFavorite(old, true, 1001, error), error.c_str());
        require(db.setNote(old, "Preserve entity A note", 1002, error), error.c_str());
        TrackIdentityResolution identity;
        require(db.resolveTrackIdentity(old.trackId, identity, error) && identity.found, "Seed identity");
        { std::ofstream out(root / L"expected.txt"); out << old.trackId << '\n' << identity.entityId << '\n'; }
        db.close();
        auto plan = buildMediaPlan({}, {{file, {}}}, MediaOrganizationMode::inPlace, MediaCollisionPolicy::replace);
        MediaCopyExecutionOptions options; options.journalPath = journal; options.backupRoot = root / L"backups";
#ifndef EXPECT_NO_MEDIA_RECOVERY
        options.captureSourceRevisions = [](const auto& path) {
            const auto old = identify(path); MediaRevisionBinding binding;
            binding.previousRevisionTrackId = old.trackId; binding.current.subsong = old.subsong;
            return std::vector<MediaRevisionBinding>{binding};
        };
#endif
        options.mutateStaging = [](const auto& path, auto&) {
            std::ofstream out(path, std::ios::binary | std::ios::app); out << "-EMBEDDED-ARTWORK"; return bool(out);
        };
        if (mode == L"produce-intent") options.allowStep = [](std::wstring_view step) {
            if (step == L"after-final-rename") ExitProcess(77);
            return true;
        };
        const auto results = executeMediaPlan(plan, {}, options);
        require(results.size() == 1 && results[0].outcome == MediaCopyOutcome::copied, "Physical commit failed");
        require(bytes(file).ends_with("-EMBEDDED-ARTWORK"), "Committed bytes missing");
        require(std::filesystem::exists(journal), "Durable item journal missing");
#ifndef EXPECT_NO_MEDIA_RECOVERY
        if (mode == L"produce-dbcommit" || mode == L"produce-undo") {
            require(db.open(root / L"state.db",error), error.c_str());
            require(db.bindKnownRevision(results[0].revisionBindings[0].current,identity.entityId,error),error.c_str());
            db.close();
            if (mode == L"produce-undo") {
                MediaBindingRecoveryItem item{results[0].bindingJournalPath,results[0].bindingJournalSha256,results[0].revisionBindings};
                require(acknowledgeMediaBindingRecovery(item,{}),"Forward confirmation before Undo");
                // Backups are byte-exact; their filesystem timestamp may differ.
                auto stamp=std::filesystem::last_write_time(results[0].backup);
                std::filesystem::last_write_time(results[0].backup,stamp+std::chrono::seconds(123));
                MediaRestoreExecutionOptions restore;restore.journalPath=root/L"journals"/L"restore.json";
                restore.allowStep=[](auto step){if(step==L"after-undo-rename")ExitProcess(77);return true;};
                (void)restoreInPlaceBackups(results,{},restore);
                throw std::runtime_error("Undo crash point was not reached");
            }
        }
#endif
        // Deliberate process death after physical commit and before any DB rebind.
        // No destructor, service callback or in-memory result is available to replay.
        ExitProcess(77);
    }
    require(mode == L"recover", "Unknown mode");
    std::ifstream expected(root / L"expected.txt"); std::string previous, entity;
    std::getline(expected, previous); std::getline(expected, entity);
    require(!previous.empty() && !entity.empty(), "Producer evidence missing");
    const auto before = bytes(file); auto current = identify(file);
    require(current.trackId != previous, "Physical revision did not change");
    NekoDatabase db; require(db.open(root / L"state.db", error), error.c_str());
    require(db.beginSession("recovery-fixture", 2000, "QA", error), error.c_str());
    PersistedPlayEvent event; event.sessionId = "recovery-fixture";
    event.trackId = current.trackId; event.utcMs = 2001; event.eventUid = "recovery-start";
    require(db.recordPlayEvent(current, event, error), error.c_str()); // playback may create B before recovery
#ifndef EXPECT_NO_MEDIA_RECOVERY
    auto scan = scanMediaBindingRecovery(root / L"journals", {});
    require(scan.pending.size() == 1, "Expected one durable binding intent");
    auto& item = scan.pending.front();
    std::vector<KnownRevisionBinding> bindings;
    for (const auto& binding : item.revisions) {
        TrackIdentityResolution old;
        require(db.resolveTrackIdentity(binding.previousRevisionTrackId, old, error) && old.found, "Previous entity missing");
        bindings.push_back({binding.current, old.entityId});
    }
    require(db.bindKnownRevisions(bindings, error), error.c_str());
    require(acknowledgeMediaBindingRecovery(item, {}), "Binding confirmation failed");
    require(scanMediaBindingRecovery(root / L"journals", {}).pending.empty(), "Acknowledged item replayed");
#endif
    PersistedTrackUserState state; require(db.readTrackUserState(current.trackId, state, error), error.c_str());
#ifdef EXPECT_NO_MEDIA_RECOVERY
    require(state.entityId != entity && !state.favorite && state.note.empty(), "Baseline gap was not reproduced");
    require(bytes(journal).find(previous) == std::string::npos, "Baseline unexpectedly contains durable prior identity");
    std::cout << "EXPECTED BASELINE GAP: committed B has a different entity and no favorite/note; old revision absent from journal.\n";
#else
    require(state.entityId == entity && state.favorite && state.note == "Preserve entity A note", "Recovery lost personal state");
    std::cout << "PASS: fresh process recovered favorite/note from durable journal.\n";
#endif
    require(bytes(file) == before, "Recovery altered media bytes");
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
