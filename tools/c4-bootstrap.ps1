param([string]$Destination=(Join-Path $PSScriptRoot '..\out\c4-tools'), [ValidateRange(1,1)][int]$Workers=1)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
$Destination=[IO.Path]::GetFullPath($Destination)
function Run([string]$Program,[string[]]$Arguments){& $Program @Arguments;if($LASTEXITCODE -ne 0){throw "$Program failed ($LASTEXITCODE)"}}
if(-not(Get-Command git -ErrorAction SilentlyContinue)){throw 'Git is required.'}
& "$PSScriptRoot/import-v142-environment.ps1" | Out-Null
$buildTools=& "$PSScriptRoot/resolve-native-build-tools.ps1"
$cmake=$buildTools.CMake
$ninja=$buildTools.Ninja
New-Item -ItemType Directory -Force $Destination,(Join-Path $Destination 'bin')|Out-Null
$tools=@(
 @{Name='glslang';URL='https://github.com/KhronosGroup/glslang.git';Commit='1062752a891c95b2bfeed9e356562d88f9df84ac';Flags=@('-DENABLE_OPT=OFF','-DENABLE_HLSL=OFF','-DENABLE_GLSLANG_BINARIES=ON','-DBUILD_TESTING=OFF');Exe='glslangValidator.exe'},
 @{Name='SPIRV-Cross';URL='https://github.com/KhronosGroup/SPIRV-Cross.git';Commit='ebe2aa0cd80f5eb5cd8a605da604cacf72205f3b';Flags=@('-DSPIRV_CROSS_CLI=ON','-DSPIRV_CROSS_ENABLE_TESTS=OFF');Exe='spirv-cross.exe'}
)
$receipt=@()
foreach($t in $tools){
 $source=Join-Path $Destination $t.Name;$build=Join-Path $Destination ($t.Name+'-build')
 if(-not(Test-Path $source)){Run 'git' @('init',$source);Run 'git' @('-C',$source,'remote','add','origin',$t.URL);Run 'git' @('-C',$source,'fetch','--depth','1','origin',$t.Commit);Run 'git' @('-C',$source,'checkout','--detach','FETCH_HEAD')}
 $head=(& git -C $source rev-parse HEAD).Trim();if($head -ne $t.Commit){throw "Compiler source is not the pinned commit: $source"}
 $configure=@('-S',$source,'-B',$build,'-DCMAKE_BUILD_TYPE=Release')
 $generator=$env:CMAKE_GENERATOR
 if([string]::IsNullOrWhiteSpace($generator)){$generator='Ninja'}
 $configure+=@('-G',$generator)
 if($generator -like 'Visual Studio*'){
  $configure+=@('-A','x64')
 }elseif($generator -eq 'Ninja'){
  $configure+="-DCMAKE_MAKE_PROGRAM:FILEPATH=$ninja"
 }
 Run $cmake ($configure+$t.Flags)
 Run $cmake @('--build',$build,'--config','Release','--parallel',([string]$Workers))
 $candidates=@(Get-ChildItem $build -Recurse -File -Filter $t.Exe)
 $found=@($candidates|Where-Object {$_.FullName -match '[\\/]Release[\\/]?'})
 if($found.Count -eq 0){$found=$candidates}
 if($found.Count -eq 0 -and $t.Name -eq 'glslang'){
  $candidates=@(Get-ChildItem $build -Recurse -File -Filter 'glslang.exe')
  $found=@($candidates|Where-Object {$_.FullName -match '[\\/]Release[\\/]?'})
  if($found.Count -eq 0){$found=$candidates}
 }
 if($found.Count -eq 0){throw "Compiler binary missing: $($t.Exe)"}
 $target=Join-Path (Join-Path $Destination 'bin') $t.Exe;Copy-Item $found[0].FullName $target -Force
 $receipt+=@{name=$t.Name;commit=$head;binary=$target;sha256=(Get-FileHash $target -Algorithm SHA256).Hash}
}
$receipt|ConvertTo-Json -Depth 5|Set-Content (Join-Path $Destination 'TOOLCHAIN_RECEIPT.json') -Encoding UTF8
Write-Host 'C4 build-only compiler tools ready. No player/profile settings changed.'
