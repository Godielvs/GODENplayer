param(
    [ValidateSet('Release')]
    [string]$Configuration = 'Release',
    [string]$EvidenceRoot = '',
    [string]$DisposableRoot = '',
    [switch]$RunInstalled
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if (-not $EvidenceRoot) {
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
    $EvidenceRoot = Join-Path $repo "artifacts\r3\$stamp\qualification"
}
$evidence = [IO.Path]::GetFullPath($EvidenceRoot).TrimEnd('\')
$allowedEvidence = [IO.Path]::GetFullPath(
    (Join-Path $repo 'artifacts')).TrimEnd('\') + '\'
if (-not ($evidence + '\').StartsWith(
        $allowedEvidence, [StringComparison]::OrdinalIgnoreCase)) {
    throw "R3 evidence must remain below repository artifacts: $evidence"
}
New-Item -ItemType Directory -Path $evidence -Force | Out-Null

$started = [DateTime]::UtcNow
$sourceLog = Join-Path $evidence 'source-matrix.log'
& (Join-Path $PSScriptRoot 'test.ps1') -Configuration $Configuration *>&1 |
    Tee-Object -LiteralPath $sourceLog
if ($LASTEXITCODE -ne 0) { throw 'R3 source matrix failed.' }

$installed = [ordered]@{
    state = 'not_run'
    reason = 'RunInstalled was not requested; no installed observation is inferred.'
}
if ($RunInstalled) {
    if (-not $DisposableRoot) {
        throw 'RunInstalled requires an explicit disposable root.'
    }
    $disposable = [IO.Path]::GetFullPath($DisposableRoot).TrimEnd('\')
    $allowedDisposable = $allowedEvidence
    if (-not ($disposable + '\').StartsWith(
            $allowedDisposable, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Installed qualification is restricted to a disposable artifacts root: $disposable"
    }
    $host = Join-Path $disposable 'foobar2000.exe'
    if (-not (Test-Path -LiteralPath $host -PathType Leaf)) {
        $host = Join-Path $disposable 'app\foobar2000.exe'
    }
    if (-not (Test-Path -LiteralPath $host -PathType Leaf)) {
        throw "Disposable foobar2000 host is missing: $disposable"
    }
    $layout = Join-Path $repo 'config\layout\nekorox-v3-cohesive-bottom.fcl'
    $installReport = Join-Path $evidence 'candidate-install.json'
    & (Join-Path $PSScriptRoot 'install-candidate.ps1') `
        -TargetRoot $disposable -LayoutPath $layout `
        -Configuration $Configuration -Apply -ReportPath $installReport | Out-Host
    $migratedLayout = Join-Path $disposable `
        'migration\nekorox-v3-shell-v2.fcl'
    $layoutReport = Join-Path $evidence 'layout-runtime.json'
    & (Join-Path $PSScriptRoot 'install-columns-layout-runtime.ps1') `
        -DevRoot (Split-Path -Parent $disposable) `
        -LayoutPath $migratedLayout `
        -ProbePath (Join-Path $repo "out\$Configuration\nekorox_ui_probe.exe") `
        -ManifestPath $layoutReport
    $installed = [ordered]@{
        state = 'candidate_applied_to_disposable_root'
        root = $disposable
        host = $host
        install_report = $installReport
        layout_report = $layoutReport
        limitation = 'Visual, displayed-frame, driver-fault and auditory observations still require their explicit probes/operator evidence.'
    }
}

$result = [ordered]@{
    schema = 'goden.r3.qualification.v1'
    started_utc = $started.ToString('o')
    completed_utc = [DateTime]::UtcNow.ToString('o')
    configuration = $Configuration
    source_matrix = [ordered]@{
        state = 'verified_local'
        command = 'tools/test.ps1 -Configuration Release'
        log = $sourceLog
        sha256 = (Get-FileHash -LiteralPath $sourceLog -Algorithm SHA256).Hash
    }
    installed = $installed
    safety = [ordered]@{
        personal_install_modified = $false
        personal_media_modified = $false
        published = $false
    }
}
$resultPath = Join-Path $evidence 'qualification.json'
$result | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath $resultPath -Encoding UTF8
Write-Host "R3 qualification record: $resultPath"
