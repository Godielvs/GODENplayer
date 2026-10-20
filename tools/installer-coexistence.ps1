# Read-only snapshots. Raw registry values and shortcut targets never leave
# this process: reports contain hashes, not usernames, paths or associations.
function Get-FoobarCoexistenceState {
    $rows = [Collections.Generic.List[string]]::new()
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $hashText = { param([string]$Text)
            ([BitConverter]::ToString($sha.ComputeHash(
                [Text.Encoding]::UTF8.GetBytes($Text)))).Replace('-', '').ToLowerInvariant()
        }
        foreach ($hive in @([Microsoft.Win32.RegistryHive]::CurrentUser,
                [Microsoft.Win32.RegistryHive]::LocalMachine)) {
            foreach ($view in @([Microsoft.Win32.RegistryView]::Registry32,
                    [Microsoft.Win32.RegistryView]::Registry64)) {
                $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive, $view)
                try {
                    $roots = [Collections.Generic.List[string]]::new()
                    $roots.Add('Software\foobar2000')
                    $roots.Add('Software\Microsoft\Windows\CurrentVersion\App Paths\foobar2000.exe')
                    foreach ($container in @('Software\Classes',
                            'Software\Microsoft\Windows\CurrentVersion\Uninstall',
                            'Software\RegisteredApplications')) {
                        $parent = $base.OpenSubKey($container)
                        if ($null -eq $parent) { continue }
                        try {
                            foreach ($name in $parent.GetSubKeyNames()) {
                                if ($name -like 'foobar2000*') { $roots.Add("$container\$name") }
                            }
                            foreach ($name in $parent.GetValueNames()) {
                                if ($name -like '*foobar2000*') {
                                    $value = ConvertTo-Json -InputObject $parent.GetValue($name,
                                        $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) -Compress
                                    $rows.Add("$hive|$view|$container|$name|$(& $hashText $value)")
                                }
                            }
                        } finally { $parent.Dispose() }
                    }
                    $roots.Add('Software\Classes\Applications\foobar2000.exe')
                    foreach ($extension in @('aac','aif','aiff','alac','ape','dff','dsf',
                            'flac','it','m4a','mka','mod','mp1','mp2','mp3','mp4',
                            'mpc','ogg','opus','s3m','spc','tak','tta','wav','wma','wv','xm')) {
                        $roots.Add("Software\Classes\.$extension")
                        $roots.Add("Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.$extension\UserChoice")
                        $roots.Add("Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.$extension\OpenWithProgids")
                    }
                    foreach ($root in ($roots | Sort-Object -Unique)) {
                        $pending = [Collections.Generic.Stack[string]]::new()
                        $pending.Push($root)
                        while ($pending.Count) {
                            $path = $pending.Pop(); $key = $base.OpenSubKey($path)
                            if ($null -eq $key) { continue }
                            try {
                                $rows.Add("$hive|$view|$path|<key>")
                                foreach ($name in ($key.GetValueNames() | Sort-Object)) {
                                    $value = ConvertTo-Json -InputObject $key.GetValue($name,
                                        $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) -Compress
                                    $rows.Add("$hive|$view|$path|$name|$($key.GetValueKind($name))|$(& $hashText $value)")
                                }
                                foreach ($name in $key.GetSubKeyNames()) { $pending.Push("$path\$name") }
                            } finally { $key.Dispose() }
                        }
                    }
                } finally { $base.Dispose() }
            }
        }
        foreach ($folder in @('DesktopDirectory','CommonDesktopDirectory','Programs','CommonPrograms')) {
            $directory = [Environment]::GetFolderPath([Environment+SpecialFolder]::$folder)
            if (!$directory -or !(Test-Path -LiteralPath $directory)) { continue }
            foreach ($file in (Get-ChildItem -LiteralPath $directory -Recurse -File -Filter '*.lnk' -ErrorAction Stop)) {
                if ($file.FullName -notmatch '(?i)foobar2000') { continue }
                $relative = $file.FullName.Substring($directory.Length).TrimStart('\')
                $rows.Add("shortcut|$folder|$relative|$((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash)")
            }
        }
        $normalized = ($rows | Sort-Object -Unique) -join "`n"
        [ordered]@{ sha256 = & $hashText $normalized; observedEntries = $rows.Count }
    } finally { $sha.Dispose() }
}
