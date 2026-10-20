param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath($RepoRoot)
$migrator = Join-Path $repo 'tools\install-native-shell-layout.js'
$cohesive = Join-Path $repo 'tools\install-cohesive-bottom-layout.js'
$fixtures = @(
    # C3 promoted the repository layout fixture to the native V3 shell. Keep
    # this assertion aligned with the checked-in fixture so the test verifies
    # V3 idempotence instead of claiming that current production data is V1.
    @{ kind = 'v3'; path = Join-Path $repo 'config\layout\nekorox-v3-cohesive-bottom.fcl' },
    @{ kind = 'old'; path = Join-Path $repo 'config\layout\nekorox-v3.fcl.before-player-shell.fcl' }
)
foreach ($required in @($migrator, $cohesive) + @($fixtures.path)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Shell integration input is missing: $required"
    }
}

$temporary = Join-Path ([IO.Path]::GetTempPath()) `
    ('goden-shell-r2-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
try {
    foreach ($fixture in $fixtures) {
        $first = Join-Path $temporary "$($fixture.kind)-first.fcl"
        $second = Join-Path $temporary "$($fixture.kind)-second.fcl"
        $cohesiveOutput = Join-Path $temporary "$($fixture.kind)-cohesive.fcl"
        $firstResult = (& node $migrator $fixture.path $first | Out-String) |
            ConvertFrom-Json
        $secondResult = (& node $migrator $first $second | Out-String) |
            ConvertFrom-Json
        if ($firstResult.sourceKind -ne $fixture.kind) {
            throw "Expected $($fixture.kind) migration, observed $($firstResult.sourceKind)."
        }
        if ($secondResult.sourceKind -ne 'v3') {
            throw 'Second shell migration was not recognized as V3.'
        }
        $firstHash = (Get-FileHash -LiteralPath $first -Algorithm SHA256).Hash
        $secondHash = (Get-FileHash -LiteralPath $second -Algorithm SHA256).Hash
        if ($firstHash -ne $secondHash) {
            throw 'V3 shell migration is not byte-idempotent.'
        }
        $backup = "$first.before-player-shell-v2.fcl"
        if ((Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash -ne
                (Get-FileHash -LiteralPath $fixture.path -Algorithm SHA256).Hash) {
            throw 'Shell migration backup is not an exact source copy.'
        }
        $cohesiveResult = (& node $cohesive $first $cohesiveOutput |
            Out-String) | ConvertFrom-Json
        if ($cohesiveResult.action -ne 'already-shell-noop' -or
                (Get-FileHash -LiteralPath $cohesiveOutput -Algorithm SHA256).Hash -ne
                $firstHash) {
            throw 'Legacy cohesive installer modified a current shell.'
        }
    }
    Write-Host 'Corrective R2 shell migration tests: PASS'
}
finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction SilentlyContinue
}
