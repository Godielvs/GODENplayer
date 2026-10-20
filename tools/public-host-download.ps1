# Build-only acquisition helpers. No command runs when this file is dot-sourced.
function Receive-PublicHost([Uri]$Uri, [string]$Temporary, [long]$MaximumBytes) {
    Add-Type -AssemblyName System.Net.Http
    $handler=[Net.Http.HttpClientHandler]::new()
    $handler.AllowAutoRedirect=$false
    $client=[Net.Http.HttpClient]::new($handler)
    $cancel=[Threading.CancellationTokenSource]::new([TimeSpan]::FromSeconds(120))
    $response=$null; $inputStream=$null; $outputStream=$null
    try {
        for ($redirect=0; ; ++$redirect) {
            if ($Uri.Scheme -ne 'https' -or $Uri.Host -notin @('www.foobar2000.org','foobar2000.org') -or
                    $Uri.UserInfo -or $Uri.Port -ne 443) {throw 'Official host redirect origin refused.'}
            $response=$client.GetAsync($Uri,[Net.Http.HttpCompletionOption]::ResponseHeadersRead,$cancel.Token).GetAwaiter().GetResult()
            $status=[int]$response.StatusCode
            if ($status -notin @(301,302,303,307,308)) {break}
            if ($redirect -ge 3 -or $null -eq $response.Headers.Location) {throw 'Official host redirect limit exceeded.'}
            $next=[Uri]::new($Uri,$response.Headers.Location)
            $response.Dispose();$response=$null;$Uri=$next
        }
        [void]$response.EnsureSuccessStatusCode()
        if ($response.Content.Headers.ContentLength -gt $MaximumBytes) {throw 'Official host response exceeds size limit.'}
        $inputStream=$response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
        $outputStream=[IO.File]::Open($Temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
        $buffer=[byte[]]::new(65536);[long]$total=0
        for (;;) {
            $read=$inputStream.ReadAsync($buffer,0,$buffer.Length,$cancel.Token).GetAwaiter().GetResult()
            if ($read -eq 0) {break}
            $total+=$read
            if ($total -gt $MaximumBytes) {throw 'Official host response exceeds size limit.'}
            $outputStream.Write($buffer,0,$read)
        }
    } finally {
        if ($null -ne $outputStream) {$outputStream.Dispose()}
        if ($null -ne $inputStream) {$inputStream.Dispose()}
        if ($null -ne $response) {$response.Dispose()}
        $cancel.Dispose();$client.Dispose();$handler.Dispose()
    }
}

function Assert-PublicHostIdentity([string]$Path) {
    $signature=Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne 'Valid' -or $null -eq $signature.SignerCertificate -or
            $signature.SignerCertificate.Subject -notmatch 'Illustrate Ltd') {throw 'Official host signature or publisher mismatch.'}
    if ((Get-Item -LiteralPath $Path).VersionInfo.ProductVersion -notmatch '^2\.25\.10(?:\D|$)') {throw 'Official host version mismatch.'}
}

function Get-VerifiedPublicHost([string]$Destination, [Uri]$Uri, [string]$ExpectedSha256,
        [long]$MaximumBytes=16777216) {
    $Destination=[IO.Path]::GetFullPath($Destination)
    $parent=Split-Path -Parent $Destination
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $validate={param([string]$Path)
        $item=Get-Item -LiteralPath $Path
        if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
                $item.Length -gt $MaximumBytes -or
                (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $ExpectedSha256) {
            throw 'Official host installer checksum/size mismatch. An existing cache is preserved; choose a fresh destination or remove the invalid cache explicitly.'
        }
        Assert-PublicHostIdentity $Path
    }
    if (Test-Path -LiteralPath $Destination) {& $validate $Destination;return}
    $temporary=Join-Path $parent ((Split-Path -Leaf $Destination)+'.download-'+[Guid]::NewGuid().ToString('N'))
    try {
        Receive-PublicHost $Uri $temporary $MaximumBytes
        & $validate $temporary
        # File.Move refuses an existing destination: concurrent acquisition
        # cannot replace a previously published, verified cache entry.
        [IO.File]::Move($temporary,$Destination)
    } finally {
        # One exact unique temporary file, never a recursive caller path.
        if (Test-Path -LiteralPath $temporary -PathType Leaf) {Remove-Item -LiteralPath $temporary -Force}
    }
}
