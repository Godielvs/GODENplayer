param(
    [string]$RepoRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $RepoRoot) {
    $RepoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
}

$publicSources = @(
    'src\component\daw_window.cpp',
    'src\component\listen_visual_renderer.cpp',
    'src\component\mainmenu.cpp',
    'src\component\native_panels.cpp',
    'src\component\options_window.cpp',
    'src\component\ui_preferences.cpp',
    'src\component\discord_presence_service.cpp',
    'src\component\workspace_panel.cpp',
    'src\component\wave_seek_panel.cpp'
)
$blocked = @(
    'GODEN CORE',
    'WAVE ONLY',
    'NOT ANALYZED',
    'FIXED SCALE',
    'CACHE-STABLE',
    'MS SOURCE',
    'LOCAL RPC',
    'lazy-loaded'
)
$failures = New-Object 'System.Collections.Generic.List[string]'
foreach ($relative in $publicSources) {
    $path = Join-Path $RepoRoot $relative
    $lineNumber = 0
    foreach ($line in [IO.File]::ReadLines($path)) {
        $lineNumber++
        foreach ($term in $blocked) {
            if ($line.IndexOf($term,
                    [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                $failures.Add("${relative}:${lineNumber}: blocked copy '$term'")
            }
        }
        $isDiscordProtocolReady = $relative -eq
            'src\component\discord_presence_service.cpp' -and
            ($line -match '\\"evt\\":\\"READY\\"' -or
             $line -match 'qaPresenceLog\("rpc ready"\)')
        if ($line -match 'L?"[^"]*\bREADY\b[^"]*"' -and
                -not $isDiscordProtocolReady) {
            $failures.Add("${relative}:${lineNumber}: blocked healthy-state copy 'READY'")
        }
        if ($line -match '[^\x00-\x7F]') {
            $failures.Add("${relative}:${lineNumber}: literal non-ASCII public copy must use an explicit escape")
        }
        if ($line -match 'L?"[^"]*(Mostrar|Manter|Como voc|Voc[eê]|cabe[cç]alho)[^"]*"') {
            $failures.Add("${relative}:${lineNumber}: public copy must remain English")
        }
    }
}

if ($failures.Count -ne 0) {
    throw "Public copy lint failed:`n$($failures -join "`n")"
}
Write-Host 'Public copy lint passed.'
