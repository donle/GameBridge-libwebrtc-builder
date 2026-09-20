param([string]$ExportDirectory=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..')))
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$root=[IO.Path]::GetFullPath($ExportDirectory)
$manifest=Get-Content -Raw -LiteralPath (Join-Path $root 'export-manifest.json') | ConvertFrom-Json
$required=@(
    'src/native/include/gamebridge/audio/audio_clock.h',
    'src/native/include/gamebridge/audio/opus_codec.h',
    'src/native/src/audio/audio_clock.cpp',
    'src/native/src/audio/opus_codec.cpp'
)
$nativeAllowed=$required+@('src/native/tests/rtc_abi_tests.cpp','src/native/tests/rtc_abi_c_layout.c',
    'src/native/tests/rtc_media_fixture.h','src/native/tests/rtc_connection_diagnostics.h',
    'src/native/bench/rtc_bridge_bench.cpp','src/native/bench/pacing.h')
$otherAllowed=@('.gitattributes','.github/workflows/build-libwebrtc.yml',
    'scripts/build-libwebrtc-ci.ps1','scripts/rtc-ci-storage.ps1','scripts/rtc-ci-git.ps1',
    'scripts/test-rtc-native-gate.ps1','scripts/validate-rtc-native-gate.ps1',
    'scripts/rtc-artifact-security.ps1','src/rtcbridge/include/gamebridge_rtc.h')
$paths=@($manifest.files | ForEach-Object { [string]$_.path })
foreach($requiredPath in $required){if($requiredPath -cnotin $paths){throw "Required real Opus ABI-test dependency omitted: $requiredPath"}}
if(@($paths | Sort-Object -Unique).Count -ne $paths.Count){throw 'Duplicate builder manifest path'}
foreach($entry in $manifest.files){
    $relative=[string]$entry.path
    if($relative -cnotin $nativeAllowed -and $relative -cnotin $otherAllowed -and $relative -cnotmatch '^src/rtcbridge-native/[A-Za-z0-9_./-]+\.(h|cpp|gn|json|md|txt|ps1|patch|py)$'){throw 'Builder publication path is outside the reviewed source boundary'}
    $path=[IO.Path]::GetFullPath((Join-Path $root $relative))
    if(!$path.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase) -or $relative.Split('/') -contains '..'){throw 'Builder publication path escaped its root'}
    if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $entry.sha256 -or (Get-Item -LiteralPath $path).Length -ne $entry.bytes){throw 'Builder publication bytes differ from the reviewed manifest'}
}
$actual=@(Get-ChildItem -LiteralPath $root -Force | Where-Object Name -NE '.git' | ForEach-Object {
    if($_.PSIsContainer){Get-ChildItem -LiteralPath $_.FullName -Recurse -File -Force}else{$_}
} | ForEach-Object { $_.FullName.Substring($root.Length+1).Replace('\','/') } | Where-Object { $_ -cne 'export-manifest.json' })
if(Compare-Object $paths $actual -CaseSensitive){throw 'Builder checkout has unmanifested or missing source files'}
Write-Host "Builder publication manifest: exact allowlist, complete Opus test dependencies, $($paths.Count) byte hashes PASS"
