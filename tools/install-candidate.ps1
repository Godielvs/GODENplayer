param(
    [Parameter(Mandatory = $true)]
    [string]$TargetRoot,
    [Parameter(Mandatory = $true)]
    [string]$LayoutPath,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$Apply,
    [string]$ReportPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$target = [IO.Path]::GetFullPath($TargetRoot).TrimEnd('\')
$layout = [IO.Path]::GetFullPath($LayoutPath)
$component = Join-Path $repo "out\$Configuration\foo_nekorox.dll"
$worker = Join-Path $repo "out\$Configuration\nekorox-analysis.exe"
$shader = Join-Path $repo "out\$Configuration\goden-shader-render.exe"
$migrator = Join-Path $PSScriptRoot 'install-native-shell-layout.js'
foreach ($required in @($target, $layout, $component, $worker, $shader, $migrator)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Candidate input is missing: $required"
    }
}

$componentText = [Text.Encoding]::Unicode.GetString(
    [IO.File]::ReadAllBytes($component))
if (-not $componentText.Contains('GodenPlayerShellV2')) {
    throw 'Candidate binary does not advertise the PlayerShell V2 window contract.'
}

$temporary = Join-Path ([IO.Path]::GetTempPath()) `
    ('goden-candidate-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
$migrated = Join-Path $temporary 'nekorox-v3-shell-v2.fcl'
try {
    $migration = (& node $migrator $layout $migrated | Out-String) |
        ConvertFrom-Json
    $migrationHash = (Get-FileHash -LiteralPath $migrated -Algorithm SHA256).Hash
    $componentHash = (Get-FileHash -LiteralPath $component -Algorithm SHA256).Hash
    $workerHash = (Get-FileHash -LiteralPath $worker -Algorithm SHA256).Hash
    $operations = @(
        [ordered]@{ source = $component; destination = Join-Path $target 'profile\user-components-x64\foo_nekorox\foo_nekorox.dll' },
        [ordered]@{ source = $worker; destination = Join-Path $target 'profile\user-components-x64\foo_nekorox\nekorox-analysis.exe' },
        [ordered]@{ source = $shader; destination = Join-Path $target 'profile\user-components-x64\foo_nekorox\goden-shader-render.exe' },
        [ordered]@{ source = $migrated; destination = Join-Path $target 'migration\nekorox-v3-shell-v2.fcl' }
    )
    if ($Apply) {
        $candidateExecutables = @(
            (Join-Path $target 'foobar2000.exe'),
            (Join-Path $target 'app\foobar2000.exe'))
        $running = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
            Where-Object {
                $_.ExecutablePath -and $candidateExecutables -contains
                    [IO.Path]::GetFullPath($_.ExecutablePath)
            })
        if ($running.Count -ne 0) {
            throw 'Close the explicit candidate foobar2000 target before applying.'
        }
        $stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
        foreach ($operation in $operations) {
            $parent = Split-Path -Parent $operation.destination
            New-Item -ItemType Directory -Path $parent -Force | Out-Null
            if (Test-Path -LiteralPath $operation.destination -PathType Leaf) {
                Copy-Item -LiteralPath $operation.destination -Destination `
                    "$($operation.destination).before-shell-v2-$stamp" -ErrorAction Stop
            }
            Copy-Item -LiteralPath $operation.source -Destination `
                $operation.destination -Force
        }
    }
    $report = [ordered]@{
        schema = 'goden.candidate-install.v2'
        createdUtc = [DateTime]::UtcNow.ToString('o')
        mode = if ($Apply) { 'applied' } else { 'dry-run' }
        targetRoot = $target
        configuration = $Configuration
        componentSha256 = $componentHash
        workerSha256 = $workerHash
        layoutSha256 = $migrationHash
        sourceKind = $migration.sourceKind
        mutatesActiveConfiguration = $false
        operations = $operations
    }
    if ($ReportPath) {
        $reportFull = [IO.Path]::GetFullPath($ReportPath)
        New-Item -ItemType Directory -Path (Split-Path -Parent $reportFull) `
            -Force | Out-Null
        $report | ConvertTo-Json -Depth 5 |
            Set-Content -LiteralPath $reportFull -Encoding UTF8
    }
    $report | ConvertTo-Json -Depth 5
}
finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction SilentlyContinue
}
