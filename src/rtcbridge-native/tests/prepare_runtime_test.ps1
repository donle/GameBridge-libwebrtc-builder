param([Parameter(Mandatory)][string]$SourceRoot,[Parameter(Mandatory)][string]$Output)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$pins=Get-Content -Raw -LiteralPath "$PSScriptRoot/../pins.json" | ConvertFrom-Json
if((git -C $SourceRoot rev-parse HEAD).Trim() -cne $pins.webrtc_revision){throw 'Runtime test requires the exact pinned upstream revision'}
$bridge=Get-Content -Raw -LiteralPath "$PSScriptRoot/../src/bridge.cpp"
# Test the real member initialization order before the network socket server.
# No replacement network stack or alternate initializer is used by the test.
$match=[regex]::Match($bridge,'(?s)struct Runtime \{(?<prefix>.*?)std::unique_ptr<w::Thread> network')
if(!$match.Success){throw 'Runtime network initialization boundary not found'}
[IO.File]::WriteAllText([IO.Path]::GetFullPath($Output),$match.Groups['prefix'].Value,[Text.UTF8Encoding]::new($false))
