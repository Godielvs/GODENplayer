param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$progId = 'Goden.Audio'
$extensions = @(
    '.aac', '.aif', '.aiff', '.ape', '.cue', '.flac', '.fpl',
    '.m3u', '.m3u8', '.m4a', '.mp3', '.ogg', '.opus', '.pls',
    '.wav', '.wma', '.wv'
)
$classes = 'HKCU:\Software\Classes'

foreach ($extension in $extensions) {
    $openWith = Join-Path $classes "$extension\OpenWithProgids"
    if (Test-Path -LiteralPath $openWith) {
        Remove-ItemProperty -LiteralPath $openWith -Name $progId `
            -ErrorAction SilentlyContinue
    }
}

$ownedKeys = @(
    (Join-Path $classes $progId),
    (Join-Path $classes 'SystemFileAssociations\audio\shell\Goden.Play'),
    'HKCU:\Software\Goden'
)
foreach ($path in $ownedKeys) {
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Recurse -Force
    }
}
Remove-ItemProperty -LiteralPath 'HKCU:\Software\RegisteredApplications' `
    -Name 'Goden foobar2000' -ErrorAction SilentlyContinue

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

Write-Host 'Removed Open with Goden registration.'
Write-Host 'Existing user-selected default associations were not changed.'
