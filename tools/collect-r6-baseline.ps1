param(
    [string]$Base = 'df11db3e1f83cea8347086166101976bbeeecc87',
    [string]$OutputPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $OutputPath) {
    $OutputPath = Join-Path $repo 'artifacts\r6\baseline\baseline.json'
}

function Get-Sha256([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

$trackedContracts = @(
    'src/core/include/nekorox/scroll_controller.h',
    'src/core/src/scroll_controller.cpp',
    'tests/fixtures/wheel_schedules.json',
    'src/component/pixel_ui/goden_pixel_assets.generated.hpp',
    'src/component/shaders/listen_scope_light.hlsl',
    'src/component/scope_light_pass.cpp',
    'src/component/scope_light_pass.h'
)
$contractRows = foreach ($relative in $trackedContracts) {
    $absolute = Join-Path $repo ($relative -replace '/', '\')
    $baseBlob = (& git -C $repo rev-parse "${Base}:$relative" 2>$null)
    & git -C $repo diff --quiet $Base -- $relative
    $changedFromBase = $LASTEXITCODE -ne 0
    [ordered]@{
        path = $relative
        base_git_blob = if ($LASTEXITCODE -eq 0) { $baseBlob } else { $null }
        current_sha256 = Get-Sha256 $absolute
        changed_from_base = $changedFromBase
    }
}

$binaryRows = foreach ($relative in @(
    'out/Release/foo_nekorox.dll',
    'out/Release/foo_nekorox.pdb',
    'out/Release/nekorox-analysis.exe',
    'out/Release/nekorox_tests.exe',
    'out/Release/r6_d2d_effect_probe.exe',
    'out/obj/foo_nekorox/Release/ambient-shaders/album_ambient_a1_ps.h'
)) {
    $absolute = Join-Path $repo ($relative -replace '/', '\')
    [ordered]@{
        path = $relative
        exists = Test-Path -LiteralPath $absolute -PathType Leaf
        bytes = if (Test-Path -LiteralPath $absolute -PathType Leaf) {
            (Get-Item -LiteralPath $absolute).Length
        } else { 0 }
        sha256 = Get-Sha256 $absolute
    }
}

$status = @(& git -C $repo status --porcelain=v1)
$diffText = (& git -C $repo diff --binary --no-ext-diff | Out-String)
$diffBytes = [Text.Encoding]::UTF8.GetBytes($diffText)
$sha = [Security.Cryptography.SHA256]::Create()
try { $dirtyDigest = ([BitConverter]::ToString($sha.ComputeHash($diffBytes))).Replace('-', '') }
finally { $sha.Dispose() }

$result = [ordered]@{
    schema = 'goden.r6.baseline.v1'
    captured_utc = [DateTime]::UtcNow.ToString('o')
    repository = $repo
    required_base = $Base
    head = (& git -C $repo rev-parse HEAD)
    branch = (& git -C $repo branch --show-current)
    initial_state = [ordered]@{
        observed_before_r6 = 'clean'
        observed_head = $Base
    }
    current_worktree = [ordered]@{
        dirty = $status.Count -gt 0
        porcelain = $status
        tracked_diff_sha256 = $dirtyDigest
    }
    submodules = @(& git -C $repo submodule status --recursive)
    rollout = [ordered]@{
        environment = 'GODEN_FLAG_ALBUM_AMBIENT_A1'
        production_default = 'off'
        qa_value = '1'
        preference_default = 'subtle'
    }
    frozen_contracts = @($contractRows)
    binaries = @($binaryRows)
    host = [ordered]@{
        os = (Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber)
        cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1 Name, NumberOfCores, NumberOfLogicalProcessors)
        gpu = @(Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion, AdapterRAM)
    }
}
$directory = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Force -Path $directory | Out-Null
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Write-Host $OutputPath
