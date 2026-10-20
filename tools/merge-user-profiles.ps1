param(
    [Parameter(Mandatory=$true)][string]$TargetProfile,
    [string]$SourceProfile = "$env:LOCALAPPDATA\Programs\NekoRoX\app\profile",
    [string]$ReportPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$target = [IO.Path]::GetFullPath($TargetProfile).TrimEnd('\')
$source = [IO.Path]::GetFullPath($SourceProfile).TrimEnd('\')
$sqlite = (Get-Command sqlite3 -ErrorAction Stop).Source
$targetNeko = Join-Path $target 'nekorox\nekorox.db'
$sourceNeko = Join-Path $source 'nekorox\nekorox.db'
$targetMeta = Join-Path $target 'metadb.sqlite'
$sourceMeta = Join-Path $source 'metadb.sqlite'
$statsIndex = 'metadb_index_C653739F_14B3_4EF2_819B_A3E2883230AE'

function Invoke-Sqlite(
    [string]$Database,
    [string]$Sql
) {
    $output = @(& $sqlite $Database $Sql 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "sqlite3 failed for ${Database}: $($output -join [Environment]::NewLine)"
    }
    return ($output -join "`n").Trim()
}

function ConvertTo-SqlString([string]$Value) {
    return $Value.Replace("'", "''").Replace('\', '/')
}

function Get-UInt64FromHex([string]$Hex) {
    if ($Hex.Length -lt 16) {
        throw "Playback Statistics blob is shorter than eight bytes: $Hex"
    }
    [byte[]]$bytes = 0..7 | ForEach-Object {
        [Convert]::ToByte($Hex.Substring($_ * 2, 2), 16)
    }
    return [BitConverter]::ToUInt64($bytes, 0)
}

function Assert-DatabaseHealthy([string]$Database) {
    if (-not (Test-Path -LiteralPath $Database -PathType Leaf)) {
        throw "Database not found: $Database"
    }
    $check = Invoke-Sqlite $Database 'PRAGMA quick_check;'
    if ($check -cne 'ok') {
        throw "SQLite quick_check failed for ${Database}: $check"
    }
}

$running = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'")
if ($running.Count -gt 0) {
    $details = $running | ForEach-Object {
        "PID $($_.ProcessId): $($_.ExecutablePath)"
    }
    throw "Close every foobar2000 process before merging profiles.`n$($details -join "`n")"
}

if ($target -ceq $source) {
    throw 'TargetProfile and SourceProfile must be different.'
}

foreach ($database in @($targetNeko, $sourceNeko, $targetMeta, $sourceMeta)) {
    Assert-DatabaseHealthy $database
}

$targetMigrations = Invoke-Sqlite $targetNeko (
    "SELECT group_concat(version, ',') FROM " +
    '(SELECT version FROM schema_migrations ORDER BY version);')
$sourceMigrations = Invoke-Sqlite $sourceNeko (
    "SELECT group_concat(version, ',') FROM " +
    '(SELECT version FROM schema_migrations ORDER BY version);')
if ($targetMigrations -cne $sourceMigrations) {
    throw "NekoRoX schema migrations differ: target=$targetMigrations source=$sourceMigrations"
}

$sourceNekoSql = ConvertTo-SqlString $sourceNeko
$nekoBeforeTracks = [int64](Invoke-Sqlite $targetNeko 'SELECT count(*) FROM tracks;')
$nekoBeforeEvents = [int64](Invoke-Sqlite $targetNeko 'SELECT count(*) FROM play_events;')

$nekoMergeSql = @"
PRAGMA foreign_keys=ON;
ATTACH DATABASE '$sourceNekoSql' AS src;
BEGIN IMMEDIATE;

INSERT INTO tracks (
    track_id, path, subsong, file_size, mtime_ticks, partial_fingerprint,
    artist, album, title, duration_ms, last_seen_utc_ms
)
SELECT
    track_id, path, subsong, file_size, mtime_ticks, partial_fingerprint,
    artist, album, title, duration_ms, last_seen_utc_ms
FROM src.tracks
WHERE 1
ON CONFLICT(track_id) DO UPDATE SET
    path = excluded.path,
    subsong = excluded.subsong,
    file_size = excluded.file_size,
    mtime_ticks = excluded.mtime_ticks,
    partial_fingerprint = excluded.partial_fingerprint,
    artist = excluded.artist,
    album = excluded.album,
    title = excluded.title,
    duration_ms = excluded.duration_ms,
    last_seen_utc_ms = excluded.last_seen_utc_ms
WHERE excluded.last_seen_utc_ms > tracks.last_seen_utc_ms;

INSERT INTO sessions (session_id, started_utc_ms, ended_utc_ms, app_version)
SELECT session_id, started_utc_ms, ended_utc_ms, app_version
FROM src.sessions
WHERE 1
ON CONFLICT(session_id) DO UPDATE SET
    ended_utc_ms = CASE
        WHEN excluded.ended_utc_ms IS NULL THEN sessions.ended_utc_ms
        WHEN sessions.ended_utc_ms IS NULL THEN excluded.ended_utc_ms
        WHEN excluded.ended_utc_ms > sessions.ended_utc_ms THEN excluded.ended_utc_ms
        ELSE sessions.ended_utc_ms
    END;

INSERT INTO personas (persona_id, display_name, profile, updated_utc_ms)
SELECT persona_id, display_name, profile, updated_utc_ms FROM src.personas WHERE 1
ON CONFLICT(persona_id) DO UPDATE SET
    display_name = excluded.display_name,
    profile = excluded.profile,
    updated_utc_ms = excluded.updated_utc_ms
WHERE excluded.updated_utc_ms > personas.updated_utc_ms;

INSERT OR IGNORE INTO persona_aliases (alias, alias_key, persona_id)
SELECT alias, alias_key, persona_id FROM src.persona_aliases;

INSERT OR IGNORE INTO persona_relationships (
    source_persona_id, target_persona_id, relation, note
)
SELECT source_persona_id, target_persona_id, relation, note
FROM src.persona_relationships;

INSERT INTO favorites (track_id, favorite, updated_utc_ms)
SELECT track_id, favorite, updated_utc_ms FROM src.favorites WHERE 1
ON CONFLICT(track_id) DO UPDATE SET
    favorite = excluded.favorite,
    updated_utc_ms = excluded.updated_utc_ms
WHERE excluded.updated_utc_ms > favorites.updated_utc_ms;

INSERT INTO notes (track_id, body, updated_utc_ms)
SELECT track_id, body, updated_utc_ms FROM src.notes WHERE 1
ON CONFLICT(track_id) DO UPDATE SET
    body = excluded.body,
    updated_utc_ms = excluded.updated_utc_ms
WHERE excluded.updated_utc_ms > notes.updated_utc_ms;

INSERT INTO lyrics (track_id, source, body, is_synced, updated_utc_ms)
SELECT track_id, source, body, is_synced, updated_utc_ms FROM src.lyrics WHERE 1
ON CONFLICT(track_id) DO UPDATE SET
    source = excluded.source,
    body = excluded.body,
    is_synced = excluded.is_synced,
    updated_utc_ms = excluded.updated_utc_ms
WHERE excluded.updated_utc_ms > lyrics.updated_utc_ms;

INSERT INTO settings (key, value, updated_utc_ms)
SELECT key, value, updated_utc_ms FROM src.settings WHERE 1
ON CONFLICT(key) DO UPDATE SET
    value = excluded.value,
    updated_utc_ms = excluded.updated_utc_ms
WHERE excluded.updated_utc_ms > settings.updated_utc_ms;

INSERT INTO palettes (track_id, palette_blob, algorithm_version, updated_utc_ms)
SELECT track_id, palette_blob, algorithm_version, updated_utc_ms
FROM src.palettes WHERE 1
ON CONFLICT(track_id) DO UPDATE SET
    palette_blob = excluded.palette_blob,
    algorithm_version = excluded.algorithm_version,
    updated_utc_ms = excluded.updated_utc_ms
WHERE excluded.updated_utc_ms > palettes.updated_utc_ms;

INSERT INTO analysis_manifests (
    track_id, analyzer_version, state, manifest_json, result_blob, updated_utc_ms
)
SELECT
    track_id, analyzer_version, state, manifest_json, result_blob, updated_utc_ms
FROM src.analysis_manifests WHERE 1
ON CONFLICT(track_id, analyzer_version) DO UPDATE SET
    state = excluded.state,
    manifest_json = excluded.manifest_json,
    result_blob = excluded.result_blob,
    updated_utc_ms = excluded.updated_utc_ms
WHERE excluded.updated_utc_ms > analysis_manifests.updated_utc_ms;

INSERT INTO analysis_layers (
    track_id, analyzer_version, layer_name, resolution_ms, codec,
    uncompressed_bytes, stored_bytes, payload, last_access_utc_ms
)
SELECT
    track_id, analyzer_version, layer_name, resolution_ms, codec,
    uncompressed_bytes, stored_bytes, payload, last_access_utc_ms
FROM src.analysis_layers WHERE 1
ON CONFLICT(track_id, analyzer_version, layer_name) DO UPDATE SET
    resolution_ms = excluded.resolution_ms,
    codec = excluded.codec,
    uncompressed_bytes = excluded.uncompressed_bytes,
    stored_bytes = excluded.stored_bytes,
    payload = excluded.payload,
    last_access_utc_ms = excluded.last_access_utc_ms
WHERE excluded.last_access_utc_ms > analysis_layers.last_access_utc_ms;

INSERT INTO art_index (
    asset_id, track_id, persona_id, role, path, content_hash,
    selected_by_user, updated_utc_ms
)
SELECT
    asset_id, track_id, persona_id, role, path, content_hash,
    selected_by_user, updated_utc_ms
FROM src.art_index WHERE 1
ON CONFLICT(asset_id) DO UPDATE SET
    track_id = excluded.track_id,
    persona_id = excluded.persona_id,
    role = excluded.role,
    path = excluded.path,
    content_hash = excluded.content_hash,
    selected_by_user = excluded.selected_by_user,
    updated_utc_ms = excluded.updated_utc_ms
WHERE excluded.updated_utc_ms > art_index.updated_utc_ms;

INSERT INTO play_events (
    session_id, track_id, event_type, event_utc_ms, position_ms,
    duration_ms, playlist_index, item_index
)
SELECT DISTINCT
    s.session_id, s.track_id, s.event_type, s.event_utc_ms, s.position_ms,
    s.duration_ms, s.playlist_index, s.item_index
FROM src.play_events AS s
WHERE NOT EXISTS (
    SELECT 1
    FROM play_events AS t
    WHERE t.session_id = s.session_id
      AND t.track_id = s.track_id
      AND t.event_type = s.event_type
      AND t.event_utc_ms = s.event_utc_ms
      AND t.position_ms = s.position_ms
      AND t.duration_ms = s.duration_ms
);

COMMIT;
DETACH DATABASE src;
PRAGMA foreign_keys=ON;
PRAGMA foreign_key_check;
PRAGMA quick_check;
"@

$nekoMergeResult = Invoke-Sqlite $targetNeko $nekoMergeSql
if (($nekoMergeResult -split "`n")[-1] -cne 'ok') {
    throw "NekoRoX database validation failed after merge: $nekoMergeResult"
}

$nekoAfterTracks = [int64](Invoke-Sqlite $targetNeko 'SELECT count(*) FROM tracks;')
$nekoAfterEvents = [int64](Invoke-Sqlite $targetNeko 'SELECT count(*) FROM play_events;')
$nekoSourceMissing = [int64](Invoke-Sqlite $targetNeko @"
ATTACH DATABASE '$sourceNekoSql' AS src;
SELECT count(*)
FROM src.play_events AS s
WHERE NOT EXISTS (
    SELECT 1 FROM play_events AS t
    WHERE t.session_id = s.session_id
      AND t.track_id = s.track_id
      AND t.event_type = s.event_type
      AND t.event_utc_ms = s.event_utc_ms
      AND t.position_ms = s.position_ms
      AND t.duration_ms = s.duration_ms
);
DETACH DATABASE src;
"@)
if ($nekoSourceMissing -ne 0) {
    throw "NekoRoX merge left $nekoSourceMissing source play events missing."
}

$sourceMetaSql = ConvertTo-SqlString $sourceMeta
$targetIndexTables = @((Invoke-Sqlite $targetMeta (
    "SELECT name FROM sqlite_master WHERE type='table' " +
    "AND name GLOB 'metadb_index_*' AND name NOT GLOB '*_data' " +
    'ORDER BY name;')) -split "`n" | Where-Object { $_ })
$sourceIndexTables = @((Invoke-Sqlite $sourceMeta (
    "SELECT name FROM sqlite_master WHERE type='table' " +
    "AND name GLOB 'metadb_index_*' AND name NOT GLOB '*_data' " +
    'ORDER BY name;')) -split "`n" | Where-Object { $_ })
$indexDifference = @(Compare-Object $targetIndexTables $sourceIndexTables)
if ($indexDifference.Count -gt 0) {
    throw "metadb index table sets differ: $($indexDifference | Out-String)"
}

$metaBefore = [int64](Invoke-Sqlite $targetMeta 'SELECT count(*) FROM metadb;')
$indexSql = ''
foreach ($table in $targetIndexTables) {
    if ($table -notmatch '^metadb_index_[A-F0-9_]+$') {
        throw "Unexpected metadb index table name: $table"
    }
    $dataTable = "${table}_data"
    $indexSql += @"
WITH new_rows AS (
    SELECT s.filename, row_number() OVER (ORDER BY s.filename) AS row_number
    FROM src.[$table] AS s
    WHERE NOT EXISTS (
        SELECT 1 FROM main.[$table] AS t WHERE t.filename = s.filename
    )
)
INSERT INTO main.[$table] (key, filename)
SELECT
    (SELECT coalesce(max(key), 0) FROM main.[$table]) + row_number,
    filename
FROM new_rows;

INSERT INTO main.[$dataTable] (key, value)
SELECT target_map.key, source_data.value
FROM src.[$table] AS source_map
JOIN src.[$dataTable] AS source_data ON source_data.key = source_map.key
JOIN main.[$table] AS target_map ON target_map.filename = source_map.filename
WHERE NOT EXISTS (
    SELECT 1 FROM main.[$dataTable] AS target_data
    WHERE target_data.key = target_map.key
);
"@
}

$metaMergeSql = @"
ATTACH DATABASE '$sourceMetaSql' AS src;
BEGIN IMMEDIATE;

INSERT OR IGNORE INTO main.config (key, value)
SELECT key, value FROM src.config;

INSERT OR IGNORE INTO main.metadb_indexes (name, synced, retention)
SELECT name, synced, retention FROM src.metadb_indexes;

INSERT OR IGNORE INTO main.metadb (
    name, info, infoBrowse, size, lastModified, infoBrowseTime,
    lastseen, created, attribs, attribsValid
)
SELECT
    name, info, infoBrowse, size, lastModified, infoBrowseTime,
    lastseen, created, attribs, attribsValid
FROM src.metadb;

$indexSql
COMMIT;
DETACH DATABASE src;
PRAGMA quick_check;
"@

$metaMergeResult = Invoke-Sqlite $targetMeta $metaMergeSql
if (($metaMergeResult -split "`n")[-1] -cne 'ok') {
    throw "metadb validation failed after union: $metaMergeResult"
}

$statsConflictRows = Invoke-Sqlite $targetMeta @"
ATTACH DATABASE '$sourceMetaSql' AS src;
SELECT target_map.key || '|' || hex(target_data.value) || '|' || hex(source_data.value)
FROM main.[$statsIndex] AS target_map
JOIN main.[${statsIndex}_data] AS target_data ON target_data.key = target_map.key
JOIN src.[$statsIndex] AS source_map ON source_map.filename = target_map.filename
JOIN src.[${statsIndex}_data] AS source_data ON source_data.key = source_map.key
WHERE target_data.value <> source_data.value
ORDER BY target_map.key;
DETACH DATABASE src;
"@

$statsConflictCount = 0
$statsSelectedFromSource = 0
$statsUpdates = ''
foreach ($row in @($statsConflictRows -split "`n" | Where-Object { $_ })) {
    $parts = $row.Split('|')
    if ($parts.Count -ne 3) {
        throw "Unexpected playback-stat comparison row: $row"
    }
    $statsConflictCount++
    $targetCount = Get-UInt64FromHex $parts[1]
    $sourceCount = Get-UInt64FromHex $parts[2]
    if ($sourceCount -gt $targetCount) {
        $key = [int64]$parts[0]
        $sourceBlob = $parts[2]
        $statsUpdates += "UPDATE main.[${statsIndex}_data] SET value = X'$sourceBlob' WHERE key = $key;`n"
        $statsSelectedFromSource++
    }
}

if ($statsUpdates) {
    $statsResult = Invoke-Sqlite $targetMeta @"
BEGIN IMMEDIATE;
$statsUpdates
COMMIT;
PRAGMA quick_check;
"@
    if (($statsResult -split "`n")[-1] -cne 'ok') {
        throw "metadb validation failed after playback-stat merge: $statsResult"
    }
}

$metaAfter = [int64](Invoke-Sqlite $targetMeta 'SELECT count(*) FROM metadb;')
$metaSourceMissing = [int64](Invoke-Sqlite $targetMeta @"
ATTACH DATABASE '$sourceMetaSql' AS src;
SELECT count(*) FROM src.metadb AS s
WHERE NOT EXISTS (SELECT 1 FROM main.metadb AS t WHERE t.name = s.name);
DETACH DATABASE src;
"@)
if ($metaSourceMissing -ne 0) {
    throw "metadb merge left $metaSourceMissing source entries missing."
}

Assert-DatabaseHealthy $targetNeko
Assert-DatabaseHealthy $targetMeta

$report = [ordered]@{
    schemaVersion = 1
    generatedUtc = [DateTime]::UtcNow.ToString('o')
    targetProfile = $target
    sourceProfile = $source
    neko = [ordered]@{
        migrations = $targetMigrations
        tracksBefore = $nekoBeforeTracks
        tracksAfter = $nekoAfterTracks
        playEventsBefore = $nekoBeforeEvents
        playEventsAfter = $nekoAfterEvents
        sourcePlayEventsMissing = $nekoSourceMissing
        quickCheck = 'ok'
    }
    metadb = [ordered]@{
        entriesBefore = $metaBefore
        entriesAfter = $metaAfter
        sourceEntriesMissing = $metaSourceMissing
        indexTables = $targetIndexTables.Count
        playbackStatConflicts = $statsConflictCount
        playbackStatBlobsSelectedFromSource = $statsSelectedFromSource
        playbackStatPolicy = 'Keep target on ties; otherwise retain the valid component blob with the greater play count.'
        quickCheck = 'ok'
    }
    playlists = [ordered]@{
        policy = 'Target profile retained. Divergent playlists are transient NekoRoX Selection/Open state; source remains recoverable from the pre-merge snapshot.'
    }
}

if ($ReportPath) {
    $fullReportPath = [IO.Path]::GetFullPath($ReportPath)
    $reportDirectory = Split-Path -Parent $fullReportPath
    [IO.Directory]::CreateDirectory($reportDirectory) | Out-Null
    [IO.File]::WriteAllText(
        $fullReportPath,
        ($report | ConvertTo-Json -Depth 8),
        [Text.UTF8Encoding]::new($false))
}

$report | ConvertTo-Json -Depth 8
