param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$InstalledExe = "$env:LOCALAPPDATA\Programs\Goden\app\foobar2000.exe",
    [string]$EvidenceRoot = '',
    [string]$ProbePath = '',
    [string]$OldLayoutPath = '',
    [string]$ShellV1LayoutPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath($RepoRoot).TrimEnd('\')
if (-not $EvidenceRoot) {
    $EvidenceRoot = Join-Path $repo ('artifacts\corrective-r2\' +
        [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ'))
}
$evidence = [IO.Path]::GetFullPath($EvidenceRoot).TrimEnd('\')
$allowed = [IO.Path]::GetFullPath(
    (Join-Path $repo 'artifacts\corrective-r2')).TrimEnd('\') + '\'
if (-not (($evidence + '\').StartsWith(
        $allowed, [StringComparison]::OrdinalIgnoreCase))) {
    throw "Evidence root escaped artifacts/corrective-r2: $evidence"
}
if (-not $ProbePath) {
    $ProbePath = Join-Path $repo 'out\Release\nekorox_ui_probe.exe'
}
if (-not $OldLayoutPath) {
    $OldLayoutPath = Join-Path $repo `
        'config\layout\nekorox-v3-cohesive-bottom.fcl.before-player-shell.fcl'
}
if (-not $ShellV1LayoutPath) {
    $ShellV1LayoutPath = Join-Path $repo `
        'config\layout\nekorox-v3-cohesive-bottom.fcl'
}

New-Item -ItemType Directory -Path $evidence -Force | Out-Null
$fixtureRoot = Join-Path $evidence 'fixtures\library'
$profileRoot = Join-Path $evidence 'profiles'
New-Item -ItemType Directory -Path $fixtureRoot, $profileRoot -Force | Out-Null

function Get-Hash([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash
}

function Write-PcmWave([string]$Path, [double]$Frequency, [int]$Seconds = 2) {
    $sampleRate = 44100
    $samples = $sampleRate * $Seconds
    $stream = [IO.File]::Create($Path)
    $writer = [IO.BinaryWriter]::new($stream)
    try {
        $dataBytes = $samples * 4
        $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'))
        $writer.Write([int](36 + $dataBytes))
        $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
        $writer.Write([int]16)
        $writer.Write([int16]1)
        $writer.Write([int16]2)
        $writer.Write([int]$sampleRate)
        $writer.Write([int]($sampleRate * 4))
        $writer.Write([int16]4)
        $writer.Write([int16]16)
        $writer.Write([Text.Encoding]::ASCII.GetBytes('data'))
        $writer.Write([int]$dataBytes)
        for ($index = 0; $index -lt $samples; ++$index) {
            $phase = 2.0 * [Math]::PI * $index / $sampleRate
            $left = [Math]::Sin($Frequency * $phase) * 0.36
            $right = [Math]::Sin(($Frequency * 1.5) * $phase + 0.2) * 0.32
            $writer.Write([int16]([Math]::Round($left * 32767.0)))
            $writer.Write([int16]([Math]::Round($right * 32767.0)))
        }
    }
    finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}

function Write-Cover([string]$Path, [int]$Argb, [string]$Label) {
    Add-Type -AssemblyName System.Drawing
    $bitmap = [Drawing.Bitmap]::new(512, 512)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.Clear([Drawing.Color]::FromArgb($Argb))
        $brush = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(235, 8, 9, 9))
        $font = [Drawing.Font]::new('Segoe UI', 32, [Drawing.FontStyle]::Bold)
        try { $graphics.DrawString($Label, $font, $brush, 28, 210) }
        finally { $font.Dispose(); $brush.Dispose() }
        $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
    }
    finally { $graphics.Dispose(); $bitmap.Dispose() }
}

$albums = @(
    [ordered]@{ artist='Synthetic North'; album='Glass Geometry'; color=0xff25c9c3;
        lyrics='synced'; discs=@(@('Arrival','Refraction'),@('Afterimage')) },
    [ordered]@{ artist='Synthetic West'; album='Long Form Signals'; color=0xffff5d8f;
        lyrics='plain'; discs=@(@('Chapter One','Chapter Two','Chapter Three')) },
    [ordered]@{ artist='Synthetic Void'; album='Instrumental Absence'; color=0xff8e72ff;
        lyrics='absent'; discs=@(@('No Words','Still No Words')) }
)
$fixtureFiles = [Collections.Generic.List[object]]::new()
$albumIndex = 0
foreach ($album in $albums) {
    ++$albumIndex
    $albumDir = Join-Path $fixtureRoot ("Album-{0}" -f $albumIndex)
    New-Item -ItemType Directory -Path $albumDir -Force | Out-Null
    $cover = Join-Path $albumDir 'folder.png'
    Write-Cover $cover $album.color $album.album
    $bio = (("{0} is a synthetic artist used only for corrective R2 QA. " -f
        $album.artist) * 80).Trim()
    [IO.File]::WriteAllText((Join-Path $albumDir 'BIO.txt'), $bio,
        [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $albumDir 'NOTES.txt'),
        "LOCAL synthetic note for $($album.album).", [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $albumDir 'PERSONA.json'),
        ([ordered]@{ source='PERSONAL'; alias=$album.artist; relations=@('Fixture Ensemble') } |
            ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
    $discNumber = 0
    foreach ($disc in $album.discs) {
        ++$discNumber
        $trackNumber = 0
        foreach ($title in $disc) {
            ++$trackNumber
            $stem = '{0:D2}-{1:D2} {2}' -f $discNumber, $trackNumber, $title
            $wave = Join-Path $albumDir ($stem + '.wav')
            Write-PcmWave $wave (150.0 + 37.0 * $albumIndex + 11.0 * $trackNumber)
            if ($album.lyrics -eq 'synced') {
                [IO.File]::WriteAllText((Join-Path $albumDir ($stem + '.lrc')),
                    "[00:00.00]$title`n[00:00.80]Synthetic synchronized line`n",
                    [Text.UTF8Encoding]::new($false))
            }
            elseif ($album.lyrics -eq 'plain') {
                [IO.File]::WriteAllText((Join-Path $albumDir ($stem + '.txt')),
                    "$title`nSynthetic plain lyric without timestamps.`n",
                    [Text.UTF8Encoding]::new($false))
            }
            $fixtureFiles.Add([ordered]@{
                path = $wave.Substring($evidence.Length + 1)
                artist = $album.artist
                album = $album.album
                disc = $discNumber
                track = $trackNumber
                title = $title
                lyrics = $album.lyrics
                sha256 = Get-Hash $wave
            })
        }
    }
}

$profiles = @()
foreach ($profile in @(
        [ordered]@{ name='old-topology'; layout=$OldLayoutPath; expectedShell='absent' },
        [ordered]@{ name='shell-v1'; layout=$ShellV1LayoutPath; expectedShell='GodenPlayerShellV1' })) {
    $directory = Join-Path $profileRoot $profile.name
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $layoutCopy = $null
    if (Test-Path -LiteralPath $profile.layout -PathType Leaf) {
        $layoutCopy = Join-Path $directory 'layout.fcl'
        Copy-Item -LiteralPath $profile.layout -Destination $layoutCopy
    }
    $descriptor = [ordered]@{
        schema='goden.corrective-r2.profile-fixture.v1'
        topology=$profile.name
        expectedShell=$profile.expectedShell
        layoutSource=[IO.Path]::GetFullPath($profile.layout)
        layoutSha256=if ($layoutCopy) { Get-Hash $layoutCopy } else { $null }
        libraryRoot=$fixtureRoot
        containsPersonalData=$false
    }
    $descriptor | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (
        Join-Path $directory 'profile-fixture.json') -Encoding UTF8
    $profiles += $descriptor
}

$installedFull = [IO.Path]::GetFullPath($InstalledExe)
$processes = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
    Where-Object { $_.ExecutablePath -and
        [IO.Path]::GetFullPath($_.ExecutablePath).Equals(
            $installedFull, [StringComparison]::OrdinalIgnoreCase) })
$runtime = [ordered]@{ state='not_running'; executable=$installedFull }
if ($processes.Count -eq 1) {
    $process = $processes[0]
    $runtime.state = 'observed_read_only'
    $runtime.processId = $process.ProcessId
    $runtime.commandLine = $process.CommandLine
    $runtime.profile = Join-Path (Split-Path -Parent $installedFull) 'profile'
    $managed = Get-Process -Id $process.ProcessId
    $modules = @($managed.Modules | Where-Object {
        $_.ModuleName -in 'foo_nekorox.dll','foo_vis_spectrum_analyzer.dll' } |
        ForEach-Object { [ordered]@{ name=$_.ModuleName; path=$_.FileName;
            sha256=Get-Hash $_.FileName } })
    $runtime.modules = $modules
    if (Test-Path -LiteralPath $ProbePath -PathType Leaf) {
        $treePath = Join-Path $evidence 'installed-window-tree.txt'
        $tree = & $ProbePath $installedFull 'window-tree' 2>&1 | Out-String
        [IO.File]::WriteAllText($treePath, $tree, [Text.UTF8Encoding]::new($false))
        $runtime.windowTree = $treePath
        $runtime.shellV1Observed = $tree.Contains('GodenPlayerShellV1')
        $runtime.shellV2Observed = $tree.Contains('GodenPlayerShellV2')
        $runtime.workspaceObserved = $tree.Contains('NekoRoXWorkspaceV3')
        $runtime.coverObserved = $tree.Contains('NekoRoXCoverV3')
    }
}
elseif ($processes.Count -gt 1) { throw 'Multiple exact installed processes observed' }

$flagNames = Select-String -LiteralPath (Join-Path $repo 'src\component\rollout_flags.cpp') `
    -Pattern 'L"GODEN_FLAG_[A-Z0-9_]+"' -AllMatches | ForEach-Object {
        $_.Matches.Value.Trim('L','"')
    }
$flags = foreach ($name in $flagNames) {
    $safeWrite = $name -in 'GODEN_FLAG_TAG_EDITOR_V1','GODEN_FLAG_TAG_IMPORT_V1'
    [ordered]@{
        name=$name
        sourceDefault=(-not $safeWrite)
        collectorProcessOverride=[Environment]::GetEnvironmentVariable($name, 'Process')
        userOverride=[Environment]::GetEnvironmentVariable($name, 'User')
        machineOverride=[Environment]::GetEnvironmentVariable($name, 'Machine')
        remoteProcessEnvironmentReadable=$false
    }
}

$status = @(git -C $repo -c core.quotepath=false status --porcelain=v1 -uall)
$workspace = Get-Content -LiteralPath (Join-Path $repo 'src\component\workspace_panel.cpp')
$wheelLines = @($workspace | Select-String -Pattern 'addWheelImpulseAt|addWheelImpulse\(' |
    ForEach-Object { [ordered]@{ line=$_.LineNumber; text=$_.Line.Trim() } })
$scrollCpp = Join-Path $repo 'src\core\src\scroll_controller.cpp'
$scrollHeader = Join-Path $repo 'src\core\include\nekorox\scroll_controller.h'
$manifest = [ordered]@{
    schema='goden.corrective-r2.baseline.v1'
    capturedUtc=[DateTime]::UtcNow.ToString('o')
    repository=[ordered]@{
        root=$repo
        head=(git -C $repo rev-parse HEAD).Trim()
        branch=(git -C $repo branch --show-current).Trim()
        status=$status
    }
    frozenPhysics=[ordered]@{
        scrollControllerCpp=[ordered]@{ path=$scrollCpp; sha256=Get-Hash $scrollCpp }
        scrollControllerHeader=[ordered]@{ path=$scrollHeader; sha256=Get-Hash $scrollHeader }
        callerWheelLines=$wheelLines
    }
    rolloutFlags=$flags
    installedRuntime=$runtime
    fixtureLibrary=[ordered]@{
        root=$fixtureRoot
        containsPersonalData=$false
        files=$fixtureFiles
        profiles=$profiles
    }
    qualification=[ordered]@{
        productRuntimeObserved=($runtime.state -eq 'observed_read_only')
        shellHostObserved=($runtime.shellV1Observed -or $runtime.shellV2Observed)
        syntheticFixturesPrepared=$true
        pacingDisplayQualified=$false
        pacingReason='No external display trace collected in C00'
    }
}
$path = Join-Path $evidence 'BASELINE.json'
$manifest | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $path -Encoding UTF8
$fixtureFiles | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (
    Join-Path $evidence 'fixtures\FIXTURES.json') -Encoding UTF8
Write-Host "Corrective R2 baseline: $path"
Write-Host "Installed runtime: $($runtime.state)"
Write-Host "Shell host observed: $($manifest.qualification.shellHostObserved)"

