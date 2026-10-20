param(
    [Parameter(Mandatory=$true)][string]$DevRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($DevRoot).TrimEnd('\')
$app = Join-Path $root 'app'
$previous = Join-Path $root 'app.previous'
$swap = Join-Path $root 'app.rollback-swap'

foreach ($path in @($app, $previous, $swap)) {
    $full = [IO.Path]::GetFullPath($path)
    if (-not $full.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing rollback outside $root."
    }
}
if (-not (Test-Path -LiteralPath $app) -or -not (Test-Path -LiteralPath $previous)) {
    throw 'Both app and app.previous are required for rollback.'
}
if (Test-Path -LiteralPath $swap) {
    throw "Rollback swap path already exists: $swap"
}
$running = Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
    Where-Object {
        $_.ExecutablePath -and
        [IO.Path]::GetFullPath($_.ExecutablePath).StartsWith(
            $root + '\', [StringComparison]::OrdinalIgnoreCase)
    }
if ($running) { throw 'Close the V3 development portable before rollback.' }

Move-Item -LiteralPath $app -Destination $swap
try {
    Move-Item -LiteralPath $previous -Destination $app
    Move-Item -LiteralPath $swap -Destination $previous
}
catch {
    if (-not (Test-Path -LiteralPath $app) -and (Test-Path -LiteralPath $swap)) {
        Move-Item -LiteralPath $swap -Destination $app
    }
    throw
}
Write-Host "Swapped current and previous NekoRoX V3 development portables."

