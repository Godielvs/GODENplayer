[CmdletBinding()]
param(
    [string]$Manifest = (Join-Path $PSScriptRoot '..\packaging\presence-assets-manifest.json')
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$document = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
if ($document.schemaVersion -ne 1) { throw 'Unsupported presence asset manifest schema.' }
if ($document.applicationId -notmatch '^\d{17,20}$') { throw 'Invalid Discord application ID.' }
if (-not $document.assets -or $document.assets.Count -eq 0) { throw 'No presence assets declared.' }

$keys = @{}
foreach ($asset in $document.assets) {
    if ($asset.key -notmatch '^[a-z0-9_]{2,32}$') {
        throw "Invalid Discord asset key: $($asset.key)"
    }
    if ($keys.ContainsKey($asset.key)) { throw "Duplicate asset key: $($asset.key)" }
    $keys[$asset.key] = $true
    $source = Join-Path $repo $asset.source
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Missing source asset: $source"
    }
    $actual = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    if ($actual -ne $asset.sourceSha256) {
        throw "Hash mismatch for $($asset.key): $actual"
    }
}

[pscustomobject]@{
    applicationId = $document.applicationId
    assets = $document.assets.Count
    localVerification = 'PASS'
    externalVerification = $document.verification.status
} | ConvertTo-Json
