param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$DevRoot = '',
    [string]$ManifestPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if (-not $DevRoot) {
    $DevRoot = Join-Path $repo 'artifacts\interaction-portable'
}
$devRootFull = [IO.Path]::GetFullPath($DevRoot).TrimEnd('\')
$allowedRoot = [IO.Path]::GetFullPath(
    (Join-Path $repo 'artifacts')).TrimEnd('\') + '\'
if (-not $devRootFull.StartsWith(
        $allowedRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Interaction QA is restricted to disposable artifacts: $devRootFull"
}

$app = Join-Path $devRootFull 'app\foobar2000.exe'
$probe = Join-Path $repo "out\$Configuration\nekorox_ui_probe.exe"
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repo 'artifacts\interaction-qa.json'
}
foreach ($required in @($app, $probe)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Interaction QA input is missing: $required"
    }
}

function Get-AppProcess {
    $matches = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
        Where-Object {
            $_.ExecutablePath -and
            [IO.Path]::GetFullPath($_.ExecutablePath).Equals(
                $app, [StringComparison]::OrdinalIgnoreCase)
        })
    if ($matches.Count -gt 1) {
        throw 'Multiple disposable foobar2000 processes are running.'
    }
    if ($matches.Count -eq 0) { return $null }
    try { return Get-Process -Id $matches[0].ProcessId -ErrorAction Stop }
    catch [Microsoft.PowerShell.Commands.ProcessCommandException] { return $null }
}

function Wait-App([bool]$Present, [int]$TimeoutMs = 15000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $process = Get-AppProcess
        if ($Present -and $null -ne $process -and
                $process.MainWindowHandle -ne 0) {
            return $process
        }
        if (-not $Present -and $null -eq $process) { return $null }
        Start-Sleep -Milliseconds 50
    } while ($watch.ElapsedMilliseconds -lt $TimeoutMs)
    $verb = if ($Present) { 'start' } else { 'exit' }
    throw "Disposable foobar2000 did not $verb within ${TimeoutMs}ms."
}

function Invoke-Probe(
    [string]$Action,
    [string]$Argument = '',
    [switch]$AllowFailure
) {
    $arguments = @($app, $Action)
    if ($Argument) { $arguments += $Argument }
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $probe @arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    $text = ($output | ForEach-Object { $_.ToString() }) -join "`n"
    if ($exitCode -ne 0) {
        if ($AllowFailure) { return $null }
        throw "UI probe '$Action' failed with exit code ${exitCode}: $text"
    }
    return $text
}

function Get-TransportState {
    $text = Invoke-Probe 'transport-state'
    if ($text -notmatch
            'playing=(true|false)\s+muted=(true|false)\s+repeat=(true|false)\s+shuffle=(true|false)\s+costMap=(true|false)\s+volume=(\d+)\s+order=(\d+)') {
        throw "Could not parse transport state: $text"
    }
    return [pscustomobject]@{
        Playing = $Matches[1] -eq 'true'
        Muted = $Matches[2] -eq 'true'
        Repeat = $Matches[3] -eq 'true'
        Shuffle = $Matches[4] -eq 'true'
        CostMap = $Matches[5] -eq 'true'
        Volume = [int]$Matches[6]
        Order = [int]$Matches[7]
        Raw = $text
    }
}

function Get-FileOpenState {
    $text = Invoke-Probe 'file-open-state'
    if ($text -notmatch
            'temporaryAlbum=(true|false)\s+dismissed=(true|false)\s+temporaryItems=(\d+)') {
        throw "Could not parse temporary-album state: $text"
    }
    return [pscustomobject]@{
        Active = $Matches[1] -eq 'true'
        Dismissed = $Matches[2] -eq 'true'
        Items = [int]$Matches[3]
        Raw = $text
    }
}

function Get-TemporaryWorkspaceState {
    $text = Invoke-Probe 'temporary-workspace-state'
    if ($text -notmatch
            'active=(true|false)\s+tracks=(\d+)\s+groups=(\d+)\s+libraryTracks=(\d+)\s+totalGroups=(\d+)') {
        throw "Could not parse temporary Workspace state: $text"
    }
    return [pscustomobject]@{
        Active = $Matches[1] -eq 'true'
        Tracks = [int]$Matches[2]
        Groups = [int]$Matches[3]
        LibraryTracks = [int]$Matches[4]
        TotalGroups = [int]$Matches[5]
        Raw = $text
    }
}

function Wait-TemporaryWorkspace(
    [bool]$Active,
    [int]$Tracks,
    [int]$Groups,
    [int]$TimeoutMs = 15000
) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $state = Get-TemporaryWorkspaceState
        if ($state.Active -eq $Active -and
                ($Tracks -lt 0 -or $state.Tracks -eq $Tracks) -and
                ($Groups -lt 0 -or $state.Groups -eq $Groups)) {
            return $state
        }
        Start-Sleep -Milliseconds 100
    } while ($watch.ElapsedMilliseconds -lt $TimeoutMs)
    throw "Temporary Workspace did not reach active=$Active tracks=$Tracks groups=$Groups. Last: $($state.Raw)"
}

function Wait-TemporaryAlbum(
    [bool]$Active,
    [int]$MinimumItems = 0,
    [bool]$Dismissed = $false,
    [int]$TimeoutMs = 15000
) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $state = Get-FileOpenState
        if ($state.Active -eq $Active -and
                $state.Items -ge $MinimumItems -and
                (-not $Dismissed -or $state.Dismissed)) {
            return $state
        }
        Start-Sleep -Milliseconds 50
    } while ($watch.ElapsedMilliseconds -lt $TimeoutMs)
    throw "Temporary album did not reach the requested state: $($state.Raw)"
}

function Get-LibraryCount {
    $text = Invoke-Probe 'library-count'
    if ($text -notmatch 'Library tracks=(\d+)') {
        throw "Could not parse library count: $text"
    }
    return [int]$Matches[1]
}

function Get-WindowState {
    return Invoke-Probe 'window-state'
}

function Wait-WindowTitle([string]$Needle, [int]$TimeoutMs = 10000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $state = Get-WindowState
        if ($state.IndexOf(
                $Needle, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            return $state
        }
        Start-Sleep -Milliseconds 50
    } while ($watch.ElapsedMilliseconds -lt $TimeoutMs)
    throw "Window title did not contain '$Needle': $state"
}

function Wait-Playing([int]$TimeoutMs = 10000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $state = Get-TransportState
        if ($state.Playing) { return $state }
        Start-Sleep -Milliseconds 50
    } while ($watch.ElapsedMilliseconds -lt $TimeoutMs)
    throw "Playback did not start within ${TimeoutMs}ms: $($state.Raw)"
}

function Get-Sha256([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try {
        return ([BitConverter]::ToString(
            $algorithm.ComputeHash($stream))).Replace('-', '')
    }
    finally {
        $stream.Dispose()
        $algorithm.Dispose()
    }
}

function Write-TestWave([string]$Path, [double]$Frequency) {
    $directory = Split-Path -Parent $Path
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $sampleRate = 44100
    $samples = [int]($sampleRate * 8.0)
    $dataBytes = $samples * 2
    $stream = [IO.File]::Create($Path)
    $writer = New-Object IO.BinaryWriter($stream)
    try {
        $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'))
        $writer.Write([int](36 + $dataBytes))
        $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVE'))
        $writer.Write([Text.Encoding]::ASCII.GetBytes('fmt '))
        $writer.Write([int]16)
        $writer.Write([int16]1)
        $writer.Write([int16]1)
        $writer.Write([int]$sampleRate)
        $writer.Write([int]($sampleRate * 2))
        $writer.Write([int16]2)
        $writer.Write([int16]16)
        $writer.Write([Text.Encoding]::ASCII.GetBytes('data'))
        $writer.Write([int]$dataBytes)
        for ($index = 0; $index -lt $samples; $index++) {
            $sample = [Math]::Sin(
                2.0 * [Math]::PI * $Frequency * $index / $sampleRate)
            $writer.Write([int16]([Math]::Round($sample * 1800.0)))
        }
    }
    finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}

$evidenceRoot = Join-Path $repo 'artifacts\interaction-qa'
New-Item -ItemType Directory -Force -Path $evidenceRoot | Out-Null
$menuCapture = Join-Path $evidenceRoot 'native-menu.bmp'
$dragFixture = Join-Path $evidenceRoot 'nekorox-drag-fixture.wav'
$openFixture = Join-Path $evidenceRoot 'nekorox-openwith-fixture.wav'
$temporaryAlbumFolder = Join-Path $evidenceRoot 'temporary-album-folder'
$temporaryAlbumZip = Join-Path $evidenceRoot 'temporary-album.zip'
Write-TestWave $dragFixture 330.0
Write-TestWave $openFixture 550.0
Write-TestWave (Join-Path $temporaryAlbumFolder '01-temporary-alpha.wav') 440.0
Write-TestWave (Join-Path $temporaryAlbumFolder '02-temporary-beta.wav') 660.0
if (Test-Path -LiteralPath $temporaryAlbumZip) {
    Remove-Item -LiteralPath $temporaryAlbumZip -Force
}
Compress-Archive -Path (Join-Path $temporaryAlbumFolder '*') `
    -DestinationPath $temporaryAlbumZip -CompressionLevel Fastest

$result = [ordered]@{
    schema = 'nekorox.interaction-qa.v1'
    generatedUtc = [DateTime]::UtcNow.ToString('o')
    app = $app
    libraryTracks = 0
    transport = [ordered]@{}
    cover = ''
    randomAlbumNowPlayingReveal = ''
    menu = [ordered]@{}
    notesRoundTrip = $false
    dragDrop = [ordered]@{}
    temporaryAlbum = [ordered]@{}
    openWith = [ordered]@{}
    displayMinimum = [ordered]@{}
    passed = $false
}

try {
    if ($null -eq (Get-AppProcess)) {
        Start-Process -FilePath $app
    }
    $process = Wait-App $true
    $dpiState = Invoke-Probe 'window-dpi-state'
    if ($dpiState -notmatch
            'dpi=(\d+)\s+awareness=2\s+physical=(\d+)x(\d+)\s+work=(\d+)x(\d+)') {
        throw "Could not parse Per-Monitor V2 state: $dpiState"
    }
    $dpi = [int]$Matches[1]
    $workWidth = [int]$Matches[4]
    $workHeight = [int]$Matches[5]
    $minimum = Invoke-Probe 'window-resize' '640x480'
    if ($minimum -notmatch 'Resized main window to (\d+)x(\d+)') {
        throw "Could not parse safe minimum: $minimum"
    }
    $actualWidth = [int]$Matches[1]
    $actualHeight = [int]$Matches[2]
    $expectedWidth = [Math]::Min(
        [int][Math]::Floor((720 * $dpi + 48) / 96), $workWidth)
    $expectedHeight = [Math]::Min(
        [int][Math]::Floor((620 * $dpi + 48) / 96), $workHeight)
    if ($actualWidth -ne $expectedWidth -or $actualHeight -ne $expectedHeight) {
        throw "Unsafe main minimum: expected ${expectedWidth}x${expectedHeight}, got ${actualWidth}x${actualHeight}."
    }
    $result.displayMinimum = [ordered]@{
        dpiState = $dpiState
        requested = '640x480'
        actual = "${actualWidth}x${actualHeight}"
    }
    Invoke-Probe 'window-native-maximize' | Out-Null
    Start-Sleep -Milliseconds 250

    Invoke-Probe 'mute-on' | Out-Null
    $result.libraryTracks = Get-LibraryCount

    Invoke-Probe 'lib' | Out-Null
    Invoke-Probe 'play-first-album' | Out-Null
    $null = Wait-Playing
    $result.randomAlbumNowPlayingReveal =
        Invoke-Probe 'verify-random-album-now-playing-reveal'
    $null = Wait-Playing

    $state = Get-TransportState
    if ($state.Repeat) { Invoke-Probe 'repeat-toggle' | Out-Null }
    if ((Get-TransportState).Shuffle) {
        Invoke-Probe 'shuffle-toggle' | Out-Null
    }

    Invoke-Probe 'repeat-toggle' | Out-Null
    $repeatOn = Get-TransportState
    if (-not $repeatOn.Repeat -or $repeatOn.Order -eq 0) {
        throw "Repeat did not become active: $($repeatOn.Raw)"
    }
    Invoke-Probe 'repeat-toggle' | Out-Null
    $repeatOff = Get-TransportState
    if ($repeatOff.Repeat) {
        throw "Repeat did not return to default: $($repeatOff.Raw)"
    }

    Invoke-Probe 'shuffle-toggle' | Out-Null
    $shuffleOn = Get-TransportState
    if (-not $shuffleOn.Shuffle -or $shuffleOn.Order -eq 0) {
        throw "Shuffle did not become active: $($shuffleOn.Raw)"
    }
    Invoke-Probe 'shuffle-toggle' | Out-Null
    $shuffleOff = Get-TransportState
    if ($shuffleOff.Shuffle) {
        throw "Shuffle did not return to default: $($shuffleOff.Raw)"
    }

    Invoke-Probe 'volume-set' '37' | Out-Null
    $volume37 = Get-TransportState
    if ($volume37.Muted -or $volume37.Volume -lt 330 -or
            $volume37.Volume -gt 430) {
        throw "Volume did not track the 37% pointer: $($volume37.Raw)"
    }
    Invoke-Probe 'mute-toggle' | Out-Null
    $muted = Get-TransportState
    if (-not $muted.Muted) {
        throw "Mute did not become active: $($muted.Raw)"
    }
    Invoke-Probe 'mute-toggle' | Out-Null
    $unmuted = Get-TransportState
    if ($unmuted.Muted) {
        throw "Mute did not restore volume: $($unmuted.Raw)"
    }
    Invoke-Probe 'volume-set' '0' | Out-Null

    $result.transport = [ordered]@{
        repeatOn = $repeatOn.Raw
        repeatOff = $repeatOff.Raw
        shuffleOn = $shuffleOn.Raw
        shuffleOff = $shuffleOff.Raw
        volume37 = $volume37.Raw
        muted = $muted.Raw
        unmuted = $unmuted.Raw
    }

    Invoke-Probe 'menu-capture' $menuCapture | Out-Null
    if (-not (Test-Path -LiteralPath $menuCapture) -or
            (Get-Item -LiteralPath $menuCapture).Length -lt 1024) {
        throw 'Native menu capture is missing or empty.'
    }
    $result.menu = [ordered]@{
        path = $menuCapture
        sha256 = Get-Sha256 $menuCapture
        windowStateAfterClose = Get-WindowState
    }

    Start-Sleep -Milliseconds 1200
    $cover = Invoke-Probe 'cover-state'
    if ($cover -notmatch
            'track=true\s+pixels=true\s+bitmap=true.*cacheReady=true') {
        throw "Track-first cover did not become ready: $cover"
    }
    $result.cover = $cover

    # PlayerShell V2 intentionally owns the immersive views and embeds the
    # Workspace in Library-only mode. The retired INF/BIO/LYR/ART tab hit
    # targets are therefore not valid interaction gates for this topology.
    Invoke-Probe 'lib' | Out-Null
    foreach ($scroll in @('scroll-artist', 'scroll-album', 'scroll-results')) {
        $forward = Invoke-Probe $scroll '12' -AllowFailure
        $backward = Invoke-Probe $scroll '-12' -AllowFailure
        if ($null -eq $forward -and $null -eq $backward) {
            throw "UI probe '$scroll' could not move in either direction."
        }
    }
    $result.notesRoundTrip = $false

    $beforeDrop = Get-LibraryCount
    Invoke-Probe 'drop-file' $dragFixture | Out-Null
    $dragTitle = Wait-WindowTitle 'nekorox-drag-fixture'
    $afterDrop = Get-LibraryCount
    if ($afterDrop -ne $beforeDrop) {
        throw "Drag-and-drop changed the library: $beforeDrop -> $afterDrop"
    }
    $result.dragDrop = [ordered]@{
        title = $dragTitle
        libraryBefore = $beforeDrop
        libraryAfter = $afterDrop
    }

    Invoke-Probe 'drop-file' $temporaryAlbumFolder | Out-Null
    $folderAlbum = Wait-TemporaryAlbum $true 2
    $folderWorkspace = Wait-TemporaryWorkspace $true 2 1
    if ($folderWorkspace.LibraryTracks -ne $beforeDrop -or
            $folderWorkspace.TotalGroups -le 1) {
        throw "Temporary album replaced the library instead of coexisting: $($folderWorkspace.Raw)"
    }
    $folderFirst = Wait-WindowTitle '01-temporary-alpha'
    Invoke-Probe 'next' | Out-Null
    $folderSecond = Wait-WindowTitle '02-temporary-beta'

    Invoke-Probe 'play-first-library-after-temporary' | Out-Null
    $null = Wait-Playing
    $dismissedAlbum = Wait-TemporaryAlbum $false 0 $true
    $dismissedWorkspace = Wait-TemporaryWorkspace $false 0 -1

    Invoke-Probe 'drop-file' $temporaryAlbumZip | Out-Null
    $zipAlbum = Wait-TemporaryAlbum $true 2
    $zipWorkspace = Wait-TemporaryWorkspace $true 2 1
    if ($zipWorkspace.LibraryTracks -ne $beforeDrop -or
            $zipWorkspace.TotalGroups -le 1) {
        throw "ZIP temporary album replaced the library instead of coexisting: $($zipWorkspace.Raw)"
    }
    $zipFirst = Wait-WindowTitle '01-temporary-alpha'
    Invoke-Probe 'next' | Out-Null
    $zipSecond = Wait-WindowTitle '02-temporary-beta'
    $result.temporaryAlbum = [ordered]@{
        folder = $folderAlbum.Raw
        folderWorkspace = $folderWorkspace.Raw
        folderFirst = $folderFirst
        folderSecond = $folderSecond
        dismissed = $dismissedAlbum.Raw
        dismissedWorkspace = $dismissedWorkspace.Raw
        zip = $zipAlbum.Raw
        zipWorkspace = $zipWorkspace.Raw
        zipFirst = $zipFirst
        zipSecond = $zipSecond
    }

    $openProcess = Start-Process -FilePath $app -PassThru -ArgumentList @(
        '/show', '/nekorox-open', "`"$openFixture`"")
    $null = $openProcess.WaitForExit(5000)
    $openTitle = Wait-WindowTitle 'nekorox-openwith-fixture'
    $afterOpen = Get-LibraryCount
    if ($afterOpen -ne $beforeDrop) {
        throw "Open With changed the library: $beforeDrop -> $afterOpen"
    }
    $openState = Get-TransportState
    if (-not $openState.Playing) {
        throw "Open With did not start playback: $($openState.Raw)"
    }
    $result.openWith = [ordered]@{
        title = $openTitle
        libraryBefore = $beforeDrop
        libraryAfter = $afterOpen
        transport = $openState.Raw
    }

    $result.passed = $true
}
finally {
    try { & $app /exit | Out-Null } catch {}
    try { Wait-App $false 10000 | Out-Null } catch {}
    $manifestDirectory = Split-Path -Parent $ManifestPath
    New-Item -ItemType Directory -Force -Path $manifestDirectory | Out-Null
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ManifestPath -Encoding UTF8
}

if (-not $result.passed) {
    throw 'NekoRoX interaction QA failed.'
}

Write-Host 'NekoRoX interaction QA: PASS'
Write-Host "Manifest: $ManifestPath"
