param(
    [switch]$RequireSocialSdk,
    [switch]$SkipNetwork
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$headerPath = Join-Path $repo 'src\component\presence_deployment.h'
$assetsPath = Join-Path $repo 'packaging\presence-assets-manifest.json'
$header = Get-Content -LiteralPath $headerPath -Raw

function Read-HeaderValue([string]$Name) {
    $match = [regex]::Match($header,
        ('inline constexpr std::string_view {0}\{{(?:"([^"]*)")?\}};' -f
            [regex]::Escape($Name)))
    if (-not $match.Success) {
        throw "Presence deployment value '$Name' is missing."
    }
    return $match.Groups[1].Value
}

$applicationId = Read-HeaderValue 'applicationId'
$relayEndpoint = (Read-HeaderValue 'relayEndpoint').TrimEnd('/')
if ($applicationId -notmatch '^[0-9]{16,24}$') {
    throw 'Official Discord Application ID has not been configured.'
}
if ($relayEndpoint -notmatch '^https://') {
    throw 'Production HTTPS presence relay has not been configured.'
}

$assets = Get-Content -LiteralPath $assetsPath -Raw | ConvertFrom-Json
if ($assets.applicationId -ne $applicationId) {
    throw 'Presence asset manifest belongs to a different Discord application.'
}
$requiredKeys = @('goden_default', 'goden_playing', 'goden_daw')
foreach ($key in $requiredKeys) {
    $entry = @($assets.assets | Where-Object key -EQ $key)
    if ($entry.Count -ne 1 -or -not $entry[0].source) {
        throw "Presence asset '$key' is missing or ambiguous."
    }
    $path = Join-Path $repo $entry[0].source
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Presence asset '$key' is missing: $path"
    }
}
& (Join-Path $PSScriptRoot 'verify-presence-assets.ps1') -Manifest $assetsPath |
    Out-Null

if (-not $SkipNetwork) {
    $health = Invoke-RestMethod -Uri "$relayEndpoint/health" -TimeoutSec 10
    if (-not $health.healthy -or $health.service -ne 'goden-presence-relay') {
        throw 'The configured relay did not pass its health check.'
    }
}

$sdkRoot = Join-Path $repo 'third_party\discord-social-sdk'
$sdkReady = (Test-Path -LiteralPath (Join-Path $sdkRoot 'include\discordpp.h')) -and
    (Test-Path -LiteralPath (Join-Path $sdkRoot 'bin\discord_partner_sdk.dll'))
if ($RequireSocialSdk -and -not $sdkReady) {
    throw 'Discord Social SDK portal package is not installed in third_party\discord-social-sdk.'
}

Write-Host 'GODENplayer Presence deployment: PASS'
Write-Host "  Application ID: $applicationId"
Write-Host "  Relay: $relayEndpoint"
Write-Host ('  Social SDK package: ' + $(if ($sdkReady) { 'ready' } else { 'IPC rollback only' }))
