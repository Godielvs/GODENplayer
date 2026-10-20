#include "nekorox/database.h"

#include "../vendor/sqlite/sqlite3.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <windows.h>

namespace {

struct TestDatabaseFiles {
    std::filesystem::path path;
    ~TestDatabaseFiles()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.wstring() + L"-wal", ignored);
        std::filesystem::remove(path.wstring() + L"-shm", ignored);
        std::filesystem::remove(path.wstring() + L".pre-schema-v4.bak", ignored);
        std::filesystem::remove(path.wstring() + L".pre-schema-v5.bak", ignored);
        std::filesystem::remove(path.wstring() + L".pre-schema-v6.bak", ignored);
    }
};

bool sqliteExec(sqlite3* database, const char* sql)
{
    char* message = nullptr;
    const auto result = sqlite3_exec(database, sql, nullptr, nullptr, &message);
    sqlite3_free(message);
    return result == SQLITE_OK;
}

int sqliteInteger(sqlite3* database, const char* sql)
{
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return -1;
    }
    const auto value = sqlite3_step(statement) == SQLITE_ROW
        ? sqlite3_column_int(statement, 0) : -1;
    sqlite3_finalize(statement);
    return value;
}

void requireIdentity(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void testDatabaseInitializationOwnership(const std::filesystem::path& temporary)
{
    TestDatabaseFiles files{temporary / (L"goden-init-ownership-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64()) + L".db")};
    sqlite3* raw = nullptr;
    const auto opened = sqlite3_open16(files.path.c_str(), &raw);
    HANDLE file = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION identity{};
    const bool identified = opened == SQLITE_OK &&
        sqlite3_file_control(raw, "main", SQLITE_FCNTL_WIN32_GET_HANDLE, &file) == SQLITE_OK &&
        GetFileInformationByHandle(file, &identity);
    if (raw) sqlite3_close(raw);
    requireIdentity(identified, "Could not identify initialization ownership fixture");
    // The name is an interprocess ownership contract, derived from the actual
    // file, not a timing assumption about how long schema creation takes.
    const auto name = L"Local\\GODENplayer.DatabaseInit." +
        std::to_wstring(identity.dwVolumeSerialNumber) + L"." +
        std::to_wstring(identity.nFileIndexHigh) + L"." +
        std::to_wstring(identity.nFileIndexLow);
    const auto mutex = CreateMutexW(nullptr, TRUE, name.c_str());
    requireIdentity(mutex != nullptr, "Could not hold database initialization ownership");
    TestDatabaseFiles other{temporary / (L"goden-init-independent-" +
        std::to_wstring(GetCurrentProcessId()) + L".db")};
    nekorox::NekoDatabase independent;
    std::string independentError;
    const auto independentOk = independent.open(other.path, independentError);
    if (!independentOk) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        throw std::runtime_error("Unrelated database initialization was blocked: " + independentError);
    }
    bool success = false;
    std::string error;
    std::thread blocked([&] {
        nekorox::NekoDatabase database;
        success = database.open(files.path, error);
    });
    blocked.join();
    ReleaseMutex(mutex);
    if (success || error.find("initialization timed out") == std::string::npos) {
        CloseHandle(mutex);
        throw std::runtime_error("Competing initializer did not fail with a bounded, specific timeout");
    }
    // A dead owner leaves an abandoned mutex, but it must not prevent SQLite's
    // normal recovery and a complete schema migration by the next opener.
    bool abandonedOwned = false;
    std::thread abandoned([&] {
        abandonedOwned = WaitForSingleObject(mutex, 10000) == WAIT_OBJECT_0;
        // Intentionally do not release: this test thread exits owning it.
    });
    abandoned.join();
    if (!abandonedOwned) {
        CloseHandle(mutex);
        throw std::runtime_error("Test could not abandon initialization ownership");
    }
    nekorox::NekoDatabase recovered;
    error.clear();
    const auto recoveredOk = recovered.open(files.path, error);
    CloseHandle(mutex);
    requireIdentity(recoveredOk && recovered.schemaVersion() == nekorox::kDatabaseSchemaVersion,
        "Abandoned initialization owner did not recover: " + error);
}

void testBindingStateReconciliation(const std::filesystem::path& temporary)
{
    TestDatabaseFiles files{temporary /
        (L"goden-binding-state-" + std::to_wstring(GetCurrentProcessId()) + L".db")};
    nekorox::NekoDatabase database;
    std::string error;
    requireIdentity(database.open(files.path, error), error);
    auto track = [](const char* id) {
        nekorox::PersistedTrack value;
        value.trackId = id;
        value.path = "c:/binding-fixture.flac";
        value.title = "before";
        return value;
    };
    auto identity = [&](const nekorox::PersistedTrack& value) {
        nekorox::TrackIdentityResolution result;
        requireIdentity(database.resolveTrackIdentity(value.trackId, result, error), error);
        return result;
    };
    auto state = [&](const nekorox::PersistedTrack& value) {
        nekorox::PersistedTrackUserState result;
        requireIdentity(database.readTrackUserState(value.trackId, result, error), error);
        return result;
    };
    const auto a = track("binding-a");
    const auto b = track("binding-b");
    requireIdentity(database.setFavorite(a, true, 10, error), error);
    requireIdentity(database.setNote(a, "old", 11, error), error);
    requireIdentity(database.setFavorite(b, false, 20, error), error);
    requireIdentity(database.setNote(b, "new", 21, error), error);
    const auto owner = identity(a).entityId;
    requireIdentity(database.bindKnownRevision(b, owner, error), error);
    auto result = state(b);
    requireIdentity(!result.favorite && result.note == "new" &&
        result.favoriteUpdatedUtcMs == 20 && result.noteUpdatedUtcMs == 21 &&
        state(a).note == "new", "Newer temporary state was hidden by binding");

    auto c = track("binding-c");
    requireIdentity(database.setFavorite(c, true, 15, error), error);
    requireIdentity(database.setNote(c, "newer note only", 30, error), error);
    requireIdentity(database.bindKnownRevision(c, owner, error), error);
    result = state(c);
    requireIdentity(!result.favorite && result.favoriteUpdatedUtcMs == 20 &&
        result.note == "newer note only" && result.noteUpdatedUtcMs == 30,
        "Favorite and note did not merge independently");

    auto clear = track("binding-clear");
    requireIdentity(database.setNote(clear, {}, 40, error), error);
    requireIdentity(database.bindKnownRevision(clear, owner, error), error);
    requireIdentity(state(a).note.empty() && state(a).noteUpdatedUtcMs == 40,
        "A newer explicit clear resurrected an old note");
    auto stale = track("binding-stale");
    requireIdentity(database.setFavorite(stale, true, 20, error), error);
    requireIdentity(database.setNote(stale, "stale", 39, error), error);
    requireIdentity(database.bindKnownRevision(stale, owner, error), error);
    requireIdentity(!state(a).favorite && state(a).note.empty() &&
        state(a).noteUpdatedUtcMs == 40, "Older edits or tied favorite replaced destination");
    auto tie = track("binding-tie");
    requireIdentity(database.setNote(tie, "tie", 40, error), error);
    requireIdentity(database.bindKnownRevision(tie, owner, error), error);
    requireIdentity(state(a).note.empty(), "Equal note timestamps changed the destination");
    auto empty = track("binding-no-state");
    requireIdentity(database.setNote(a, "survives no opinion", 50, error), error);
    requireIdentity(database.bindKnownRevision(empty, owner, error), error);
    requireIdentity(state(empty).note == "survives no opinion",
        "Absent source state was treated as an explicit clear");

    const auto d = track("binding-d");
    requireIdentity(database.setFavorite(d, false, 1, error), error);
    const auto dOwner = identity(d).entityId;
    requireIdentity(!database.bindKnownRevision(b, dOwner, error) &&
        identity(b).entityId == owner, "An established alias was silently moved");
    requireIdentity(!database.bindKnownRevision(a, dOwner, error) &&
        identity(a).entityId == owner, "A shared canonical entity was split");

    sqlite3* raw = nullptr;
    requireIdentity(sqlite3_open16(files.path.c_str(), &raw) == SQLITE_OK,
        "Cannot open binding fixture for failure injection");
    struct Close { sqlite3* db; ~Close() { sqlite3_close_v2(db); } } close{raw};
    requireIdentity(sqliteExec(raw, "PRAGMA foreign_keys=ON;"), "Cannot enable fixture FKs");
    requireIdentity(database.setNote(b, "alias edit", 60, error), error);
    requireIdentity(database.bindKnownRevision(b, owner, error), error);
    requireIdentity(state(a).note == "alias edit" && sqliteInteger(raw,
        "SELECT count(*) FROM track_entities WHERE entity_id='entity:v1:binding-b';") == 0,
        "Alias writes recreated an orphan default entity");
    requireIdentity(sqliteInteger(raw,
        "SELECT count(*) FROM notes WHERE track_id='binding-b';") == 0,
        "Consumed temporary note was left behind");

    auto first = track("binding-batch-first");
    auto second = track("binding-batch-second");
    requireIdentity(database.setNote(first, "first pending", 70, error), error);
    requireIdentity(database.setNote(second, "second pending", 80, error), error);
    first.title = "must roll back";
    std::vector<nekorox::KnownRevisionBinding> batch{{first, owner}, {second, dOwner}};
    requireIdentity(sqliteExec(raw, R"sql(
CREATE TRIGGER reject_second_binding BEFORE UPDATE ON track_revision_bindings
WHEN NEW.revision_track_id='binding-batch-second'
BEGIN SELECT RAISE(ABORT,'injected second binding failure'); END;
)sql"), "Cannot inject later binding failure");
    requireIdentity(!database.bindKnownRevisions(batch, error) &&
        error.find("injected second binding failure") != std::string::npos,
        "Later binding SQL failure was not reported");
    requireIdentity(identity(first).entityId == "entity:v1:binding-batch-first" &&
        identity(second).entityId == "entity:v1:binding-batch-second" &&
        state(first).note == "first pending" && state(a).note == "alias edit" &&
        state(d).note.empty() && sqliteInteger(raw,
            "SELECT count(*) FROM tracks WHERE track_id='binding-batch-first' AND title='before';") == 1,
        "A failed batch committed state, metadata or earlier bindings");
    requireIdentity(sqliteExec(raw, "DROP TRIGGER reject_second_binding;"), "Cannot remove injection");
    requireIdentity(database.bindKnownRevisions(batch, error), error);
    requireIdentity(identity(first).entityId == owner && identity(second).entityId == dOwner &&
        state(a).note == "first pending" && state(d).note == "second pending",
        "Successful batch did not commit both independent states");
    batch = {{first, owner}, {first, owner}};
    requireIdentity(database.bindKnownRevisions(batch, error), error);
    batch = {{first, owner}, {first, dOwner}};
    requireIdentity(!database.bindKnownRevisions(batch, error) && identity(first).entityId == owner,
        "Conflicting duplicate ownership was accepted");

    auto chainB = track("binding-chain-b");
    auto chainC = track("binding-chain-c");
    requireIdentity(database.setNote(chainB, "B", 90, error), error);
    const auto chainOwner = identity(chainB).entityId;
    batch = {{chainB, owner}, {chainC, chainOwner}};
    requireIdentity(!database.bindKnownRevisions(batch, error) &&
        error == "batch contains dependent revision bindings" &&
        identity(chainB).entityId == chainOwner && !identity(chainC).found,
        "Dependent batch bindings were applied in input order");
    std::reverse(batch.begin(), batch.end());
    requireIdentity(!database.bindKnownRevisions(batch, error) && !identity(chainC).found,
        "Reversing dependent bindings changed acceptance");

    const auto journal = track("binding-journal");
    requireIdentity(database.setNote(journal, "journal edit", 100, error), error);
    requireIdentity(sqliteExec(raw, R"sql(
INSERT INTO media_operation_journal(operation_id,item_id,entity_id,state,updated_utc_ms)
VALUES('test','1','entity:v1:binding-journal','complete',1);
)sql"), "Cannot seed journal reference");
    requireIdentity(database.bindKnownRevision(journal, owner, error), error);
    requireIdentity(state(a).note == "journal edit" && sqliteInteger(raw,
        "SELECT count(*) FROM track_entities WHERE entity_id='entity:v1:binding-journal';") == 1,
        "Journal reference prevented binding or was destroyed");
    requireIdentity(sqliteInteger(raw, "SELECT count(*) FROM pragma_foreign_key_check;") == 0 &&
        sqliteInteger(raw, "SELECT count(*) FROM pragma_integrity_check WHERE integrity_check<>'ok';") == 0,
        "Binding left a damaged database");
    database.close();
    requireIdentity(database.open(files.path, error), error);
    requireIdentity(state(b).note == "journal edit" && !state(b).favorite &&
        state(second).note == "second pending", "Merged state did not survive reopening");
}

} // namespace

// This probe is part of the test executable only. It never opens a player/profile.
int runDatabaseInitializationChild(const char* token)
{
    const std::string narrow(token);
    requireIdentity(!narrow.empty() && narrow.size() < 80 &&
        narrow.find_first_not_of("0123456789-") == std::string::npos,
        "Invalid database test token");
    const std::wstring id(narrow.begin(), narrow.end());
    wchar_t temporary[MAX_PATH]{};
    requireIdentity(GetTempPathW(MAX_PATH, temporary) != 0,
        "Database child temporary path unavailable");
    const auto name = L"Local\\GODENplayer.DbInitTest." + id;
    const auto gate = OpenEventW(SYNCHRONIZE, FALSE, name.c_str());
    const auto ready = OpenSemaphoreW(SEMAPHORE_MODIFY_STATE, FALSE,
        (name + L".ready").c_str());
    if (!gate || !ready) {
        if (gate) CloseHandle(gate);
        if (ready) CloseHandle(ready);
        return 2;
    }
    const auto signaled = ReleaseSemaphore(ready, 1, nullptr);
    CloseHandle(ready);
    const auto wait = signaled ? WaitForSingleObject(gate, 10000) : WAIT_FAILED;
    CloseHandle(gate);
    if (wait != WAIT_OBJECT_0) return 3;
    nekorox::NekoDatabase database;
    std::string error;
    const auto path = std::filesystem::path(temporary) /
        (L"goden-init-process-" + id + L".db");
    requireIdentity(database.open(path, error), error);
    requireIdentity(database.schemaVersion() == nekorox::kDatabaseSchemaVersion,
        "Child opened a stale database schema");
    requireIdentity(database.setSetting("child-" + std::to_string(GetCurrentProcessId()),
        "preserved", 1, error), error);
    return 0;
}

void runDatabaseInitializationTests()
{
    wchar_t temporary[MAX_PATH]{};
    requireIdentity(GetTempPathW(MAX_PATH, temporary) != 0,
        "Database initialization temporary path unavailable");
    testDatabaseInitializationOwnership(temporary);
    for (int round = 0; round < 8; ++round) {
        TestDatabaseFiles files{std::filesystem::path(temporary) /
            (L"goden-init-threads-" + std::to_wstring(GetCurrentProcessId()) +
             L"-" + std::to_wstring(GetTickCount64()) + L".db")};
        const auto gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        requireIdentity(gate != nullptr, "Could not create database thread gate");
        std::array<std::thread, 4> threads;
        std::array<std::string, 4> errors;
        std::atomic<unsigned> ready{0};
        try {
            for (std::size_t i = 0; i < threads.size(); ++i) {
                threads[i] = std::thread([&, i] {
                    ready.fetch_add(1, std::memory_order_release);
                    if (WaitForSingleObject(gate, 10000) != WAIT_OBJECT_0) {
                        errors[i] = "Thread start gate timed out";
                        return;
                    }
                    nekorox::NekoDatabase database;
                    if (!database.open(files.path, errors[i])) return;
                    if (database.schemaVersion() != nekorox::kDatabaseSchemaVersion) {
                        errors[i] = "Thread opened a stale schema";
                        return;
                    }
                    database.setSetting("thread-" + std::to_string(i), "preserved",
                        1, errors[i]);
                });
            }
            const auto deadline = GetTickCount64() + 10000;
            while (ready.load(std::memory_order_acquire) < threads.size() &&
                    GetTickCount64() < deadline) Sleep(1);
            requireIdentity(ready.load(std::memory_order_acquire) == threads.size(),
                "Database threads did not reach start gate");
        } catch (...) {
            SetEvent(gate);
            for (auto& thread : threads) if (thread.joinable()) thread.join();
            CloseHandle(gate);
            throw;
        }
        SetEvent(gate);
        for (auto& thread : threads) thread.join();
        CloseHandle(gate);
        for (const auto& error : errors) requireIdentity(error.empty(),
            "Concurrent database initialization failed: " + error);
        sqlite3* raw = nullptr;
        if (sqlite3_open16(files.path.c_str(), &raw) != SQLITE_OK) {
            if (raw) sqlite3_close(raw);
            throw std::runtime_error("Could not inspect concurrent initialization");
        }
        const bool valid = sqliteInteger(raw, "PRAGMA user_version;") ==
                nekorox::kDatabaseSchemaVersion &&
            sqliteInteger(raw, "SELECT count(*) FROM settings WHERE key LIKE 'thread-%' AND value='preserved';") == 4 &&
            sqliteInteger(raw, "SELECT count(*) FROM pragma_integrity_check WHERE integrity_check<>'ok';") == 0 &&
            sqliteInteger(raw, "SELECT count(*) FROM pragma_foreign_key_check;") == 0;
        sqlite3_close(raw);
        requireIdentity(valid, "Concurrent initialization lost data or damaged schema");
    }
    wchar_t executable[32768]{};
    requireIdentity(GetModuleFileNameW(nullptr, executable, 32768) != 0,
        "Database test executable path unavailable");
    for (int round = 0; round < 2; ++round) {
        const auto id = std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(round);
        TestDatabaseFiles files{std::filesystem::path(temporary) /
            (L"goden-init-process-" + id + L".db")};
        const auto name = L"Local\\GODENplayer.DbInitTest." + id;
        const auto gate = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
        const auto ready = CreateSemaphoreW(nullptr, 0, 4, (name + L".ready").c_str());
        std::array<PROCESS_INFORMATION, 4> children{};
        auto cleanup = [&] {
            for (auto& child : children) {
                if (!child.hProcess) continue;
                if (WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT) {
                    TerminateProcess(child.hProcess, 4);
                    WaitForSingleObject(child.hProcess, 3000);
                }
                CloseHandle(child.hProcess);
                CloseHandle(child.hThread);
                child = {};
            }
            if (gate) CloseHandle(gate);
            if (ready) CloseHandle(ready);
        };
        try {
            requireIdentity(gate && ready, "Could not create process start barrier");
            for (auto& child : children) {
                auto command = L"\"" + std::wstring(executable) +
                    L"\" --database-open-probe " + id;
                STARTUPINFOW startup{sizeof(STARTUPINFOW)};
                requireIdentity(CreateProcessW(executable, command.data(), nullptr,
                    nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                    &startup, &child) != FALSE, "Could not create database test child");
            }
            for (std::size_t i = 0; i < children.size(); ++i)
                requireIdentity(WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0,
                    "Database child did not reach start barrier");
            SetEvent(gate);
            for (const auto& child : children) {
                requireIdentity(WaitForSingleObject(child.hProcess, 15000) == WAIT_OBJECT_0,
                    "Database child did not finish");
                DWORD code = 1;
                requireIdentity(GetExitCodeProcess(child.hProcess, &code) && code == 0,
                    "Concurrent database process failed initialization");
            }
        } catch (...) { cleanup(); throw; }
        cleanup();
        sqlite3* raw = nullptr;
        if (sqlite3_open16(files.path.c_str(), &raw) != SQLITE_OK) {
            if (raw) sqlite3_close(raw);
            throw std::runtime_error("Could not inspect process database");
        }
        const bool valid = sqliteInteger(raw, "PRAGMA user_version;") ==
                nekorox::kDatabaseSchemaVersion &&
            sqliteInteger(raw, "SELECT count(*) FROM settings WHERE key LIKE 'child-%' AND value='preserved';") == 4 &&
            sqliteInteger(raw, "SELECT count(*) FROM pragma_integrity_check WHERE integrity_check<>'ok';") == 0 &&
            sqliteInteger(raw, "SELECT count(*) FROM pragma_foreign_key_check;") == 0;
        sqlite3_close(raw);
        requireIdentity(valid, "Concurrent processes lost data or damaged schema");
    }
}

void runWorkpackIdentityTests()
{
    wchar_t temporary[MAX_PATH]{};
    requireIdentity(GetTempPathW(MAX_PATH, temporary) != 0,
        "WP09 temporary path unavailable");
    testBindingStateReconciliation(temporary);
    TestDatabaseFiles files{std::filesystem::path(temporary) /
        (L"goden-wp09-" + std::to_wstring(GetCurrentProcessId()) + L".db")};

    nekorox::NekoDatabase database;
    std::string error;
    requireIdentity(database.open(files.path, error), error);
    requireIdentity(database.schemaVersion() == nekorox::kDatabaseSchemaVersion,
        "WP09 schema did not migrate to the current version");

    nekorox::PersistedLyricsDocumentV2 lyricDocument;
    lyricDocument.documentId = "lrclib:42";
    lyricDocument.provider = "lrclib";
    lyricDocument.providerRecordId = "42";
    lyricDocument.title = "Title";
    lyricDocument.artist = "Artist";
    lyricDocument.plainTextUtf8 = "line";
    lyricDocument.fetchedUtcMs = 10;
    lyricDocument.expiryUtcMs = 20;
    lyricDocument.policyVersion = 1;
    nekorox::PersistedLyricsBindingV2 lyricBinding;
    lyricBinding.entityId = "recording:test";
    lyricBinding.mediaRevision = "media:1";
    lyricBinding.metadataRevision = 7;
    lyricBinding.documentId = lyricDocument.documentId;
    lyricBinding.updatedUtcMs = 10;
    requireIdentity(database.storeLyricsDocumentAndBinding(
        lyricDocument, lyricBinding, error), error);
    nekorox::PersistedLyricsDocumentV2 loadedLyric;
    nekorox::PersistedLyricsBindingV2 loadedBinding;
    requireIdentity(database.readLyricsForBinding("recording:test", 0,
        "media:1", 7, loadedLyric, loadedBinding, error) &&
        loadedLyric.found && loadedLyric.plainTextUtf8 == "line" &&
        loadedBinding.documentId == "lrclib:42",
        "R3 lyrics document/binding did not round-trip");

    nekorox::PersistedTrack original;
    original.trackId = "v1:revision-a";
    original.path = "c:/music/a.flac";
    original.partialFingerprint = "a";
    original.lastSeenUtcMs = 1;
    requireIdentity(database.setFavorite(original, true, 10, error), error);
    requireIdentity(database.setNote(original, "kept across revision", 11, error), error);

    nekorox::TrackIdentityResolution first;
    requireIdentity(database.resolveTrackIdentity(original.trackId, first, error) &&
        first.found && first.canonicalTrackId == original.trackId &&
        first.entityId != first.revisionTrackId,
        "WP09 canonical, entity and revision identities were conflated");

    auto revised = original;
    revised.trackId = "v1:revision-b";
    revised.partialFingerprint = "b";
    revised.modifiedTicks = 2;
    requireIdentity(database.bindKnownRevision(revised, first.entityId, error), error);
    nekorox::PersistedTrackUserState inherited;
    requireIdentity(database.readTrackUserState(revised.trackId, inherited, error) &&
        inherited.found && inherited.favorite &&
        inherited.note == "kept across revision" &&
        inherited.entityId == first.entityId &&
        inherited.canonicalTrackId == original.trackId &&
        inherited.currentRevisionTrackId == revised.trackId,
        "WP09 known revision did not preserve entity-owned user state");

    auto copy = revised;
    copy.trackId = "v1:copy";
    copy.path = "c:/music/a copy.flac";
    copy.partialFingerprint = "copy";
    requireIdentity(database.setFavorite(copy, false, 12, error), error);
    nekorox::PersistedTrackUserState isolated;
    requireIdentity(database.readTrackUserState(copy.trackId, isolated, error) &&
        isolated.found && !isolated.favorite && isolated.note.empty() &&
        isolated.entityId != first.entityId,
        "WP09 copy incorrectly inherited the source entity");
    database.close();

    TestDatabaseFiles legacy{std::filesystem::path(temporary) /
        (L"goden-wp09-legacy-" + std::to_wstring(GetCurrentProcessId()) + L".db")};
    sqlite3* raw = nullptr;
    requireIdentity(sqlite3_open16(legacy.path.c_str(), &raw) == SQLITE_OK,
        "WP09 could not create legacy fixture");
    const auto legacySql = R"sql(
PRAGMA foreign_keys=ON;
CREATE TABLE tracks(track_id TEXT PRIMARY KEY,path TEXT NOT NULL,subsong INTEGER NOT NULL DEFAULT 0,file_size INTEGER NOT NULL DEFAULT 0,mtime_ticks INTEGER NOT NULL DEFAULT 0,partial_fingerprint TEXT NOT NULL DEFAULT '',artist TEXT NOT NULL DEFAULT '',album TEXT NOT NULL DEFAULT '',title TEXT NOT NULL DEFAULT '',duration_ms INTEGER NOT NULL DEFAULT 0,last_seen_utc_ms INTEGER NOT NULL);
CREATE TABLE sessions(session_id TEXT PRIMARY KEY,started_utc_ms INTEGER NOT NULL,ended_utc_ms INTEGER,app_version TEXT NOT NULL);
CREATE TABLE play_events(id INTEGER PRIMARY KEY AUTOINCREMENT,session_id TEXT NOT NULL REFERENCES sessions(session_id),track_id TEXT NOT NULL REFERENCES tracks(track_id),event_type INTEGER NOT NULL,event_utc_ms INTEGER NOT NULL,position_ms INTEGER NOT NULL DEFAULT 0,duration_ms INTEGER NOT NULL DEFAULT 0,playlist_index INTEGER,item_index INTEGER);
CREATE TABLE favorites(track_id TEXT PRIMARY KEY REFERENCES tracks(track_id),favorite INTEGER NOT NULL,updated_utc_ms INTEGER NOT NULL);
CREATE TABLE notes(track_id TEXT PRIMARY KEY REFERENCES tracks(track_id),body TEXT NOT NULL,updated_utc_ms INTEGER NOT NULL);
CREATE TABLE personas(persona_id TEXT PRIMARY KEY,display_name TEXT NOT NULL,profile TEXT NOT NULL DEFAULT '',updated_utc_ms INTEGER NOT NULL);
CREATE TABLE persona_aliases(alias TEXT NOT NULL,alias_key TEXT PRIMARY KEY,persona_id TEXT NOT NULL REFERENCES personas(persona_id));
CREATE TABLE persona_relationships(source_persona_id TEXT NOT NULL REFERENCES personas(persona_id),target_persona_id TEXT NOT NULL REFERENCES personas(persona_id),relation TEXT NOT NULL,note TEXT NOT NULL DEFAULT '',PRIMARY KEY(source_persona_id,target_persona_id,relation));
CREATE TABLE lyrics(track_id TEXT PRIMARY KEY REFERENCES tracks(track_id),source TEXT NOT NULL DEFAULT 'local',body TEXT NOT NULL,is_synced INTEGER NOT NULL DEFAULT 0,updated_utc_ms INTEGER NOT NULL);
CREATE TABLE analysis_manifests(track_id TEXT NOT NULL REFERENCES tracks(track_id),analyzer_version TEXT NOT NULL,state TEXT NOT NULL,manifest_json TEXT NOT NULL DEFAULT '{}',result_blob BLOB,updated_utc_ms INTEGER NOT NULL,PRIMARY KEY(track_id,analyzer_version));
CREATE TABLE analysis_layers(track_id TEXT NOT NULL,analyzer_version TEXT NOT NULL,layer_name TEXT NOT NULL,resolution_ms INTEGER NOT NULL DEFAULT 0,codec TEXT NOT NULL,uncompressed_bytes INTEGER NOT NULL,stored_bytes INTEGER NOT NULL,payload BLOB NOT NULL,last_access_utc_ms INTEGER NOT NULL,PRIMARY KEY(track_id,analyzer_version,layer_name));
CREATE TABLE schema_migrations(version INTEGER PRIMARY KEY,applied_utc_ms INTEGER NOT NULL);
CREATE TABLE settings(key TEXT PRIMARY KEY,value TEXT NOT NULL,updated_utc_ms INTEGER NOT NULL);
INSERT INTO tracks VALUES('legacy-track','c:/legacy.flac',0,10,20,'fp','Artist','Album','Title',1234,99);
INSERT INTO sessions VALUES('legacy-session',1,NULL,'legacy');
INSERT INTO play_events(session_id,track_id,event_type,event_utc_ms) VALUES('legacy-session','legacy-track',1,2);
INSERT INTO favorites VALUES('legacy-track',1,3);
INSERT INTO notes VALUES('legacy-track','human note',4);
INSERT INTO personas VALUES('person','Artist','human profile',5);
INSERT INTO persona_aliases VALUES('Artist','artist','person');
INSERT INTO lyrics VALUES('legacy-track','local','human lyric',0,6);
INSERT INTO analysis_manifests VALUES('legacy-track','v1','complete','{}',X'01',7);
INSERT INTO analysis_layers VALUES('legacy-track','v1','wave',10,'raw',1,1,X'02',8);
INSERT INTO settings VALUES('human','kept',9);
INSERT INTO schema_migrations VALUES(1,1),(2,2),(3,3);
PRAGMA user_version=3;
)sql";
    requireIdentity(sqliteExec(raw, legacySql), "WP09 legacy fixture seed failed");
    sqlite3_close(raw);

    SetEnvironmentVariableW(L"GODEN_TEST_DB_MIGRATION_FAIL_AFTER_BACKFILL", L"1");
    nekorox::NekoDatabase failedMigration;
    error.clear();
    requireIdentity(!failedMigration.open(legacy.path, error),
        "WP09 migration failpoint did not fail closed");
    SetEnvironmentVariableW(L"GODEN_TEST_DB_MIGRATION_FAIL_AFTER_BACKFILL", nullptr);
    requireIdentity(sqlite3_open16(legacy.path.c_str(), &raw) == SQLITE_OK,
        "WP09 could not inspect rollback fixture");
    requireIdentity(sqliteInteger(raw, "PRAGMA user_version;") == 3 &&
        sqliteInteger(raw, "SELECT count(*) FROM sqlite_master WHERE name='track_entities';") == 0,
        "WP09 failed migration left partial schema state");
    sqlite3_close(raw);

    nekorox::NekoDatabase migrated;
    error.clear();
    requireIdentity(migrated.open(legacy.path, error), error);
    requireIdentity(migrated.schemaVersion() == nekorox::kDatabaseSchemaVersion,
        "WP09 legacy fixture did not reach the current schema");
    nekorox::PersistedTrackUserState legacyState;
    requireIdentity(migrated.readTrackUserState("legacy-track", legacyState, error) &&
        legacyState.found && legacyState.favorite &&
        legacyState.note == "human note",
        "WP09 migration did not preserve human favorite/note content");
    migrated.close();
    requireIdentity(sqlite3_open16(legacy.path.c_str(), &raw) == SQLITE_OK,
        "WP09 could not inspect migrated fixture");
    requireIdentity(sqliteInteger(raw, "SELECT count(*) FROM tracks;") == 1 &&
        sqliteInteger(raw, "SELECT count(*) FROM play_events;") == 1 &&
        sqliteInteger(raw, "SELECT count(*) FROM personas;") == 1 &&
        sqliteInteger(raw, "SELECT count(*) FROM lyrics;") == 1 &&
        sqliteInteger(raw, "SELECT count(*) FROM analysis_manifests;") == 1 &&
        sqliteInteger(raw, "SELECT count(*) FROM analysis_layers;") == 1 &&
        sqliteInteger(raw, "SELECT count(*) FROM track_entities;") == 1 &&
        sqliteInteger(raw, "SELECT count(*) FROM lyrics_documents_v2;") == 1 &&
        sqliteInteger(raw, "SELECT count(*) FROM lyrics_bindings_v2;") == 1,
        "WP09 migration changed legacy table counts");
    requireIdentity(sqliteInteger(raw, "PRAGMA foreign_key_check;") == -1,
        "WP09 legacy fixture has a foreign-key violation");
    sqlite3_close(raw);

    nekorox::NekoDatabase reopened;
    error.clear();
    requireIdentity(reopened.open(legacy.path, error) &&
        reopened.schemaVersion() == nekorox::kDatabaseSchemaVersion,
        "WP09 migration is not idempotent on reopen");

    TestDatabaseFiles future{std::filesystem::path(temporary) /
        (L"goden-wp09-future-" + std::to_wstring(GetCurrentProcessId()) + L".db")};
    requireIdentity(sqlite3_open16(future.path.c_str(), &raw) == SQLITE_OK,
        "WP09 could not create future-schema fixture");
    requireIdentity(sqliteExec(raw,
        "CREATE TABLE human_data(value TEXT);"
        "INSERT INTO human_data VALUES('do not erase');"
        "PRAGMA user_version=99;"),
        "WP09 future-schema fixture seed failed");
    sqlite3_close(raw);
    nekorox::NekoDatabase incompatible;
    error.clear();
    requireIdentity(!incompatible.open(future.path, error) &&
        error.find("newer") != std::string::npos,
        "WP09 future schema did not fail with an actionable diagnosis");
    requireIdentity(sqlite3_open16(future.path.c_str(), &raw) == SQLITE_OK &&
        sqliteInteger(raw, "SELECT count(*) FROM human_data;") == 1,
        "WP09 future-schema rejection changed human data");
    sqlite3_close(raw);
}
