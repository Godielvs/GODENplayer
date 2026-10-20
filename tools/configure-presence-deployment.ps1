param([Parameter(Mandatory)][ValidatePattern('^[0-9]{16,24}$')][string]$ApplicationId)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
# Private endpoints and credentials live only in local Options, never in source.
$header=Join-Path $PSScriptRoot '../src/component/presence_deployment.h'
$content=Get-Content $header -Raw
$content=[regex]::Replace($content,'inline constexpr std::string_view applicationId\{[^}]*\};',
  ('inline constexpr std::string_view applicationId{{"{0}"}};' -f $ApplicationId))
[IO.File]::WriteAllText($header,$content,[Text.UTF8Encoding]::new($false))
