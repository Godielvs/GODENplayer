#include "nekorox/database.h"
#include "nekorox/compression.h"

#include "sqlite3.h"
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>
namespace nekorox {
namespace {

constexpr std::size_t kDatabaseMaximumTextBytes = 512U * 1024U;

class DatabaseInitializationLock final {
public:
    ~DatabaseInitializationLock()
    {
        if (owned_) ReleaseMutex(mutex_);
        if (mutex_) CloseHandle(mutex_);
    }

    DatabaseInitializationLock() = default;
    DatabaseInitializationLock(const DatabaseInitializationLock&) = delete;
    DatabaseInitializationLock& operator=(const DatabaseInitializationLock&) = delete;

    bool acquire(sqlite3* database, std::string& error)
    {
        // Borrow SQLite's actual file handle: do not identify a separately
        // reopened pathname, or close the handle owned by its Windows VFS.
        HANDLE file = INVALID_HANDLE_VALUE;
        BY_HANDLE_FILE_INFORMATION identity{};
        if (sqlite3_file_control(database, "main", SQLITE_FCNTL_WIN32_GET_HANDLE,
                &file) != SQLITE_OK || file == INVALID_HANDLE_VALUE ||
                !GetFileInformationByHandle(file, &identity)) {
            error = "database initialization could not identify its persistent file";
            return false;
        }
        const auto name = L"Local\\GODENplayer.DatabaseInit." +
            std::to_wstring(identity.dwVolumeSerialNumber) + L"." +
            std::to_wstring(identity.nFileIndexHigh) + L"." +
            std::to_wstring(identity.nFileIndexLow);
        mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!mutex_) {
            error = "database initialization mutex creation failed: " +
                std::to_string(GetLastError());
            return false;
        }
        const auto wait = WaitForSingleObject(mutex_, 2500);
        // An abandoned owner releases the OS mutex. SQLite still performs its
        // normal journal recovery; an earlier migration is never assumed done.
        owned_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        if (!owned_) {
            error = wait == WAIT_TIMEOUT
                ? "database initialization timed out waiting for another opener"
                : "database initialization mutex wait failed: " +
                    std::to_string(GetLastError());
        }
        return owned_;
    }

private:
    HANDLE mutex_ = nullptr;
    bool owned_ = false;
};

std::string databaseError(sqlite3* database, const char* prefix)
{
    std::string result(prefix);
    result += ": ";
    result += database != nullptr ? sqlite3_errmsg(database) : "database is closed";
    return result;
}

class Statement final {
public:
    Statement(sqlite3* database, const char* sql, std::string& error)
        : database_(database)
    {
        const auto result = sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr);
        if (result != SQLITE_OK) error = databaseError(database, "prepare failed");
    }

    ~Statement()
    {
        if (statement_ != nullptr) sqlite3_finalize(statement_);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    explicit operator bool() const noexcept { return statement_ != nullptr; }
    sqlite3_stmt* get() const noexcept { return statement_; }

    bool bindText(int index, const std::string& value, std::string& error)
    {
        return check(sqlite3_bind_text(
            statement_, index, value.c_str(), static_cast<int>(value.size()),
            SQLITE_TRANSIENT), error, "text bind failed");
    }

    bool bindInt64(int index, std::int64_t value, std::string& error)
    {
        return check(sqlite3_bind_int64(statement_, index, value), error,
            "integer bind failed");
    }

    bool bindNullableInt(int index, std::int32_t value, std::string& error)
    {
        const auto result = value < 0
            ? sqlite3_bind_null(statement_, index)
            : sqlite3_bind_int(statement_, index, value);
        return check(result, error, "nullable integer bind failed");
    }

    bool bindBlob(int index, std::span<const std::byte> value, std::string& error)
    {
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            error = "blob bind exceeds SQLite's size limit";
            return false;
        }
        return check(sqlite3_bind_blob(statement_, index,
            value.empty() ? nullptr : value.data(), static_cast<int>(value.size()),
            SQLITE_TRANSIENT), error, "blob bind failed");
    }

    bool stepDone(std::string& error)
    {
        return check(sqlite3_step(statement_), error, "statement failed", SQLITE_DONE);
    }

private:
    bool check(
        int result,
        std::string& error,
        const char* prefix,
        int expected = SQLITE_OK)
    {
        if (result == expected) return true;
        error = databaseError(database_, prefix);
        return false;
    }

    sqlite3* database_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

const char* columnText(sqlite3_stmt* statement, int column) noexcept
{
    const auto* value = sqlite3_column_text(statement, column);
    return value != nullptr ? reinterpret_cast<const char*>(value) : "";
}

std::vector<std::byte> columnBlob(sqlite3_stmt* statement, int column)
{
    const auto bytes = sqlite3_column_bytes(statement, column);
    const auto* data = static_cast<const std::byte*>(
        sqlite3_column_blob(statement, column));
    if (bytes <= 0 || data == nullptr) return {};
    return std::vector<std::byte>(data, data + bytes);
}

std::int64_t utcNowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool bindTrack(Statement& statement, const PersistedTrack& track, std::string& error)
{
    return statement.bindText(1, track.trackId, error) &&
        statement.bindText(2, track.path, error) &&
        statement.bindInt64(3, track.subsong, error) &&
        statement.bindInt64(4, static_cast<std::int64_t>(track.fileSize), error) &&
        statement.bindInt64(5, static_cast<std::int64_t>(track.modifiedTicks), error) &&
        statement.bindText(6, track.partialFingerprint, error) &&
        statement.bindText(7, track.artist, error) &&
        statement.bindText(8, track.album, error) &&
        statement.bindText(9, track.title, error) &&
        statement.bindInt64(10, track.durationMs, error) &&
        statement.bindInt64(11, track.lastSeenUtcMs, error);
}

bool bindEvent(Statement& statement, const PersistedPlayEvent& event, std::string& error)
{
    return statement.bindText(1, event.eventUid, error) &&
        statement.bindText(2, event.sessionId, error) &&
        statement.bindText(3, event.trackId, error) &&
        statement.bindInt64(4, static_cast<std::int32_t>(event.type), error) &&
        statement.bindInt64(5, event.utcMs, error) &&
        statement.bindInt64(6, event.positionMs, error) &&
        statement.bindInt64(7, event.durationMs, error) &&
        statement.bindNullableInt(8, event.playlistIndex, error) &&
        statement.bindNullableInt(9, event.itemIndex, error);
}

bool upsertTrack(sqlite3* database, const PersistedTrack& track, std::string& error)
{
    Statement statement(database, R"sql(
INSERT INTO tracks(
    track_id, path, subsong, file_size, mtime_ticks, partial_fingerprint,
    artist, album, title, duration_ms, last_seen_utc_ms)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)
ON CONFLICT(track_id) DO UPDATE SET
    path=excluded.path,
    subsong=excluded.subsong,
    file_size=excluded.file_size,
    mtime_ticks=excluded.mtime_ticks,
    partial_fingerprint=excluded.partial_fingerprint,
    artist=CASE WHEN excluded.artist<>'' THEN excluded.artist ELSE tracks.artist END,
    album=CASE WHEN excluded.album<>'' THEN excluded.album ELSE tracks.album END,
    title=CASE WHEN excluded.title<>'' THEN excluded.title ELSE tracks.title END,
    duration_ms=CASE WHEN excluded.duration_ms>0 THEN excluded.duration_ms ELSE tracks.duration_ms END,
    last_seen_utc_ms=MAX(excluded.last_seen_utc_ms, tracks.last_seen_utc_ms);
)sql", error);
    if (!statement || !bindTrack(statement, track, error) ||
        !statement.stepDone(error)) return false;
    const auto entityId = "entity:v1:" + track.trackId;
    Statement entity(database, R"sql(
INSERT OR IGNORE INTO track_entities(entity_id, canonical_track_id)
SELECT ?1, ?2
WHERE NOT EXISTS(SELECT 1 FROM track_revision_bindings WHERE revision_track_id=?2);
)sql", error);
    if (!entity || !entity.bindText(1, entityId, error) ||
        !entity.bindText(2, track.trackId, error) || !entity.stepDone(error)) {
        return false;
    }
    Statement binding(database, R"sql(
INSERT OR IGNORE INTO track_revision_bindings(revision_track_id, entity_id)
VALUES(?1, ?2);
)sql", error);
    return binding && binding.bindText(1, track.trackId, error) &&
        binding.bindText(2, entityId, error) && binding.stepDone(error);
}

bool backupDatabase(sqlite3* source, const std::filesystem::path& path,
    int targetVersion, std::string& error)
{
    const auto backupPath = std::filesystem::path(path.wstring() +
        L".pre-schema-v" + std::to_wstring(targetVersion) + L".bak");
    std::error_code existsError;
    if (std::filesystem::exists(backupPath, existsError)) return true;
    sqlite3* destination = nullptr;
    if (sqlite3_open16(backupPath.c_str(), &destination) != SQLITE_OK) {
        error = databaseError(destination, "migration backup open failed");
        if (destination) sqlite3_close_v2(destination);
        return false;
    }
    auto* backup = sqlite3_backup_init(destination, "main", source, "main");
    bool success = backup != nullptr;
    if (success) {
        const auto step = sqlite3_backup_step(backup, -1);
        success = step == SQLITE_DONE;
        if (!success) error = databaseError(destination,
            "migration backup copy failed");
        sqlite3_backup_finish(backup);
    } else {
        error = databaseError(destination, "migration backup init failed");
    }
    if (sqlite3_close_v2(destination) != SQLITE_OK && success) {
        error = "migration backup close failed";
        success = false;
    }
    if (!success) {
        std::error_code removeError;
        std::filesystem::remove(backupPath, removeError);
    }
    return success;
}

} // namespace

const char* playEventTypeName(PlayEventType type) noexcept
{
    switch (type) {
    case PlayEventType::start: return "start";
    case PlayEventType::pause: return "pause";
    case PlayEventType::resume: return "resume";
    case PlayEventType::seek: return "seek";
    case PlayEventType::skip: return "skip";
    case PlayEventType::finish: return "finish";
    case PlayEventType::stop: return "stop";
    }
    return "unknown";
}

NekoDatabase::~NekoDatabase()
{
    close();
}

bool NekoDatabase::open(const std::filesystem::path& path, std::string& error)
{
    close();
    error.clear();

    std::error_code directoryError;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), directoryError);
    }
    if (directoryError) {
        error = "could not create database directory: " + directoryError.message();
        return false;
    }

    path_ = path;
    if (sqlite3_open16(path.c_str(), &db_) != SQLITE_OK) {
        error = databaseError(db_, "open failed");
        close();
        return false;
    }
    sqlite3_extended_result_codes(db_, 1);
    sqlite3_busy_timeout(db_, 2500);

    // Separate services/processes can open one profile concurrently. Ownership
    // must include WAL setup, the initial version read, backup gaps and every
    // migration commit; individual SQL transactions cannot protect a version
    // cached before another opener advances it. Ordinary queries/writes retain
    // SQLite's existing concurrency after initialization finishes.
    DatabaseInitializationLock initialization;
    if (!initialization.acquire(db_, error)) {
        // This connection never acquired initialization ownership. Do not run
        // close()'s explicit checkpoint against another owner's initialization.
        sqlite3_close_v2(db_);
        db_ = nullptr;
        path_.clear();
        return false;
    }
    if (!execute("PRAGMA foreign_keys=ON;", error) ||
        !execute("PRAGMA journal_mode=WAL;", error) ||
        !execute("PRAGMA synchronous=NORMAL;", error) ||
        !execute("PRAGMA wal_autocheckpoint=1000;", error) ||
        !execute("PRAGMA temp_store=MEMORY;", error) ||
        !migrate(error)) {
        close();
        return false;
    }
    return true;
}

void NekoDatabase::close() noexcept
{
    schemaVersion_ = 0;
    path_.clear();
    if (db_ == nullptr) return;
    sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);
    sqlite3_close_v2(db_);
    db_ = nullptr;
}

bool NekoDatabase::execute(const char* sql, std::string& error)
{
    char* message = nullptr;
    const auto result = sqlite3_exec(db_, sql, nullptr, nullptr, &message);
    if (result == SQLITE_OK) return true;
    error = message != nullptr ? message : databaseError(db_, "SQL failed");
    sqlite3_free(message);
    return false;
}

bool NekoDatabase::migrate(std::string& error)
{
    int current = 0;
    {
        Statement versionStatement(db_, "PRAGMA user_version;", error);
        if (!versionStatement) return false;
        if (sqlite3_step(versionStatement.get()) != SQLITE_ROW) {
            error = databaseError(db_, "could not read schema version");
            return false;
        }
        current = sqlite3_column_int(versionStatement.get(), 0);
    }
    const auto openingVersion = current;
    if (current > kDatabaseSchemaVersion) {
        error = "database schema is newer than this component";
        return false;
    }
    if (current == kDatabaseSchemaVersion) {
        schemaVersion_ = current;
        return true;
    }

    if (current < 1) {
        if (!execute("BEGIN IMMEDIATE;", error)) return false;
        const bool migrated = execute(R"sql(
PRAGMA application_id=1313164111;
CREATE TABLE IF NOT EXISTS schema_migrations (
    version INTEGER PRIMARY KEY,
    applied_utc_ms INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS sessions (
    session_id TEXT PRIMARY KEY,
    started_utc_ms INTEGER NOT NULL,
    ended_utc_ms INTEGER,
    app_version TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS tracks (
    track_id TEXT PRIMARY KEY,
    path TEXT NOT NULL,
    subsong INTEGER NOT NULL DEFAULT 0,
    file_size INTEGER NOT NULL DEFAULT 0,
    mtime_ticks INTEGER NOT NULL DEFAULT 0,
    partial_fingerprint TEXT NOT NULL DEFAULT '',
    artist TEXT NOT NULL DEFAULT '',
    album TEXT NOT NULL DEFAULT '',
    title TEXT NOT NULL DEFAULT '',
    duration_ms INTEGER NOT NULL DEFAULT 0,
    last_seen_utc_ms INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS tracks_location_idx
    ON tracks(path, subsong, file_size, mtime_ticks);
CREATE TABLE IF NOT EXISTS play_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
    track_id TEXT NOT NULL REFERENCES tracks(track_id) ON DELETE RESTRICT,
    event_type INTEGER NOT NULL CHECK(event_type BETWEEN 1 AND 7),
    event_utc_ms INTEGER NOT NULL,
    position_ms INTEGER NOT NULL DEFAULT 0,
    duration_ms INTEGER NOT NULL DEFAULT 0,
    playlist_index INTEGER,
    item_index INTEGER
);
CREATE INDEX IF NOT EXISTS play_events_session_idx
    ON play_events(session_id, event_utc_ms, id);
CREATE INDEX IF NOT EXISTS play_events_track_idx
    ON play_events(track_id, event_utc_ms);
CREATE TABLE IF NOT EXISTS favorites (
    track_id TEXT PRIMARY KEY REFERENCES tracks(track_id) ON DELETE CASCADE,
    favorite INTEGER NOT NULL CHECK(favorite IN (0, 1)),
    updated_utc_ms INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS notes (
    track_id TEXT PRIMARY KEY REFERENCES tracks(track_id) ON DELETE CASCADE,
    body TEXT NOT NULL,
    updated_utc_ms INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS personas (
    persona_id TEXT PRIMARY KEY,
    display_name TEXT NOT NULL,
    profile TEXT NOT NULL DEFAULT '',
    updated_utc_ms INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS persona_aliases (
    alias TEXT PRIMARY KEY,
    persona_id TEXT NOT NULL REFERENCES personas(persona_id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS lyrics (
    track_id TEXT PRIMARY KEY REFERENCES tracks(track_id) ON DELETE CASCADE,
    source TEXT NOT NULL DEFAULT 'local',
    body TEXT NOT NULL,
    is_synced INTEGER NOT NULL DEFAULT 0 CHECK(is_synced IN (0, 1)),
    updated_utc_ms INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS art_index (
    asset_id TEXT PRIMARY KEY,
    track_id TEXT REFERENCES tracks(track_id) ON DELETE CASCADE,
    persona_id TEXT REFERENCES personas(persona_id) ON DELETE CASCADE,
    role TEXT NOT NULL,
    path TEXT NOT NULL,
    content_hash TEXT NOT NULL,
    selected_by_user INTEGER NOT NULL DEFAULT 0 CHECK(selected_by_user IN (0, 1)),
    updated_utc_ms INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS palettes (
    track_id TEXT PRIMARY KEY REFERENCES tracks(track_id) ON DELETE CASCADE,
    palette_blob BLOB NOT NULL,
    algorithm_version INTEGER NOT NULL,
    updated_utc_ms INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS analysis_manifests (
    track_id TEXT NOT NULL REFERENCES tracks(track_id) ON DELETE CASCADE,
    analyzer_version TEXT NOT NULL,
    state TEXT NOT NULL,
    manifest_json TEXT NOT NULL DEFAULT '{}',
    result_blob BLOB,
    updated_utc_ms INTEGER NOT NULL,
    PRIMARY KEY(track_id, analyzer_version)
);
INSERT OR IGNORE INTO schema_migrations(version, applied_utc_ms)
    VALUES(1, CAST((julianday('now') - 2440587.5) * 86400000 AS INTEGER));
PRAGMA user_version=1;
)sql", error);

        if (!migrated) {
            std::string ignored;
            execute("ROLLBACK;", ignored);
            return false;
        }
        if (!execute("COMMIT;", error)) {
            std::string ignored;
            execute("ROLLBACK;", ignored);
            return false;
        }
        current = 1;
    }

    if (current < 2) {
        if (!execute("BEGIN IMMEDIATE;", error)) return false;
        const bool migrated = execute(R"sql(
CREATE TABLE IF NOT EXISTS analysis_layers (
    track_id TEXT NOT NULL,
    analyzer_version TEXT NOT NULL,
    layer_name TEXT NOT NULL,
    resolution_ms INTEGER NOT NULL DEFAULT 0,
    codec TEXT NOT NULL,
    uncompressed_bytes INTEGER NOT NULL,
    stored_bytes INTEGER NOT NULL,
    payload BLOB NOT NULL,
    last_access_utc_ms INTEGER NOT NULL,
    PRIMARY KEY(track_id, analyzer_version, layer_name),
    FOREIGN KEY(track_id, analyzer_version)
        REFERENCES analysis_manifests(track_id, analyzer_version)
        ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS analysis_layers_lru_idx
    ON analysis_layers(last_access_utc_ms, track_id, analyzer_version);
INSERT OR IGNORE INTO schema_migrations(version, applied_utc_ms)
    VALUES(2, CAST((julianday('now') - 2440587.5) * 86400000 AS INTEGER));
PRAGMA user_version=2;
)sql", error);
        if (!migrated) {
            std::string ignored;
            execute("ROLLBACK;", ignored);
            return false;
        }
        if (!execute("COMMIT;", error)) {
            std::string ignored;
            execute("ROLLBACK;", ignored);
            return false;
        }
        current = 2;
    }

    if (current < 3) {
        if (!execute("BEGIN IMMEDIATE;", error)) return false;
        const bool migrated = execute(R"sql(
ALTER TABLE persona_aliases RENAME TO persona_aliases_v1;
CREATE TABLE persona_aliases (
    alias TEXT NOT NULL,
    alias_key TEXT PRIMARY KEY,
    persona_id TEXT NOT NULL REFERENCES personas(persona_id) ON DELETE CASCADE
);
INSERT INTO persona_aliases(alias, alias_key, persona_id)
    SELECT alias, alias, persona_id FROM persona_aliases_v1;
DROP TABLE persona_aliases_v1;
CREATE INDEX persona_aliases_persona_idx
    ON persona_aliases(persona_id, alias_key);
CREATE TABLE persona_relationships (
    source_persona_id TEXT NOT NULL
        REFERENCES personas(persona_id) ON DELETE CASCADE,
    target_persona_id TEXT NOT NULL
        REFERENCES personas(persona_id) ON DELETE CASCADE,
    relation TEXT NOT NULL,
    note TEXT NOT NULL DEFAULT '',
    PRIMARY KEY(source_persona_id, target_persona_id, relation)
);
CREATE INDEX persona_relationships_target_idx
    ON persona_relationships(target_persona_id, source_persona_id);
CREATE TABLE settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL,
    updated_utc_ms INTEGER NOT NULL
);
INSERT OR IGNORE INTO schema_migrations(version, applied_utc_ms)
    VALUES(3, CAST((julianday('now') - 2440587.5) * 86400000 AS INTEGER));
PRAGMA user_version=3;
)sql", error);
        if (!migrated) {
            std::string ignored;
            execute("ROLLBACK;", ignored);
            return false;
        }
        if (!execute("COMMIT;", error)) {
            std::string ignored;
            execute("ROLLBACK;", ignored);
            return false;
        }
        current = 3;
    }

    if (current < 4) {
        if (openingVersion > 0 && !backupDatabase(db_, path_, 4, error)) return false;
        if (!execute("BEGIN IMMEDIATE;", error)) return false;
        const bool migrated = execute(R"sql(
CREATE TABLE IF NOT EXISTS track_entities (
    entity_id TEXT PRIMARY KEY,
    canonical_track_id TEXT UNIQUE NOT NULL
        REFERENCES tracks(track_id) ON DELETE RESTRICT
);
CREATE TABLE IF NOT EXISTS track_revision_bindings (
    revision_track_id TEXT PRIMARY KEY
        REFERENCES tracks(track_id) ON DELETE CASCADE,
    entity_id TEXT NOT NULL
        REFERENCES track_entities(entity_id) ON DELETE RESTRICT
);
CREATE INDEX IF NOT EXISTS track_revision_bindings_entity_idx
    ON track_revision_bindings(entity_id, revision_track_id);
CREATE TABLE IF NOT EXISTS media_operation_journal (
    operation_id TEXT NOT NULL,
    item_id TEXT NOT NULL,
    entity_id TEXT,
    state TEXT NOT NULL,
    before_revision TEXT,
    after_revision TEXT,
    source_path TEXT NOT NULL DEFAULT '',
    destination_path TEXT NOT NULL DEFAULT '',
    backup_path TEXT NOT NULL DEFAULT '',
    expected_stat TEXT NOT NULL DEFAULT '',
    error TEXT NOT NULL DEFAULT '',
    updated_utc_ms INTEGER NOT NULL,
    PRIMARY KEY(operation_id, item_id),
    FOREIGN KEY(entity_id) REFERENCES track_entities(entity_id)
        ON DELETE RESTRICT
);
CREATE INDEX IF NOT EXISTS media_operation_journal_path_idx
    ON media_operation_journal(source_path, destination_path, state);
INSERT OR IGNORE INTO track_entities(entity_id, canonical_track_id)
    SELECT 'entity:v1:' || track_id, track_id FROM tracks ORDER BY track_id;
INSERT OR IGNORE INTO track_revision_bindings(revision_track_id, entity_id)
    SELECT track_id, 'entity:v1:' || track_id FROM tracks ORDER BY track_id;
)sql", error);
#if defined(GODEN_ENABLE_QA_HOOKS) && GODEN_ENABLE_QA_HOOKS
        wchar_t failpoint[2]{};
        const auto forceRollback = GetEnvironmentVariableW(
            L"GODEN_TEST_DB_MIGRATION_FAIL_AFTER_BACKFILL",
            failpoint, static_cast<DWORD>(std::size(failpoint))) != 0;
#else
        constexpr bool forceRollback = false;
#endif
        if (!migrated || forceRollback || !execute(R"sql(
INSERT OR IGNORE INTO schema_migrations(version, applied_utc_ms)
    VALUES(4, CAST((julianday('now') - 2440587.5) * 86400000 AS INTEGER));
PRAGMA user_version=4;
)sql", error)) {
            if (forceRollback) error = "schema v4 migration failpoint";
            std::string ignored;
            execute("ROLLBACK;", ignored);
            return false;
        }
        if (!execute("COMMIT;", error)) {
            std::string ignored;
            execute("ROLLBACK;", ignored);
            return false;
        }
        current = 4;
    }

    if (current < 5) {
        if (openingVersion > 0 &&
            !backupDatabase(db_, path_, 5, error)) return false;
        if (!execute("BEGIN IMMEDIATE;", error)) return false;
        const bool migrated = execute(R"sql(
CREATE TABLE IF NOT EXISTS lyrics_documents_v2 (
    document_id TEXT PRIMARY KEY,
    provider TEXT NOT NULL,
    provider_record_id TEXT NOT NULL DEFAULT '',
    provider_revision TEXT NOT NULL DEFAULT '',
    title TEXT NOT NULL DEFAULT '',
    artist TEXT NOT NULL DEFAULT '',
    album TEXT NOT NULL DEFAULT '',
    duration_ms INTEGER NOT NULL DEFAULT 0,
    plain_text TEXT NOT NULL DEFAULT '',
    synced_text TEXT NOT NULL DEFAULT '',
    instrumental INTEGER NOT NULL DEFAULT 0 CHECK(instrumental IN (0, 1)),
    text_hash TEXT NOT NULL DEFAULT '',
    timing_hash TEXT NOT NULL DEFAULT '',
    timing_status INTEGER NOT NULL DEFAULT 0,
    reason_codes TEXT NOT NULL DEFAULT '',
    source_display_name TEXT NOT NULL DEFAULT '',
    attribution TEXT NOT NULL DEFAULT '',
    source_url TEXT NOT NULL DEFAULT '',
    fetched_utc_ms INTEGER NOT NULL,
    expiry_utc_ms INTEGER NOT NULL,
    policy_version INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS lyrics_documents_v2_expiry_idx
    ON lyrics_documents_v2(expiry_utc_ms, fetched_utc_ms);
CREATE TABLE IF NOT EXISTS lyrics_bindings_v2 (
    entity_id TEXT NOT NULL,
    subsong INTEGER NOT NULL,
    media_revision TEXT NOT NULL,
    metadata_revision INTEGER NOT NULL,
    document_id TEXT NOT NULL REFERENCES lyrics_documents_v2(document_id)
        ON DELETE RESTRICT,
    state INTEGER NOT NULL,
    timing_status INTEGER NOT NULL,
    reason_codes TEXT NOT NULL DEFAULT '',
    updated_utc_ms INTEGER NOT NULL,
    PRIMARY KEY(entity_id, subsong, media_revision, metadata_revision)
);
CREATE INDEX IF NOT EXISTS lyrics_bindings_v2_document_idx
    ON lyrics_bindings_v2(document_id, entity_id);
CREATE TABLE IF NOT EXISTS lyrics_rejections_v2 (
    entity_id TEXT NOT NULL,
    subsong INTEGER NOT NULL,
    media_revision TEXT NOT NULL,
    document_id TEXT NOT NULL,
    text_hash TEXT NOT NULL DEFAULT '',
    timing_hash TEXT NOT NULL DEFAULT '',
    reason TEXT NOT NULL,
    rejected_utc_ms INTEGER NOT NULL,
    PRIMARY KEY(entity_id, subsong, media_revision, document_id)
);
CREATE TABLE IF NOT EXISTS lyrics_user_offsets_v2 (
    entity_id TEXT NOT NULL,
    subsong INTEGER NOT NULL,
    media_revision TEXT NOT NULL,
    text_hash TEXT NOT NULL,
    timing_hash TEXT NOT NULL,
    offset_ms INTEGER NOT NULL,
    updated_utc_ms INTEGER NOT NULL,
    PRIMARY KEY(entity_id, subsong, media_revision, text_hash, timing_hash)
);
INSERT OR IGNORE INTO lyrics_documents_v2(
    document_id, provider, provider_record_id, provider_revision,
    title, artist, album, duration_ms, plain_text, synced_text,
    instrumental, text_hash, timing_hash, timing_status, reason_codes,
    source_display_name, attribution, source_url,
    fetched_utc_ms, expiry_utc_ms, policy_version)
SELECT 'legacy:v1:' || l.track_id, l.source, l.track_id, 'legacy-v1',
       t.title, t.artist, t.album, t.duration_ms,
       l.body, CASE WHEN l.is_synced=1 THEN l.body ELSE '' END,
       0, '', '', CASE WHEN l.is_synced=1 THEN 1 ELSE 0 END,
       'legacy-import', 'Local', '', '',
       l.updated_utc_ms, 9223372036854775807, 1
FROM lyrics l JOIN tracks t ON t.track_id=l.track_id;
INSERT OR IGNORE INTO lyrics_bindings_v2(
    entity_id, subsong, media_revision, metadata_revision,
    document_id, state, timing_status, reason_codes, updated_utc_ms)
SELECT COALESCE(b.entity_id, 'entity:v1:' || l.track_id),
       t.subsong, l.track_id, 0, 'legacy:v1:' || l.track_id,
       2, CASE WHEN l.is_synced=1 THEN 1 ELSE 0 END,
       'legacy-import', l.updated_utc_ms
FROM lyrics l JOIN tracks t ON t.track_id=l.track_id
LEFT JOIN track_revision_bindings b ON b.revision_track_id=l.track_id;
INSERT OR IGNORE INTO schema_migrations(version, applied_utc_ms)
    VALUES(5, CAST((julianday('now') - 2440587.5) * 86400000 AS INTEGER));
PRAGMA user_version=5;
)sql", error);
        if (!migrated) {
            std::string ignored;
            execute("ROLLBACK;", ignored);
            return false;
        }
        if (!execute("COMMIT;", error)) {
            std::string ignored;
            execute("ROLLBACK;", ignored);
            return false;
        }
        current = 5;
    }

    if (current < 6) {
        if (openingVersion > 0 &&
            !backupDatabase(db_, path_, 6, error)) return false;
        if (!execute("BEGIN IMMEDIATE;", error)) return false;
        const bool migrated = execute(R"sql(
ALTER TABLE play_events ADD COLUMN event_uid TEXT NOT NULL DEFAULT '';
CREATE UNIQUE INDEX IF NOT EXISTS play_events_event_uid_idx
    ON play_events(event_uid) WHERE event_uid <> '';
INSERT OR IGNORE INTO schema_migrations(version, applied_utc_ms)
    VALUES(6, CAST((julianday('now') - 2440587.5) * 86400000 AS INTEGER));
PRAGMA user_version=6;
)sql", error);
        if (!migrated) {
            std::string ignored;
            execute("ROLLBACK;", ignored);
            return false;
        }
        if (!execute("COMMIT;", error)) {
            std::string ignored;
            execute("ROLLBACK;", ignored);
            return false;
        }
        current = 6;
    }

    schemaVersion_ = current;
    return true;
}

bool NekoDatabase::beginSession(
    const std::string& sessionId,
    std::int64_t startedUtcMs,
    const std::string& appVersion,
    std::string& error)
{
    Statement statement(db_, R"sql(
INSERT INTO sessions(session_id, started_utc_ms, app_version)
VALUES(?1, ?2, ?3)
ON CONFLICT(session_id) DO UPDATE SET
    started_utc_ms=excluded.started_utc_ms,
    ended_utc_ms=NULL,
    app_version=excluded.app_version;
)sql", error);
    return statement && statement.bindText(1, sessionId, error) &&
        statement.bindInt64(2, startedUtcMs, error) &&
        statement.bindText(3, appVersion, error) && statement.stepDone(error);
}

bool NekoDatabase::endSession(
    const std::string& sessionId,
    std::int64_t endedUtcMs,
    std::string& error)
{
    Statement statement(db_,
        "UPDATE sessions SET ended_utc_ms=?2 WHERE session_id=?1;", error);
    return statement && statement.bindText(1, sessionId, error) &&
        statement.bindInt64(2, endedUtcMs, error) && statement.stepDone(error);
}

bool NekoDatabase::recordPlayEvent(
    const PersistedTrack& track,
    const PersistedPlayEvent& event,
    std::string& error,
    bool* inserted)
{
    if (inserted) *inserted = false;
    if (track.trackId.empty() || event.trackId != track.trackId ||
        event.sessionId.empty() || event.eventUid.empty()) {
        error = "play event identity is incomplete";
        return false;
    }
    if (!execute("BEGIN IMMEDIATE;", error)) return false;

    bool success = upsertTrack(db_, track, error);

    if (success) {
        Statement eventStatement(db_, R"sql(
INSERT OR IGNORE INTO play_events(
    event_uid, session_id, track_id, event_type, event_utc_ms, position_ms,
    duration_ms, playlist_index, item_index)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9);
)sql", error);
        success = eventStatement && bindEvent(eventStatement, event, error) &&
            eventStatement.stepDone(error);
        if (success && inserted) *inserted = sqlite3_changes(db_) != 0;
    }
    if (!success) {
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return false;
    }
    if (!execute("COMMIT;", error)) {
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return false;
    }
    return true;
}

std::vector<HistoryEventRecord> NekoDatabase::readSessionEvents(
    const std::string& sessionId,
    std::size_t limit,
    std::string& error)
{
    std::vector<HistoryEventRecord> result;
    Statement statement(db_, R"sql(
SELECT e.id, e.event_uid, e.session_id, e.track_id, e.event_type, e.event_utc_ms,
       e.position_ms, e.duration_ms, t.artist, t.album, t.title, t.path, t.subsong
FROM play_events AS e
JOIN tracks AS t ON t.track_id=e.track_id
WHERE e.session_id=?1
ORDER BY e.event_utc_ms ASC, e.id ASC
LIMIT ?2;
)sql", error);
    if (!statement || !statement.bindText(1, sessionId, error) ||
        !statement.bindInt64(2, static_cast<std::int64_t>(
            std::min<std::size_t>(limit, static_cast<std::size_t>(
                std::numeric_limits<std::int32_t>::max()))), error)) {
        return result;
    }

    while (true) {
        const auto step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            error = databaseError(db_, "history query failed");
            result.clear();
            break;
        }
        HistoryEventRecord row;
        row.id = sqlite3_column_int64(statement.get(), 0);
        row.eventUid = columnText(statement.get(), 1);
        row.sessionId = columnText(statement.get(), 2);
        row.trackId = columnText(statement.get(), 3);
        row.type = static_cast<PlayEventType>(sqlite3_column_int(statement.get(), 4));
        row.utcMs = sqlite3_column_int64(statement.get(), 5);
        row.positionMs = sqlite3_column_int64(statement.get(), 6);
        row.durationMs = sqlite3_column_int64(statement.get(), 7);
        row.artist = columnText(statement.get(), 8);
        row.album = columnText(statement.get(), 9);
        row.title = columnText(statement.get(), 10);
        row.path = columnText(statement.get(), 11);
        row.subsong = static_cast<std::uint32_t>(sqlite3_column_int64(statement.get(), 12));
        result.push_back(std::move(row));
    }
    return result;
}

bool NekoDatabase::setFavorite(const PersistedTrack& track, bool favorite,
    std::int64_t updatedUtcMs, std::string& error)
{
    error.clear();
    if (track.trackId.empty()) {
        error = "favorite track identity is incomplete";
        return false;
    }
    if (!execute("BEGIN IMMEDIATE;", error)) return false;
    bool success = upsertTrack(db_, track, error);
    TrackIdentityResolution identity;
    if (success) success = resolveTrackIdentity(track.trackId, identity, error) &&
        identity.found;
    if (success) {
        Statement statement(db_, R"sql(
INSERT INTO favorites(track_id, favorite, updated_utc_ms)
VALUES(?1, ?2, ?3)
ON CONFLICT(track_id) DO UPDATE SET
    favorite=excluded.favorite,
    updated_utc_ms=excluded.updated_utc_ms;
)sql", error);
        success = statement && statement.bindText(
                1, identity.canonicalTrackId, error) &&
            statement.bindInt64(2, favorite ? 1 : 0, error) &&
            statement.bindInt64(3,
                updatedUtcMs > 0 ? updatedUtcMs : utcNowMs(), error) &&
            statement.stepDone(error);
    }
    if (!success) {
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return false;
    }
    if (!execute("COMMIT;", error)) {
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return false;
    }
    return true;
}

bool NekoDatabase::setNote(const PersistedTrack& track,
    const std::string& note, std::int64_t updatedUtcMs, std::string& error)
{
    error.clear();
    if (track.trackId.empty()) {
        error = "note track identity is incomplete";
        return false;
    }
    constexpr std::size_t kMaximumNoteBytes = 64U * 1024U;
    if (note.size() > kMaximumNoteBytes) {
        error = "note exceeds 64 KiB";
        return false;
    }
    if (!execute("BEGIN IMMEDIATE;", error)) return false;
    bool success = upsertTrack(db_, track, error);
    TrackIdentityResolution identity;
    if (success) success = resolveTrackIdentity(track.trackId, identity, error) &&
        identity.found;
    // An empty body is a timestamped clear. Deleting the row would let an older
    // note reappear when a temporary revision is reunited with its entity.
    if (success) {
        Statement statement(db_, R"sql(
INSERT INTO notes(track_id, body, updated_utc_ms)
VALUES(?1, ?2, ?3)
ON CONFLICT(track_id) DO UPDATE SET
    body=excluded.body,
    updated_utc_ms=excluded.updated_utc_ms;
)sql", error);
        success = statement && statement.bindText(
                1, identity.canonicalTrackId, error) &&
            statement.bindText(2, note, error) &&
            statement.bindInt64(3,
                updatedUtcMs > 0 ? updatedUtcMs : utcNowMs(), error) &&
            statement.stepDone(error);
    }
    if (!success) {
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return false;
    }
    if (!execute("COMMIT;", error)) {
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return false;
    }
    return true;
}

bool NekoDatabase::readTrackUserState(const std::string& trackId,
    PersistedTrackUserState& state, std::string& error)
{
    state = {};
    error.clear();
    if (trackId.empty()) {
        error = "user-state track identity is incomplete";
        return false;
    }
    Statement statement(db_, R"sql(
SELECT b.revision_track_id, e.entity_id, e.canonical_track_id,
       COALESCE(f.favorite, 0),
       COALESCE(n.body, ''),
       COALESCE(f.updated_utc_ms, 0),
       COALESCE(n.updated_utc_ms, 0)
FROM track_revision_bindings AS b
JOIN track_entities AS e ON e.entity_id=b.entity_id
LEFT JOIN favorites AS f ON f.track_id=e.canonical_track_id
LEFT JOIN notes AS n ON n.track_id=e.canonical_track_id
WHERE b.revision_track_id=?1;
)sql", error);
    if (!statement || !statement.bindText(1, trackId, error)) return false;
    const auto step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return true;
    if (step != SQLITE_ROW) {
        error = databaseError(db_, "user-state query failed");
        return false;
    }
    state.found = true;
    state.trackId = columnText(statement.get(), 0);
    state.currentRevisionTrackId = state.trackId;
    state.entityId = columnText(statement.get(), 1);
    state.canonicalTrackId = columnText(statement.get(), 2);
    state.favorite = sqlite3_column_int(statement.get(), 3) != 0;
    state.note = columnText(statement.get(), 4);
    state.favoriteUpdatedUtcMs = sqlite3_column_int64(statement.get(), 5);
    state.noteUpdatedUtcMs = sqlite3_column_int64(statement.get(), 6);
    return true;
}

bool NekoDatabase::resolveTrackIdentity(const std::string& revisionTrackId,
    TrackIdentityResolution& identity, std::string& error)
{
    identity = {};
    error.clear();
    if (revisionTrackId.empty()) {
        error = "revision identity is incomplete";
        return false;
    }
    Statement statement(db_, R"sql(
SELECT b.revision_track_id, b.entity_id, e.canonical_track_id
FROM track_revision_bindings AS b
JOIN track_entities AS e ON e.entity_id=b.entity_id
WHERE b.revision_track_id=?1;
)sql", error);
    if (!statement || !statement.bindText(1, revisionTrackId, error)) return false;
    const auto step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return true;
    if (step != SQLITE_ROW) {
        error = databaseError(db_, "identity query failed");
        return false;
    }
    identity.found = true;
    identity.revisionTrackId = columnText(statement.get(), 0);
    identity.entityId = columnText(statement.get(), 1);
    identity.canonicalTrackId = columnText(statement.get(), 2);
    return true;
}

bool NekoDatabase::bindKnownRevision(const PersistedTrack& revision,
    const std::string& entityId, std::string& error)
{
    const KnownRevisionBinding binding{revision, entityId};
    return bindKnownRevisions(std::span(&binding, 1), error);
}

bool NekoDatabase::bindKnownRevisions(
    std::span<const KnownRevisionBinding> bindings, std::string& error)
{
    error.clear();
    if (bindings.empty()) return true;
    std::unordered_map<std::string, std::string> destinations;
    std::unordered_set<std::string> targets;
    for (const auto& binding : bindings) {
        const auto [existing, inserted] = destinations.emplace(
            binding.revision.trackId, binding.entityId);
        if (!inserted && existing->second != binding.entityId) {
            error = "batch assigns one revision to different entities";
            return false;
        }
        targets.insert(binding.entityId);
    }
    for (const auto& [revision, entity] : destinations) {
        const auto defaultEntity = "entity:v1:" + revision;
        if (entity != defaultEntity && targets.contains(defaultEntity)) {
            error = "batch contains dependent revision bindings";
            return false;
        }
    }
    if (!execute("BEGIN IMMEDIATE;", error)) return false;
    bool success = true;
    for (const auto& binding : bindings) {
        if (!bindKnownRevisionInTransaction(binding.revision, binding.entityId, error)) {
            success = false;
            break;
        }
    }
    if (!success || !execute("COMMIT;", error)) {
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return false;
    }
    return true;
}

bool NekoDatabase::bindKnownRevisionInTransaction(const PersistedTrack& revision,
    const std::string& entityId, std::string& error)
{
    error.clear();
    if (revision.trackId.empty() || entityId.empty()) {
        error = "known revision binding is incomplete";
        return false;
    }
    if (!upsertTrack(db_, revision, error)) return false;
    TrackIdentityResolution source;
    if (!resolveTrackIdentity(revision.trackId, source, error) || !source.found) {
        if (error.empty()) error = "revision entity does not exist";
        return false;
    }
    if (source.entityId == entityId) return true;
    std::string canonical;
    {
        Statement target(db_,
            "SELECT canonical_track_id FROM track_entities WHERE entity_id=?1;", error);
        if (!target || !target.bindText(1, entityId, error)) return false;
        const auto result = sqlite3_step(target.get());
        if (result != SQLITE_ROW) {
            error = result == SQLITE_DONE ? "known entity does not exist" :
                databaseError(db_, "known entity lookup failed");
            return false;
        }
        canonical = columnText(target.get(), 0);
    }
    // Only absorb the default entity created for this unassociated revision.
    // Moving one alias out of an established entity would split personal state.
    if (source.entityId != "entity:v1:" + revision.trackId ||
            source.canonicalTrackId != revision.trackId) {
        error = "revision already belongs to a different known entity";
        return false;
    }
    {
        Statement aliases(db_, "SELECT 1 FROM track_revision_bindings "
            "WHERE entity_id=?1 AND revision_track_id<>?2 LIMIT 1;", error);
        if (!aliases || !aliases.bindText(1, source.entityId, error) ||
                !aliases.bindText(2, revision.trackId, error)) return false;
        const auto result = sqlite3_step(aliases.get());
        if (result != SQLITE_DONE) {
            error = result == SQLITE_ROW ? "revision entity has other known revisions" :
                databaseError(db_, "revision aliases lookup failed");
            return false;
        }
    }
    // Merge each field independently. Equal timestamps retain the established
    // destination; retrying a completed binding cannot reapply orphan rows.
    for (const auto* sql : {
        R"sql(INSERT INTO favorites(track_id, favorite, updated_utc_ms)
SELECT ?1, favorite, updated_utc_ms FROM favorites WHERE track_id=?2
ON CONFLICT(track_id) DO UPDATE SET favorite=excluded.favorite,
    updated_utc_ms=excluded.updated_utc_ms
WHERE excluded.updated_utc_ms>favorites.updated_utc_ms;)sql",
        R"sql(INSERT INTO notes(track_id, body, updated_utc_ms)
SELECT ?1, body, updated_utc_ms FROM notes WHERE track_id=?2
ON CONFLICT(track_id) DO UPDATE SET body=excluded.body,
    updated_utc_ms=excluded.updated_utc_ms
WHERE excluded.updated_utc_ms>notes.updated_utc_ms;)sql"}) {
        Statement merge(db_, sql, error);
        if (!merge || !merge.bindText(1, canonical, error) ||
                !merge.bindText(2, source.canonicalTrackId, error) ||
                !merge.stepDone(error)) return false;
    }
    Statement bind(db_,
        "UPDATE track_revision_bindings SET entity_id=?2 WHERE revision_track_id=?1;", error);
    if (!bind || !bind.bindText(1, revision.trackId, error) ||
            !bind.bindText(2, entityId, error) || !bind.stepDone(error)) return false;
    for (const auto* sql : {"DELETE FROM favorites WHERE track_id=?1;",
            "DELETE FROM notes WHERE track_id=?1;"}) {
        Statement consumed(db_, sql, error);
        if (!consumed || !consumed.bindText(1, source.canonicalTrackId, error) ||
                !consumed.stepDone(error)) return false;
    }
    // Journal references retain their original entity for recovery/audit.
    Statement cleanup(db_, R"sql(
DELETE FROM track_entities
WHERE entity_id=?1 AND canonical_track_id=?2
  AND NOT EXISTS(SELECT 1 FROM track_revision_bindings
                 WHERE entity_id=track_entities.entity_id)
  AND NOT EXISTS(SELECT 1 FROM media_operation_journal
                 WHERE entity_id=track_entities.entity_id);
)sql", error);
    return cleanup && cleanup.bindText(1, source.entityId, error) &&
        cleanup.bindText(2, revision.trackId, error) && cleanup.stepDone(error);
}

bool NekoDatabase::upsertPersona(
    const PersistedPersona& persona, std::string& error)
{
    error.clear();
    constexpr std::size_t kMaximumEntries = 128U;
    if (persona.personaId.empty() || persona.displayName.empty() ||
        persona.displayName.size() > 4096U ||
        persona.profile.size() > kDatabaseMaximumTextBytes ||
        persona.aliases.empty() || persona.aliases.size() > kMaximumEntries ||
        persona.relationships.size() > kMaximumEntries) {
        error = "persona payload is incomplete or exceeds its limits";
        return false;
    }
    for (const auto& alias : persona.aliases) {
        if (alias.display.empty() || alias.key.empty() ||
            alias.display.size() > 4096U || alias.key.size() > 4096U) {
            error = "persona alias is incomplete or exceeds its limits";
            return false;
        }
    }
    for (const auto& relationship : persona.relationships) {
        if (relationship.targetPersonaId.empty() ||
            relationship.targetDisplayName.empty() ||
            relationship.relation.empty() ||
            relationship.targetDisplayName.size() > 4096U ||
            relationship.targetAliasKey.size() > 4096U ||
            relationship.relation.size() > 4096U ||
            relationship.note.size() > 64U * 1024U) {
            error = "persona relationship is incomplete or exceeds its limits";
            return false;
        }
    }

    if (!execute("BEGIN IMMEDIATE;", error)) return false;
    bool success = true;
    const auto updated = persona.updatedUtcMs > 0
        ? persona.updatedUtcMs : utcNowMs();
    {
        Statement statement(db_, R"sql(
INSERT INTO personas(persona_id, display_name, profile, updated_utc_ms)
VALUES(?1, ?2, ?3, ?4)
ON CONFLICT(persona_id) DO UPDATE SET
    display_name=excluded.display_name,
    profile=excluded.profile,
    updated_utc_ms=excluded.updated_utc_ms;
)sql", error);
        success = statement && statement.bindText(1, persona.personaId, error) &&
            statement.bindText(2, persona.displayName, error) &&
            statement.bindText(3, persona.profile, error) &&
            statement.bindInt64(4, updated, error) && statement.stepDone(error);
    }
    if (success) {
        Statement statement(db_,
            "DELETE FROM persona_aliases WHERE persona_id=?1;", error);
        success = statement && statement.bindText(1, persona.personaId, error) &&
            statement.stepDone(error);
    }
    for (const auto& alias : persona.aliases) {
        if (!success) break;
        Statement statement(db_, R"sql(
INSERT INTO persona_aliases(alias, alias_key, persona_id)
VALUES(?1, ?2, ?3);
)sql", error);
        success = statement && statement.bindText(1, alias.display, error) &&
            statement.bindText(2, alias.key, error) &&
            statement.bindText(3, persona.personaId, error) &&
            statement.stepDone(error);
    }
    if (success) {
        Statement statement(db_,
            "DELETE FROM persona_relationships WHERE source_persona_id=?1;", error);
        success = statement && statement.bindText(1, persona.personaId, error) &&
            statement.stepDone(error);
    }
    for (const auto& relationship : persona.relationships) {
        if (!success) break;
        auto targetId = relationship.targetPersonaId;
        if (!relationship.targetAliasKey.empty()) {
            Statement lookup(db_,
                "SELECT persona_id FROM persona_aliases WHERE alias_key=?1;", error);
            success = lookup && lookup.bindText(
                1, relationship.targetAliasKey, error);
            if (!success) break;
            const auto step = sqlite3_step(lookup.get());
            if (step == SQLITE_ROW) targetId = columnText(lookup.get(), 0);
            else if (step != SQLITE_DONE) {
                error = databaseError(db_, "persona relationship lookup failed");
                success = false;
                break;
            }
        }
        {
            Statement target(db_, R"sql(
INSERT INTO personas(persona_id, display_name, profile, updated_utc_ms)
VALUES(?1, ?2, '', ?3)
ON CONFLICT(persona_id) DO NOTHING;
)sql", error);
            success = target && target.bindText(1, targetId, error) &&
                target.bindText(2, relationship.targetDisplayName, error) &&
                target.bindInt64(3, updated, error) && target.stepDone(error);
        }
        if (success && !relationship.targetAliasKey.empty()) {
            Statement alias(db_, R"sql(
INSERT INTO persona_aliases(alias, alias_key, persona_id)
VALUES(?1, ?2, ?3)
ON CONFLICT(alias_key) DO NOTHING;
)sql", error);
            success = alias && alias.bindText(
                1, relationship.targetDisplayName, error) &&
                alias.bindText(2, relationship.targetAliasKey, error) &&
                alias.bindText(3, targetId, error) && alias.stepDone(error);
        }
        if (success) {
            Statement relation(db_, R"sql(
INSERT INTO persona_relationships(
    source_persona_id, target_persona_id, relation, note)
VALUES(?1, ?2, ?3, ?4);
)sql", error);
            success = relation && relation.bindText(1, persona.personaId, error) &&
                relation.bindText(2, targetId, error) &&
                relation.bindText(3, relationship.relation, error) &&
                relation.bindText(4, relationship.note, error) &&
                relation.stepDone(error);
        }
    }

    if (!success) {
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return false;
    }
    if (!execute("COMMIT;", error)) {
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return false;
    }
    return true;
}

bool NekoDatabase::readPersonaByAliasKey(
    const std::string& aliasKey, PersistedPersona& persona, std::string& error)
{
    persona = {};
    error.clear();
    if (aliasKey.empty()) {
        error = "persona alias key is empty";
        return false;
    }
    Statement statement(db_,
        "SELECT persona_id FROM persona_aliases WHERE alias_key=?1;", error);
    if (!statement || !statement.bindText(1, aliasKey, error)) return false;
    const auto step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return true;
    if (step != SQLITE_ROW) {
        error = databaseError(db_, "persona alias query failed");
        return false;
    }
    return readPersonaById(columnText(statement.get(), 0), persona, error);
}

bool NekoDatabase::readPersonaById(
    const std::string& personaId, PersistedPersona& persona, std::string& error)
{
    persona = {};
    error.clear();
    if (personaId.empty()) {
        error = "persona identity is empty";
        return false;
    }
    Statement base(db_, R"sql(
SELECT persona_id, display_name, profile, updated_utc_ms
FROM personas WHERE persona_id=?1;
)sql", error);
    if (!base || !base.bindText(1, personaId, error)) return false;
    const auto baseStep = sqlite3_step(base.get());
    if (baseStep == SQLITE_DONE) return true;
    if (baseStep != SQLITE_ROW) {
        error = databaseError(db_, "persona query failed");
        return false;
    }
    persona.found = true;
    persona.personaId = columnText(base.get(), 0);
    persona.displayName = columnText(base.get(), 1);
    persona.profile = columnText(base.get(), 2);
    persona.updatedUtcMs = sqlite3_column_int64(base.get(), 3);

    Statement aliases(db_, R"sql(
SELECT alias, alias_key FROM persona_aliases
WHERE persona_id=?1 ORDER BY alias_key;
)sql", error);
    if (!aliases || !aliases.bindText(1, personaId, error)) return false;
    while (true) {
        const auto step = sqlite3_step(aliases.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            error = databaseError(db_, "persona aliases query failed");
            persona = {};
            return false;
        }
        persona.aliases.push_back({
            columnText(aliases.get(), 0), columnText(aliases.get(), 1)});
    }

    Statement relationships(db_, R"sql(
SELECT r.target_persona_id,
       p.display_name,
       COALESCE((SELECT a.alias_key FROM persona_aliases AS a
                 WHERE a.persona_id=r.target_persona_id
                 ORDER BY a.alias_key LIMIT 1), ''),
       r.relation,
       r.note
FROM persona_relationships AS r
JOIN personas AS p ON p.persona_id=r.target_persona_id
WHERE r.source_persona_id=?1
ORDER BY r.relation, p.display_name, r.target_persona_id;
)sql", error);
    if (!relationships || !relationships.bindText(1, personaId, error)) return false;
    while (true) {
        const auto step = sqlite3_step(relationships.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            error = databaseError(db_, "persona relationships query failed");
            persona = {};
            return false;
        }
        persona.relationships.push_back({
            columnText(relationships.get(), 0),
            columnText(relationships.get(), 1),
            columnText(relationships.get(), 2),
            columnText(relationships.get(), 3),
            columnText(relationships.get(), 4)});
    }
    return true;
}

bool NekoDatabase::upsertArtAsset(
    const PersistedArtAsset& asset, std::string& error)
{
    error.clear();
    if (db_ == nullptr) {
        error = "database is not open";
        return false;
    }
    if (asset.assetId.empty() || asset.personaId.empty() || asset.role.empty() ||
        asset.path.empty() || asset.contentHash.empty() ||
        asset.assetId.size() > 4096U || asset.personaId.size() > 4096U ||
        asset.role.size() > 256U || asset.path.size() > kDatabaseMaximumTextBytes ||
        asset.contentHash.size() > 4096U) {
        error = "art asset payload is incomplete or exceeds its limits";
        return false;
    }
    Statement statement(db_, R"sql(
INSERT INTO art_index(
    asset_id, track_id, persona_id, role, path, content_hash,
    selected_by_user, updated_utc_ms)
VALUES(?1, NULL, ?2, ?3, ?4, ?5, ?6, ?7)
ON CONFLICT(asset_id) DO UPDATE SET
    persona_id=excluded.persona_id,
    role=excluded.role,
    path=excluded.path,
    content_hash=excluded.content_hash,
    selected_by_user=MAX(art_index.selected_by_user, excluded.selected_by_user),
    updated_utc_ms=excluded.updated_utc_ms;
)sql", error);
    const auto updated = asset.updatedUtcMs > 0
        ? asset.updatedUtcMs : utcNowMs();
    return statement && statement.bindText(1, asset.assetId, error) &&
        statement.bindText(2, asset.personaId, error) &&
        statement.bindText(3, asset.role, error) &&
        statement.bindText(4, asset.path, error) &&
        statement.bindText(5, asset.contentHash, error) &&
        statement.bindInt64(6, asset.selectedByUser ? 1 : 0, error) &&
        statement.bindInt64(7, updated, error) && statement.stepDone(error);
}

bool NekoDatabase::deleteUnselectedArtAssetsWithPathPrefix(
    const std::string& pathPrefix, std::string& error)
{
    error.clear();
    if (db_ == nullptr) {
        error = "database is not open";
        return false;
    }
    if (pathPrefix.empty() || pathPrefix.size() > kDatabaseMaximumTextBytes) {
        error = "art asset path prefix is invalid";
        return false;
    }
    Statement statement(db_, R"sql(
DELETE FROM art_index
WHERE selected_by_user=0
  AND substr(path, 1, length(?1))=?1;
)sql", error);
    return statement && statement.bindText(1, pathPrefix, error) &&
        statement.stepDone(error);
}

bool NekoDatabase::setSetting(
    const std::string& key, const std::string& value,
    std::int64_t updatedUtcMs, std::string& error)
{
    error.clear();
    if (db_ == nullptr) {
        error = "database is not open";
        return false;
    }
    if (key.empty() || key.size() > 4096U ||
        value.size() > kDatabaseMaximumTextBytes) {
        error = "setting payload is invalid or exceeds its limits";
        return false;
    }
    Statement statement(db_, R"sql(
INSERT INTO settings(key, value, updated_utc_ms)
VALUES(?1, ?2, ?3)
ON CONFLICT(key) DO UPDATE SET
    value=excluded.value,
    updated_utc_ms=excluded.updated_utc_ms;
)sql", error);
    return statement && statement.bindText(1, key, error) &&
        statement.bindText(2, value, error) &&
        statement.bindInt64(3, updatedUtcMs > 0 ? updatedUtcMs : utcNowMs(), error) &&
        statement.stepDone(error);
}

bool NekoDatabase::readArtAssetsForPersona(
    const std::string& personaId, std::vector<PersistedArtAsset>& assets,
    std::string& error)
{
    assets.clear();
    error.clear();
    if (db_ == nullptr) {
        error = "database is not open";
        return false;
    }
    if (personaId.empty()) {
        error = "persona identity is empty";
        return false;
    }
    Statement statement(db_, R"sql(
SELECT asset_id, persona_id, role, path, content_hash,
       selected_by_user, updated_utc_ms
FROM art_index
WHERE persona_id=?1
ORDER BY selected_by_user DESC, role, path, asset_id;
)sql", error);
    if (!statement || !statement.bindText(1, personaId, error)) return false;
    while (true) {
        const auto result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) return true;
        if (result != SQLITE_ROW) {
            error = databaseError(db_, "persona art query failed");
            assets.clear();
            return false;
        }
        assets.push_back({
            columnText(statement.get(), 0),
            columnText(statement.get(), 1),
            columnText(statement.get(), 2),
            columnText(statement.get(), 3),
            columnText(statement.get(), 4),
            sqlite3_column_int(statement.get(), 5) != 0,
            sqlite3_column_int64(statement.get(), 6),
        });
    }
}

bool NekoDatabase::readSetting(
    const std::string& key, std::string& value, bool& found, std::string& error)
{
    value.clear();
    found = false;
    error.clear();
    if (db_ == nullptr) {
        error = "database is not open";
        return false;
    }
    if (key.empty()) {
        error = "setting key is empty";
        return false;
    }
    Statement statement(db_, "SELECT value FROM settings WHERE key=?1;", error);
    if (!statement || !statement.bindText(1, key, error)) return false;
    const auto result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) return true;
    if (result != SQLITE_ROW) {
        error = databaseError(db_, "setting query failed");
        return false;
    }
    found = true;
    value = columnText(statement.get(), 0);
    return true;
}

bool NekoDatabase::storeAnalysis(const PersistedTrack& track,
    const PersistedAnalysis& analysis, std::uint64_t cacheLimitBytes,
    std::string& error)
{
    PersistedAnalysisBundle bundle;
    bundle.trackId = analysis.trackId;
    bundle.analyzerVersion = analysis.analyzerVersion;
    bundle.state = analysis.state;
    bundle.manifestJson = analysis.manifestJson;
    bundle.updatedUtcMs = analysis.updatedUtcMs;
    bundle.lastAccessUtcMs = analysis.lastAccessUtcMs;
    bundle.layers.push_back(
        {"result-json", 0, analysis.resultJson});
    bundle.layers.push_back(
        {"waveform-100ms", 100, analysis.waveform100ms});
    return storeAnalysisBundle(track, bundle, cacheLimitBytes, error);
}

bool NekoDatabase::readAnalysis(const std::string& trackId,
    const std::string& analyzerVersion, PersistedAnalysis& analysis,
    std::string& error)
{
    analysis = {};
    PersistedAnalysisBundle bundle;
    if (!readAnalysisBundle(trackId, analyzerVersion, bundle, error)) return false;
    if (!bundle.found) return true;
    const PersistedAnalysisLayer* result = nullptr;
    const PersistedAnalysisLayer* waveform = nullptr;
    for (const auto& layer : bundle.layers) {
        if (layer.name == "result-json") result = &layer;
        else if (layer.name == "waveform-100ms") waveform = &layer;
    }
    if (result == nullptr || waveform == nullptr) return true;
    analysis.found = true;
    analysis.trackId = bundle.trackId;
    analysis.analyzerVersion = bundle.analyzerVersion;
    analysis.state = bundle.state;
    analysis.manifestJson = bundle.manifestJson;
    analysis.resultJson = result->data;
    analysis.waveform100ms = waveform->data;
    analysis.updatedUtcMs = bundle.updatedUtcMs;
    analysis.lastAccessUtcMs = bundle.lastAccessUtcMs;
    return true;
}

bool NekoDatabase::storeAnalysisBundle(const PersistedTrack& track,
    const PersistedAnalysisBundle& bundle, std::uint64_t cacheLimitBytes,
    std::string& error)
{
    error.clear();
    if (track.trackId.empty() || bundle.analyzerVersion.empty() ||
        (!bundle.trackId.empty() && bundle.trackId != track.trackId) ||
        bundle.state != "complete" || bundle.layers.empty()) {
        error = "analysis bundle identity or state is incomplete";
        return false;
    }

    struct CompressedLayer {
        const PersistedAnalysisLayer* source = nullptr;
        std::vector<std::byte> payload;
    };
    std::vector<CompressedLayer> compressed;
    compressed.reserve(bundle.layers.size());
    std::unordered_set<std::string> names;
    for (const auto& layer : bundle.layers) {
        const auto validName = !layer.name.empty() && layer.name.size() <= 64 &&
            std::all_of(layer.name.begin(), layer.name.end(), [](char value) {
                const auto byte = static_cast<unsigned char>(value);
                return std::isalnum(byte) != 0 || value == '-' || value == '_';
            });
        if (!validName || layer.resolutionMs < 0 ||
            !names.insert(layer.name).second) {
            error = "analysis bundle contains an invalid or duplicate layer";
            return false;
        }
        compressed.push_back({&layer, {}});
        if (!compressZstdBlob(layer.data, compressed.back().payload, error)) {
            return false;
        }
    }
    const auto updated = bundle.updatedUtcMs > 0
        ? bundle.updatedUtcMs : utcNowMs();
    const auto accessed = bundle.lastAccessUtcMs > 0
        ? bundle.lastAccessUtcMs : updated;

    if (!execute("BEGIN IMMEDIATE;", error)) return false;
    bool success = upsertTrack(db_, track, error);

    if (success) {
        Statement manifest(db_, R"sql(
INSERT INTO analysis_manifests(
    track_id, analyzer_version, state, manifest_json, result_blob, updated_utc_ms)
VALUES(?1, ?2, ?3, ?4, NULL, ?5)
ON CONFLICT(track_id, analyzer_version) DO UPDATE SET
    state=excluded.state,
    manifest_json=excluded.manifest_json,
    result_blob=NULL,
    updated_utc_ms=excluded.updated_utc_ms;
)sql", error);
        success = manifest && manifest.bindText(1, track.trackId, error) &&
            manifest.bindText(2, bundle.analyzerVersion, error) &&
            manifest.bindText(3, bundle.state, error) &&
            manifest.bindText(4, bundle.manifestJson, error) &&
            manifest.bindInt64(5, updated, error) && manifest.stepDone(error);
    }
    if (success) {
        Statement clearLayers(db_, R"sql(
DELETE FROM analysis_layers
WHERE track_id=?1 AND analyzer_version=?2;
)sql", error);
        success = clearLayers && clearLayers.bindText(1, track.trackId, error) &&
            clearLayers.bindText(2, bundle.analyzerVersion, error) &&
            clearLayers.stepDone(error);
    }
    for (const auto& layerData : compressed) {
        if (!success) break;
        const auto& layerSource = *layerData.source;
        Statement layer(db_, R"sql(
INSERT INTO analysis_layers(
    track_id, analyzer_version, layer_name, resolution_ms, codec,
    uncompressed_bytes, stored_bytes, payload, last_access_utc_ms)
VALUES(?1, ?2, ?3, ?4, 'zstd-envelope-v1', ?5, ?6, ?7, ?8);
)sql", error);
        success = layer && layer.bindText(1, track.trackId, error) &&
            layer.bindText(2, bundle.analyzerVersion, error) &&
            layer.bindText(3, layerSource.name, error) &&
            layer.bindInt64(4, layerSource.resolutionMs, error) &&
            layer.bindInt64(5,
                static_cast<std::int64_t>(layerSource.data.size()), error) &&
            layer.bindInt64(6,
                static_cast<std::int64_t>(layerData.payload.size()), error) &&
            layer.bindBlob(7, layerData.payload, error) &&
            layer.bindInt64(8, accessed, error) && layer.stepDone(error);
    }

    if (!success) {
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return false;
    }
    if (!execute("COMMIT;", error)) {
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return false;
    }
    return pruneAnalysisCache(cacheLimitBytes, track.trackId,
        bundle.analyzerVersion, error);
}

bool NekoDatabase::readAnalysisBundle(const std::string& trackId,
    const std::string& analyzerVersion, PersistedAnalysisBundle& bundle,
    std::string& error)
{
    bundle = {};
    error.clear();
    if (trackId.empty() || analyzerVersion.empty()) {
        error = "analysis bundle lookup identity is incomplete";
        return false;
    }

    {
        Statement manifest(db_, R"sql(
SELECT state, manifest_json, updated_utc_ms
FROM analysis_manifests
WHERE track_id=?1 AND analyzer_version=?2 AND state='complete';
)sql", error);
        if (!manifest || !manifest.bindText(1, trackId, error) ||
            !manifest.bindText(2, analyzerVersion, error)) return false;
        const auto step = sqlite3_step(manifest.get());
        if (step == SQLITE_DONE) return true;
        if (step != SQLITE_ROW) {
            error = databaseError(db_, "analysis bundle manifest query failed");
            return false;
        }
        bundle.state = columnText(manifest.get(), 0);
        bundle.manifestJson = columnText(manifest.get(), 1);
        bundle.updatedUtcMs = sqlite3_column_int64(manifest.get(), 2);
    }

    bool corrupt = false;
    {
        Statement layers(db_, R"sql(
SELECT layer_name, resolution_ms, codec, payload, last_access_utc_ms
FROM analysis_layers
WHERE track_id=?1 AND analyzer_version=?2
ORDER BY layer_name;
)sql", error);
        if (!layers || !layers.bindText(1, trackId, error) ||
            !layers.bindText(2, analyzerVersion, error)) return false;
        while (true) {
            const auto step = sqlite3_step(layers.get());
            if (step == SQLITE_DONE) break;
            if (step != SQLITE_ROW) {
                error = databaseError(db_, "analysis bundle layer query failed");
                return false;
            }
            PersistedAnalysisLayer layer;
            layer.name = columnText(layers.get(), 0);
            layer.resolutionMs = sqlite3_column_int64(layers.get(), 1);
            const std::string codec = columnText(layers.get(), 2);
            const auto compressed = columnBlob(layers.get(), 3);
            bundle.lastAccessUtcMs = std::max(bundle.lastAccessUtcMs,
                sqlite3_column_int64(layers.get(), 4));
            if (codec != "zstd-envelope-v1" ||
                !decompressZstdBlob(compressed, layer.data, error)) {
                corrupt = true;
                if (error.empty()) error = "analysis bundle layer codec is unsupported";
                break;
            }
            bundle.layers.push_back(std::move(layer));
        }
    }
    if (corrupt) {
        std::string ignored;
        Statement remove(db_, R"sql(
DELETE FROM analysis_manifests
WHERE track_id=?1 AND analyzer_version=?2;
)sql", ignored);
        if (remove) {
            remove.bindText(1, trackId, ignored);
            remove.bindText(2, analyzerVersion, ignored);
            remove.stepDone(ignored);
        }
        bundle = {};
        return false;
    }
    if (bundle.layers.empty()) return true;

    const auto accessed = utcNowMs();
    Statement touch(db_, R"sql(
UPDATE analysis_layers SET last_access_utc_ms=?3
WHERE track_id=?1 AND analyzer_version=?2
  AND last_access_utc_ms < (?3 - 30000);
)sql", error);
    if (!touch || !touch.bindText(1, trackId, error) ||
        !touch.bindText(2, analyzerVersion, error) ||
        !touch.bindInt64(3, accessed, error) || !touch.stepDone(error)) {
        return false;
    }
    bundle.found = true;
    bundle.trackId = trackId;
    bundle.analyzerVersion = analyzerVersion;
    bundle.lastAccessUtcMs = accessed;
    return true;
}

bool NekoDatabase::removeAnalysisBundle(const std::string& trackId,
    const std::string& analyzerVersion, std::string& error)
{
    error.clear();
    if (trackId.empty() || analyzerVersion.empty()) {
        error = "analysis bundle removal identity is incomplete";
        return false;
    }
    Statement remove(db_, R"sql(
DELETE FROM analysis_manifests
WHERE track_id=?1 AND analyzer_version=?2;
)sql", error);
    return remove && remove.bindText(1, trackId, error) &&
        remove.bindText(2, analyzerVersion, error) && remove.stepDone(error);
}

bool NekoDatabase::storeLyricsDocumentAndBinding(
    const PersistedLyricsDocumentV2& document,
    const PersistedLyricsBindingV2& binding,
    std::string& error)
{
    error.clear();
    if (document.documentId.empty() || document.provider.empty() ||
        binding.entityId.empty() || binding.mediaRevision.empty() ||
        binding.documentId != document.documentId ||
        document.plainTextUtf8.size() > kDatabaseMaximumTextBytes ||
        document.syncedTextUtf8.size() > kDatabaseMaximumTextBytes) {
        error = "lyrics cache payload is incomplete or too large";
        return false;
    }
    if (!execute("BEGIN IMMEDIATE;", error)) return false;
    Statement stored(db_, R"sql(
INSERT INTO lyrics_documents_v2(
    document_id, provider, provider_record_id, provider_revision,
    title, artist, album, duration_ms, plain_text, synced_text,
    instrumental, text_hash, timing_hash, timing_status, reason_codes,
    source_display_name, attribution, source_url,
    fetched_utc_ms, expiry_utc_ms, policy_version)
VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,
       ?16,?17,?18,?19,?20,?21)
ON CONFLICT(document_id) DO UPDATE SET
    provider=excluded.provider,
    provider_record_id=excluded.provider_record_id,
    provider_revision=excluded.provider_revision,
    title=excluded.title, artist=excluded.artist, album=excluded.album,
    duration_ms=excluded.duration_ms,
    plain_text=excluded.plain_text, synced_text=excluded.synced_text,
    instrumental=excluded.instrumental,
    text_hash=excluded.text_hash, timing_hash=excluded.timing_hash,
    timing_status=excluded.timing_status,
    reason_codes=excluded.reason_codes,
    source_display_name=excluded.source_display_name,
    attribution=excluded.attribution, source_url=excluded.source_url,
    fetched_utc_ms=excluded.fetched_utc_ms,
    expiry_utc_ms=excluded.expiry_utc_ms,
    policy_version=excluded.policy_version;
)sql", error);
    const bool documentStored = stored &&
        stored.bindText(1, document.documentId, error) &&
        stored.bindText(2, document.provider, error) &&
        stored.bindText(3, document.providerRecordId, error) &&
        stored.bindText(4, document.providerRevision, error) &&
        stored.bindText(5, document.title, error) &&
        stored.bindText(6, document.artist, error) &&
        stored.bindText(7, document.album, error) &&
        stored.bindInt64(8, document.durationMs, error) &&
        stored.bindText(9, document.plainTextUtf8, error) &&
        stored.bindText(10, document.syncedTextUtf8, error) &&
        stored.bindInt64(11, document.instrumental ? 1 : 0, error) &&
        stored.bindText(12, document.textHash, error) &&
        stored.bindText(13, document.timingHash, error) &&
        stored.bindInt64(14, document.timingStatus, error) &&
        stored.bindText(15, document.reasonCodes, error) &&
        stored.bindText(16, document.sourceDisplayName, error) &&
        stored.bindText(17, document.attribution, error) &&
        stored.bindText(18, document.sourceUrl, error) &&
        stored.bindInt64(19, document.fetchedUtcMs, error) &&
        stored.bindInt64(20, document.expiryUtcMs, error) &&
        stored.bindInt64(21, document.policyVersion, error) &&
        stored.stepDone(error);
    if (!documentStored) {
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return false;
    }
    Statement linked(db_, R"sql(
INSERT INTO lyrics_bindings_v2(
    entity_id, subsong, media_revision, metadata_revision,
    document_id, state, timing_status, reason_codes, updated_utc_ms)
VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)
ON CONFLICT(entity_id,subsong,media_revision,metadata_revision) DO UPDATE SET
    document_id=excluded.document_id, state=excluded.state,
    timing_status=excluded.timing_status,
    reason_codes=excluded.reason_codes,
    updated_utc_ms=excluded.updated_utc_ms;
)sql", error);
    const bool bindingStored = linked &&
        linked.bindText(1, binding.entityId, error) &&
        linked.bindInt64(2, binding.subsong, error) &&
        linked.bindText(3, binding.mediaRevision, error) &&
        linked.bindInt64(4, static_cast<std::int64_t>(binding.metadataRevision), error) &&
        linked.bindText(5, binding.documentId, error) &&
        linked.bindInt64(6, binding.state, error) &&
        linked.bindInt64(7, binding.timingStatus, error) &&
        linked.bindText(8, binding.reasonCodes, error) &&
        linked.bindInt64(9, binding.updatedUtcMs, error) &&
        linked.stepDone(error);
    if (!bindingStored || !execute("COMMIT;", error)) {
        std::string ignored;
        execute("ROLLBACK;", ignored);
        return false;
    }
    return true;
}

bool NekoDatabase::readLyricsForBinding(
    const std::string& entityId,
    std::uint32_t subsong,
    const std::string& mediaRevision,
    std::uint64_t metadataRevision,
    PersistedLyricsDocumentV2& document,
    PersistedLyricsBindingV2& binding,
    std::string& error)
{
    document = {};
    binding = {};
    error.clear();
    Statement statement(db_, R"sql(
SELECT b.entity_id,b.subsong,b.media_revision,b.metadata_revision,
       b.document_id,b.state,b.timing_status,b.reason_codes,b.updated_utc_ms,
       d.provider,d.provider_record_id,d.provider_revision,
       d.title,d.artist,d.album,d.duration_ms,d.plain_text,d.synced_text,
       d.instrumental,d.text_hash,d.timing_hash,d.timing_status,
       d.reason_codes,d.source_display_name,d.attribution,d.source_url,
       d.fetched_utc_ms,d.expiry_utc_ms,d.policy_version
FROM lyrics_bindings_v2 b
JOIN lyrics_documents_v2 d ON d.document_id=b.document_id
WHERE b.entity_id=?1 AND b.subsong=?2
  AND ((b.media_revision=?3 AND b.metadata_revision=?4)
       OR b.metadata_revision=0)
ORDER BY CASE WHEN b.media_revision=?3 AND b.metadata_revision=?4
              THEN 0 ELSE 1 END,
         b.updated_utc_ms DESC
LIMIT 1;
)sql", error);
    if (!statement || !statement.bindText(1, entityId, error) ||
        !statement.bindInt64(2, subsong, error) ||
        !statement.bindText(3, mediaRevision, error) ||
        !statement.bindInt64(4, static_cast<std::int64_t>(metadataRevision), error)) {
        return false;
    }
    const auto step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return true;
    if (step != SQLITE_ROW) {
        error = databaseError(db_, "lyrics binding query failed");
        return false;
    }
    binding.entityId = columnText(statement.get(), 0);
    binding.subsong = static_cast<std::uint32_t>(sqlite3_column_int64(statement.get(), 1));
    binding.mediaRevision = columnText(statement.get(), 2);
    binding.metadataRevision = static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 3));
    binding.documentId = columnText(statement.get(), 4);
    binding.state = sqlite3_column_int(statement.get(), 5);
    binding.timingStatus = sqlite3_column_int(statement.get(), 6);
    binding.reasonCodes = columnText(statement.get(), 7);
    binding.updatedUtcMs = sqlite3_column_int64(statement.get(), 8);
    document.found = true;
    document.documentId = binding.documentId;
    document.provider = columnText(statement.get(), 9);
    document.providerRecordId = columnText(statement.get(), 10);
    document.providerRevision = columnText(statement.get(), 11);
    document.title = columnText(statement.get(), 12);
    document.artist = columnText(statement.get(), 13);
    document.album = columnText(statement.get(), 14);
    document.durationMs = sqlite3_column_int64(statement.get(), 15);
    document.plainTextUtf8 = columnText(statement.get(), 16);
    document.syncedTextUtf8 = columnText(statement.get(), 17);
    document.instrumental = sqlite3_column_int(statement.get(), 18) != 0;
    document.textHash = columnText(statement.get(), 19);
    document.timingHash = columnText(statement.get(), 20);
    document.timingStatus = sqlite3_column_int(statement.get(), 21);
    document.reasonCodes = columnText(statement.get(), 22);
    document.sourceDisplayName = columnText(statement.get(), 23);
    document.attribution = columnText(statement.get(), 24);
    document.sourceUrl = columnText(statement.get(), 25);
    document.fetchedUtcMs = sqlite3_column_int64(statement.get(), 26);
    document.expiryUtcMs = sqlite3_column_int64(statement.get(), 27);
    document.policyVersion = static_cast<std::uint32_t>(sqlite3_column_int64(statement.get(), 28));
    return true;
}

bool NekoDatabase::storeLyricsRejection(
    const PersistedLyricsRejectionV2& rejection,
    std::string& error)
{
    error.clear();
    if (rejection.entityId.empty() || rejection.mediaRevision.empty() ||
        rejection.documentId.empty() || rejection.reason.empty()) {
        error = "lyrics rejection payload is incomplete";
        return false;
    }
    Statement statement(db_, R"sql(
INSERT INTO lyrics_rejections_v2(
    entity_id,subsong,media_revision,document_id,text_hash,timing_hash,
    reason,rejected_utc_ms)
VALUES(?1,?2,?3,?4,?5,?6,?7,?8)
ON CONFLICT(entity_id,subsong,media_revision,document_id) DO UPDATE SET
    text_hash=excluded.text_hash,
    timing_hash=excluded.timing_hash,
    reason=excluded.reason,
    rejected_utc_ms=excluded.rejected_utc_ms;
)sql", error);
    return statement &&
        statement.bindText(1, rejection.entityId, error) &&
        statement.bindInt64(2, rejection.subsong, error) &&
        statement.bindText(3, rejection.mediaRevision, error) &&
        statement.bindText(4, rejection.documentId, error) &&
        statement.bindText(5, rejection.textHash, error) &&
        statement.bindText(6, rejection.timingHash, error) &&
        statement.bindText(7, rejection.reason, error) &&
        statement.bindInt64(8, rejection.rejectedUtcMs, error) &&
        statement.stepDone(error);
}

bool NekoDatabase::readLyricsRejection(
    const std::string& entityId,
    std::uint32_t subsong,
    const std::string& mediaRevision,
    const std::string& documentId,
    PersistedLyricsRejectionV2& rejection,
    std::string& error)
{
    rejection = {};
    error.clear();
    Statement statement(db_, R"sql(
SELECT entity_id,subsong,media_revision,document_id,text_hash,timing_hash,
       reason,rejected_utc_ms
FROM lyrics_rejections_v2
WHERE entity_id=?1 AND subsong=?2 AND media_revision=?3 AND document_id=?4
LIMIT 1;
)sql", error);
    if (!statement || !statement.bindText(1, entityId, error) ||
        !statement.bindInt64(2, subsong, error) ||
        !statement.bindText(3, mediaRevision, error) ||
        !statement.bindText(4, documentId, error)) return false;
    const auto step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return true;
    if (step != SQLITE_ROW) {
        error = databaseError(db_, "lyrics rejection query failed");
        return false;
    }
    rejection.found = true;
    rejection.entityId = columnText(statement.get(), 0);
    rejection.subsong = static_cast<std::uint32_t>(
        sqlite3_column_int64(statement.get(), 1));
    rejection.mediaRevision = columnText(statement.get(), 2);
    rejection.documentId = columnText(statement.get(), 3);
    rejection.textHash = columnText(statement.get(), 4);
    rejection.timingHash = columnText(statement.get(), 5);
    rejection.reason = columnText(statement.get(), 6);
    rejection.rejectedUtcMs = sqlite3_column_int64(statement.get(), 7);
    return true;
}

std::uint64_t NekoDatabase::analysisCacheBytes(std::string& error)
{
    error.clear();
    Statement statement(db_,
        "SELECT COALESCE(SUM(stored_bytes), 0) FROM analysis_layers;", error);
    if (!statement) return 0;
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        error = databaseError(db_, "analysis cache size query failed");
        return 0;
    }
    const auto value = sqlite3_column_int64(statement.get(), 0);
    return value > 0 ? static_cast<std::uint64_t>(value) : 0;
}

bool NekoDatabase::pruneAnalysisCache(std::uint64_t cacheLimitBytes,
    const std::string& protectedTrackId,
    const std::string& protectedAnalyzerVersion, std::string& error)
{
    error.clear();
    while (analysisCacheBytes(error) > cacheLimitBytes) {
        if (!error.empty()) return false;
        std::string trackId;
        std::string analyzerVersion;
        {
            Statement oldest(db_, R"sql(
SELECT track_id, analyzer_version
FROM analysis_layers
WHERE NOT (track_id=?1 AND analyzer_version=?2)
GROUP BY track_id, analyzer_version
ORDER BY MAX(last_access_utc_ms) ASC, track_id ASC, analyzer_version ASC
LIMIT 1;
)sql", error);
            if (!oldest || !oldest.bindText(1, protectedTrackId, error) ||
                !oldest.bindText(2, protectedAnalyzerVersion, error)) return false;
            const auto step = sqlite3_step(oldest.get());
            if (step == SQLITE_DONE) return true;
            if (step != SQLITE_ROW) {
                error = databaseError(db_, "analysis cache LRU query failed");
                return false;
            }
            trackId = columnText(oldest.get(), 0);
            analyzerVersion = columnText(oldest.get(), 1);
        }
        Statement remove(db_, R"sql(
DELETE FROM analysis_manifests
WHERE track_id=?1 AND analyzer_version=?2;
)sql", error);
        if (!remove || !remove.bindText(1, trackId, error) ||
            !remove.bindText(2, analyzerVersion, error) ||
            !remove.stepDone(error)) return false;
    }
    return error.empty();
}

} // namespace nekorox
