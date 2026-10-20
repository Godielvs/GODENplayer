param(
    [string]$InstallRoot = "$env:LOCALAPPDATA\Programs\Goden",
    [string]$LegacyInstallRoot = "$env:LOCALAPPDATA\Programs\NekoRoX",
    [switch]$Force,
    [switch]$Launch
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = [IO.Path]::GetFullPath($InstallRoot).TrimEnd('\')
$app = Join-Path $root 'app'
$previous = Join-Path $root 'app.previous'
$temporary = Join-Path $root 'app.rollback-temp'
$legacyApp = Join-Path ([IO.Path]::GetFullPath(
    $LegacyInstallRoot).TrimEnd('\')) 'app'

function Assert-ChildPath([string]$Path, [string]$Parent) {
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $parent = [IO.Path]::GetFullPath($Parent).TrimEnd('\') + '\'
    if (-not ($full + '\').StartsWith(
            $parent, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Rollback path escaped Goden root: $full"
    }
}

foreach ($path in @($app, $previous, $temporary)) {
    Assert-ChildPath $path $root
}
if (Test-Path -LiteralPath $temporary) {
    throw "An incomplete rollback staging directory exists: $temporary"
}

$running = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
    Where-Object {
        $_.ExecutablePath -and
        ([IO.Path]::GetFullPath($_.ExecutablePath) + '\').StartsWith(
            $root + '\', [StringComparison]::OrdinalIgnoreCase)
    })
if ($running.Count -gt 0) {
    foreach ($executable in @($running.ExecutablePath | Select-Object -Unique)) {
        $close = Start-Process -FilePath $executable -ArgumentList '/exit' `
            -Wait -PassThru -WindowStyle Hidden
        if ($close.ExitCode -ne 0) {
            Write-Warning "Graceful close returned $($close.ExitCode): $executable"
        }
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        $remaining = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
            Where-Object {
                $_.ExecutablePath -and
                ([IO.Path]::GetFullPath($_.ExecutablePath) + '\').StartsWith(
                    $root + '\', [StringComparison]::OrdinalIgnoreCase)
            })
        if ($remaining.Count -eq 0) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($remaining.Count -gt 0) {
        if (-not $Force) {
            throw 'GODENplayer did not close cleanly. Retry after closing it, or use -Force.'
        }
        foreach ($process in $remaining) {
            Stop-Process -Id $process.ProcessId -Force
        }
        Wait-Process -Id @($remaining.ProcessId) -Timeout 5 `
            -ErrorAction SilentlyContinue
    }
}

if (Test-Path -LiteralPath (Join-Path $previous 'foobar2000.exe')) {
    Move-Item -LiteralPath $app -Destination $temporary
    try {
        Move-Item -LiteralPath $previous -Destination $app
        Move-Item -LiteralPath $temporary -Destination $previous
    }
    catch {
        if (-not (Test-Path -LiteralPath $app) -and
            (Test-Path -LiteralPath $temporary)) {
            Move-Item -LiteralPath $temporary -Destination $app
        }
        throw
    }
    & (Join-Path $PSScriptRoot 'promote-user-install.ps1') `
        -SourceApp $app `
        -InstallRoot $root `
        -LegacyInstallRoot (Split-Path -Parent $legacyApp) `
        -RegistrationOnly
    $rollbackExe = Join-Path $app 'foobar2000.exe'
    Write-Host "Rolled Goden back to app.previous: $rollbackExe"
} elseif (Test-Path -LiteralPath (Join-Path $legacyApp 'foobar2000.exe')) {
    $sourceProfile = Join-Path $app 'profile'
    $legacyProfile = Join-Path $legacyApp 'profile'
    if (Test-Path -LiteralPath $sourceProfile) {
        & robocopy.exe $sourceProfile $legacyProfile /E /COPY:DAT /DCOPY:T `
            /R:2 /W:1 /XJ /NFL /NDL /NJH /NJS `
            /XD (Join-Path $sourceProfile 'crash reports') `
            /XF running nobadshutdown startup.incomplete | Out-Null
        if ($LASTEXITCODE -gt 7) {
            throw "Rollback profile synchronization failed: $LASTEXITCODE"
        }
    }
    $rollbackExe = Join-Path $legacyApp 'foobar2000.exe'
    $registrationBackup = Join-Path $root 'registration-backup'
    $registryBackups = @(Get-ChildItem -LiteralPath $registrationBackup `
        -Filter '*.reg' -File -ErrorAction SilentlyContinue)
    if ($registryBackups.Count -gt 0) {
        foreach ($backup in $registryBackups) {
            & reg.exe import $backup.FullName | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw "Could not restore registry backup: $($backup.FullName)"
            }
        }
    } else {
        $appPaths = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\App Paths\foobar2000.exe'
        New-Item -Path $appPaths -Force | Out-Null
        Set-Item -Path $appPaths -Value $rollbackExe
        New-ItemProperty -Path $appPaths -Name Path -Value $legacyApp `
            -PropertyType String -Force | Out-Null
    }

    $godenShortcutGroup = Join-Path $env:APPDATA (
        'Microsoft\Windows\Start Menu\Programs\Goden')
    if (Test-Path -LiteralPath $godenShortcutGroup) {
        foreach ($link in Get-ChildItem -LiteralPath $godenShortcutGroup `
                -Filter '*.lnk' -File) {
            Remove-Item -LiteralPath $link.FullName -Force
        }
    }
    $shortcutManifest = Join-Path $registrationBackup 'shortcuts.json'
    if (Test-Path -LiteralPath $shortcutManifest) {
        foreach ($shortcut in @(Get-Content -LiteralPath $shortcutManifest `
                -Raw | ConvertFrom-Json)) {
            New-Item -ItemType Directory -Force `
                -Path (Split-Path -Parent $shortcut.original) | Out-Null
            Copy-Item -LiteralPath $shortcut.backup `
                -Destination $shortcut.original -Force
        }
    }
    Write-Host "Restored the compatible NekoRoX installation: $rollbackExe"
} else {
    throw 'Neither app.previous nor the compatible NekoRoX installation exists.'
}

if ($Launch) {
    Start-Process -FilePath $rollbackExe
}
