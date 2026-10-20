Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$fixtureRoot = Join-Path $repo 'artifacts\profile-policy-test'
$profile = Join-Path $fixtureRoot 'profile'

if (Test-Path -LiteralPath $fixtureRoot) {
    Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
}

New-Item -ItemType Directory -Force -Path `
    (Join-Path $profile 'user-components-x64\foo_nekorox'), `
    (Join-Path $profile 'user-components-x64\foo_youtube'), `
    (Join-Path $profile 'yttm\art_img'), `
    (Join-Path $profile 'yttm\review'), `
    (Join-Path $profile 'custom\legacy'), `
    (Join-Path $profile 'foo_webview2\EBWebView'), `
    (Join-Path $profile 'configuration') | Out-Null

Set-Content -LiteralPath (Join-Path $profile 'yttm\art_img\keep.jpg') -Value 'art'
Set-Content -LiteralPath (Join-Path $profile 'yttm\foo_lastfm_img.vbs') -Value 'legacy'
Set-Content -LiteralPath (Join-Path $profile 'custom\legacy\old.js') -Value 'legacy'
Set-Content -LiteralPath `
    (Join-Path $profile 'configuration\foo_youtube.dll.cfg') -Value 'legacy'
Set-Content -LiteralPath `
    (Join-Path $profile 'configuration\foo_nekorox.dll.cfg') -Value 'keep'

$dryManifest = Join-Path $fixtureRoot 'dry.json'
& "$PSScriptRoot\normalize-profile.ps1" -ProfileRoot $profile -DevRoot $fixtureRoot `
    -ManifestPath $dryManifest
$dry = Get-Content -LiteralPath $dryManifest -Raw | ConvertFrom-Json
if ($dry.operations.Count -ne 5 -or $dry.reclaimed.bytes -ne 0 -or `
    $dry.reclaimable.bytes -le 0) {
    throw 'Profile normalization dry-run contract failed.'
}

$applyManifest = Join-Path $fixtureRoot 'apply.json'
& "$PSScriptRoot\normalize-profile.ps1" -ProfileRoot $profile -DevRoot $fixtureRoot `
    -ManifestPath $applyManifest -Apply
$remaining = @(Get-ChildItem -LiteralPath $profile -File -Recurse | ForEach-Object {
    $_.FullName.Substring($profile.Length + 1).Replace('\', '/')
})
$apply = Get-Content -LiteralPath $applyManifest -Raw | ConvertFrom-Json
if ($remaining.Count -ne 2 -or `
    $remaining -notcontains 'yttm/art_img/keep.jpg' -or `
    $remaining -notcontains 'configuration/foo_nekorox.dll.cfg' -or `
    $apply.operations.Count -ne 5 -or $apply.reclaimed.bytes -le 0) {
    throw 'Profile normalization apply contract failed.'
}

$secondManifest = Join-Path $fixtureRoot 'idempotent.json'
& "$PSScriptRoot\normalize-profile.ps1" -ProfileRoot $profile -DevRoot $fixtureRoot `
    -ManifestPath $secondManifest -Apply
$second = Get-Content -LiteralPath $secondManifest -Raw | ConvertFrom-Json
if ($second.operations.Count -ne 0 -or $second.reclaimed.bytes -ne 0) {
    throw 'Profile normalization is not idempotent.'
}

Write-Host 'Profile policy tests passed.'
