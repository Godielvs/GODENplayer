param([string]$RendererSource='', [string]$OutputRoot='', [string]$AsciiSource='')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
if(!$RendererSource){$RendererSource=Join-Path $repo 'src/component/artwork_transition_renderer.cpp'}
if(!$AsciiSource){$AsciiSource=Join-Path $repo 'src/component/ascii_artwork_transition_renderer.cpp'}
if(!$OutputRoot){$OutputRoot=Join-Path $repo 'artifacts/artwork-raster-transition'}
$full=[IO.Path]::GetFullPath($OutputRoot)
$allowed=[IO.Path]::GetFullPath((Join-Path $repo 'artifacts')).TrimEnd('\')+'\'
if(!($full+'\').StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase)){throw 'QA outputs must remain in artifacts'}
New-Item -ItemType Directory -Force -Path $full | Out-Null
$vsdev='C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\Tools\VsDevCmd.bat'
$lines=& cmd.exe /c "`"$vsdev`" -arch=x64 -host_arch=x64 >nul 2>&1 && set"
foreach($line in $lines){$split=$line.IndexOf('=');if($split -gt 0){Set-Item -LiteralPath ('Env:'+$line.Substring(0,$split)) -Value $line.Substring($split+1)}}
$sources=@("$repo/tests/artwork_raster_transition_probe.cpp",$RendererSource,
    $AsciiSource,"$repo/src/component/glyph_flow_service.cpp",
    "$repo/src/component/ui_design_system.cpp","$repo/src/component/interaction_latency_guard.cpp",
    "$repo/src/core/src/glyph_flow.cpp")
Push-Location $full
try {
    & cl.exe /nologo /std:c++20 /O2 /EHsc /utf-8 /DNDEBUG /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I "$repo/src/component" /I "$repo/src/core/include" /I "$repo/vendor" @sources /Fe:artwork-raster-probe.exe /link d2d1.lib dwrite.lib windowscodecs.lib ole32.lib user32.lib
    if($LASTEXITCODE){throw 'Artwork renderer probe compile failed'}
    & "$full/artwork-raster-probe.exe"
    if($LASTEXITCODE){throw 'Artwork raster transition regression'}
}finally{Pop-Location}
