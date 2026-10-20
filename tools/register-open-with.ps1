param(
    [Parameter(Mandatory = $true)]
    [string]$AppPath,
    [string]$IconPath = '',
    [string]$ManifestPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$app = [IO.Path]::GetFullPath($AppPath)
if (-not (Test-Path -LiteralPath $app -PathType Leaf)) {
    throw "Goden executable not found: $app"
}
if ([IO.Path]::GetFileName($app) -ne 'foobar2000.exe') {
    throw "Unexpected Goden executable: $app"
}
if (-not $ManifestPath) {
    $repo = Split-Path -Parent $PSScriptRoot
    $ManifestPath = Join-Path $repo 'artifacts\open-with-registration.json'
}

$progId = 'Goden.Audio'
$extensions = @(
    '.aac', '.aif', '.aiff', '.ape', '.cue', '.flac', '.fpl',
    '.m3u', '.m3u8', '.m4a', '.mp3', '.ogg', '.opus', '.pls',
    '.wav', '.wma', '.wv'
)
$command = "`"$app`" /show /nekorox-open `"%1`""
$icon = if ($IconPath) {
    [IO.Path]::GetFullPath($IconPath)
} else {
    "$app,0"
}
if ($IconPath -and -not (Test-Path -LiteralPath $icon -PathType Leaf)) {
    throw "Goden icon not found: $icon"
}

function Ensure-Key([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -Path $Path -Force | Out-Null
    }
}

function Set-Value(
    [string]$Path,
    [string]$Name,
    [string]$Value
) {
    Ensure-Key $Path
    New-ItemProperty -Path $Path -Name $Name -Value $Value `
        -PropertyType String -Force | Out-Null
}

function Assert-Value(
    [string]$Path,
    [string]$Name,
    [string]$Expected
) {
    $key = Get-Item -LiteralPath $Path -ErrorAction Stop
    try {
        if ($key.GetValueNames() -notcontains $Name) {
            throw "Open With registry value is missing: ${Path} [$Name]"
        }
        $actual = [string]$key.GetValue($Name)
        if ($actual -cne $Expected) {
            throw "Open With registry mismatch: ${Path} [$Name] expected [$Expected], got [$actual]"
        }
    }
    finally {
        $key.Close()
    }
}

$classes = 'HKCU:\Software\Classes'
$progIdPath = Join-Path $classes $progId
Ensure-Key $progIdPath
Set-Item -Path $progIdPath -Value 'Goden audio'
Set-Value $progIdPath 'FriendlyTypeName' 'Goden audio'

$application = Join-Path $progIdPath 'Application'
Set-Value $application 'ApplicationName' 'GODENplayer'
Set-Value $application 'ApplicationDescription' `
    'GODENplayer x64 local music player and music lab'
Set-Value $application 'ApplicationIcon' $icon

$defaultIcon = Join-Path $progIdPath 'DefaultIcon'
Ensure-Key $defaultIcon
Set-Item -Path $defaultIcon -Value $icon

$openVerb = Join-Path $progIdPath 'shell\open'
Ensure-Key $openVerb
Set-Item -Path $openVerb -Value 'Play with Goden'
Set-Value $openVerb 'MUIVerb' 'Play with Goden'
Set-Value $openVerb 'Icon' $icon
Set-Value $openVerb 'MultiSelectModel' 'Player'
$openCommand = Join-Path $openVerb 'command'
Ensure-Key $openCommand
Set-Item -Path $openCommand -Value $command

$capabilities = 'HKCU:\Software\Goden\Capabilities'
Ensure-Key $capabilities
Set-Value $capabilities 'ApplicationName' 'GODENplayer'
Set-Value $capabilities 'ApplicationDescription' `
    'GODENplayer x64 local music player and music lab'
Set-Value $capabilities 'ApplicationIcon' $icon
$fileAssociations = Join-Path $capabilities 'FileAssociations'
Ensure-Key $fileAssociations
foreach ($extension in $extensions) {
    Set-Value $fileAssociations $extension $progId
    $openWith = Join-Path $classes "$extension\OpenWithProgids"
    Set-Value $openWith $progId ''
}

$registeredApplications = 'HKCU:\Software\RegisteredApplications'
Set-Value $registeredApplications 'GODENplayer' `
    'Software\Goden\Capabilities'

$audioVerb = Join-Path $classes `
    'SystemFileAssociations\audio\shell\Goden.Play'
Ensure-Key $audioVerb
Set-Item -Path $audioVerb -Value 'Play with Goden'
Set-Value $audioVerb 'MUIVerb' 'Play with Goden'
Set-Value $audioVerb 'Icon' $icon
Set-Value $audioVerb 'MultiSelectModel' 'Player'
$audioCommand = Join-Path $audioVerb 'command'
Ensure-Key $audioCommand
Set-Item -Path $audioCommand -Value $command

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NekoRoxShellNotify {
    [DllImport("shell32.dll")]
    public static extern void SHChangeNotify(
        uint eventId, uint flags, IntPtr item1, IntPtr item2);
}
'@
[NekoRoxShellNotify]::SHChangeNotify(
    0x08000000, 0x0000, [IntPtr]::Zero, [IntPtr]::Zero)

Assert-Value $progIdPath '' 'Goden audio'
Assert-Value $progIdPath 'FriendlyTypeName' 'Goden audio'
Assert-Value $application 'ApplicationName' 'GODENplayer'
Assert-Value $application 'ApplicationDescription' `
    'GODENplayer x64 local music player and music lab'
Assert-Value $application 'ApplicationIcon' $icon
Assert-Value $openVerb '' 'Play with Goden'
Assert-Value $openVerb 'MUIVerb' 'Play with Goden'
Assert-Value $openVerb 'Icon' $icon
Assert-Value $openVerb 'MultiSelectModel' 'Player'
Assert-Value $openCommand '' $command
Assert-Value $capabilities 'ApplicationName' 'GODENplayer'
Assert-Value $capabilities 'ApplicationDescription' `
    'GODENplayer x64 local music player and music lab'
Assert-Value $capabilities 'ApplicationIcon' $icon
Assert-Value $registeredApplications 'GODENplayer' `
    'Software\Goden\Capabilities'
Assert-Value $audioVerb '' 'Play with Goden'
Assert-Value $audioVerb 'MUIVerb' 'Play with Goden'
Assert-Value $audioVerb 'Icon' $icon
Assert-Value $audioVerb 'MultiSelectModel' 'Player'
Assert-Value $audioCommand '' $command
foreach ($extension in $extensions) {
    Assert-Value $fileAssociations $extension $progId
    Assert-Value (Join-Path $classes "$extension\OpenWithProgids") `
        $progId ''
}

$manifest = [ordered]@{
    schema = 'nekorox.open-with.v2'
    registeredUtc = [DateTime]::UtcNow.ToString('o')
    appPath = $app
    iconPath = $icon
    progId = $progId
    command = $command
    extensions = $extensions
    changesDefaultAssociations = $false
    verified = $true
}
$manifestDirectory = Split-Path -Parent $ManifestPath
if ($manifestDirectory) {
    New-Item -ItemType Directory -Force -Path $manifestDirectory | Out-Null
}
$manifest | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath $ManifestPath -Encoding UTF8

Write-Host "Registered Open with Goden for $($extensions.Count) audio extensions."
Write-Host "No default file association was changed."
Write-Host "Manifest: $ManifestPath"
