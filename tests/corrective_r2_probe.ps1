param(
    [Parameter(Mandatory = $true)]
    [string]$BaselinePath,
    [string]$ExpectedHead = '414a67e1db7e0fe76913a36a7b0e58825d894505'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$baseline = Get-Content -Raw -LiteralPath $BaselinePath | ConvertFrom-Json
if ($baseline.schema -ne 'goden.corrective-r2.baseline.v1') {
    throw 'Unexpected corrective baseline schema'
}
if ($baseline.repository.head -ne $ExpectedHead) {
    throw "Baseline HEAD mismatch: $($baseline.repository.head)"
}
if (-not $baseline.installedRuntime.executable.EndsWith('foobar2000.exe')) {
    throw 'Baseline omitted the exact host executable'
}
if (-not $baseline.installedRuntime.modules -or
        -not ($baseline.installedRuntime.modules.name -contains 'foo_nekorox.dll')) {
    throw 'Baseline did not identify the loaded GODENplayer module'
}
if (-not $baseline.frozenPhysics.scrollControllerCpp.sha256 -or
        -not $baseline.frozenPhysics.scrollControllerHeader.sha256) {
    throw 'Scroll physics hashes were not frozen'
}
if (@($baseline.fixtureLibrary.files).Count -lt 8 -or
        @($baseline.fixtureLibrary.profiles).Count -ne 2) {
    throw 'Synthetic fixture matrix is incomplete'
}
if ($baseline.fixtureLibrary.containsPersonalData) {
    throw 'Synthetic fixture manifest claims personal data'
}
Write-Host 'Corrective R2 baseline probe: PASS'
