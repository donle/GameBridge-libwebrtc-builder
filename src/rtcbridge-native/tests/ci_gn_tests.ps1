param(
    [Parameter(Mandatory=$true)][string]$GnExecutable,
    [string]$BuildScript="$PSScriptRoot/../../../scripts/build-libwebrtc-ci.ps1"
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$repository=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$pins=Get-Content -Raw -LiteralPath (Join-Path $repository 'src/rtcbridge-native/pins.json') | ConvertFrom-Json
$source=Get-Content -Raw -LiteralPath $BuildScript
# Execute the production GN preparation/invocation verbatim against the real
# pinned GN parser. Only the source graph is a tiny SDK-free fixture.
$block=[regex]::Match($source,"(?s)Push-Location src\s+try \{\s*(?<body>.*?)\s*Check-Exit 'GN generation'")
if(!$block.Success){throw 'Actual GN generation block not found'}
$gnBinary=(Resolve-Path -LiteralPath $GnExecutable).Path
function gn { & $gnBinary @args }
function Write-Fixture([string]$Relative,[string]$Content) {
    $path=Join-Path $scratch $Relative
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($path)) -Force | Out-Null
    [IO.File]::WriteAllText($path,$Content,[Text.UTF8Encoding]::new($false))
}
$scratch=Join-Path $repository ('out/gn-command-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
try {
    Write-Fixture '.gn' 'buildconfig = "//BUILDCONFIG.gn"'
    Write-Fixture 'BUILDCONFIG.gn' @'
declare_args() {
  is_debug = true
  is_component_build = true
  rtc_include_tests = true
  rtc_build_examples = true
  rtc_use_h264 = false
  rtc_use_h265 = true
  proprietary_codecs = false
  ffmpeg_branding = "NOT_OVERRIDDEN"
  rtc_enable_symbol_export = true
  is_clang = false
  use_lld = false
  use_custom_libcxx = true
  symbol_level = 2
  treat_warnings_as_errors = true
  use_remoteexec = true
}
assert(!is_debug && !is_component_build && target_cpu == "x64")
assert(!rtc_include_tests && !rtc_build_examples && rtc_use_h264 && !rtc_use_h265)
assert(proprietary_codecs && ffmpeg_branding == "Chrome" && !rtc_enable_symbol_export)
assert(is_clang && use_lld && !use_custom_libcxx && symbol_level == 0)
assert(!treat_warnings_as_errors && !use_remoteexec)
set_default_toolchain("//:fixture")
'@
    Write-Fixture 'BUILD.gn' @'
toolchain("fixture") {
  tool("stamp") {
    command = "cmd /c exit 0"
    description = "STAMP {{output}}"
  }
}
'@
    Write-Fixture 'gamebridge/rtcbridge-native/BUILD.gn' 'group("all") {}'
    Push-Location $scratch
    try {
        & ([scriptblock]::Create($block.Groups['body'].Value))
        if($LASTEXITCODE -ne 0){throw 'Actual production GN invocation failed to parse the pinned arguments'}
        $written=Get-Content -Raw -LiteralPath 'out/Release/args.gn'
        if($written.Trim() -cne $pins.gn_args){throw 'Pinned GN arguments changed during command construction'}
        if(!(Test-Path -LiteralPath 'out/Release/build.ninja')){throw 'Real GN did not generate its fixture graph'}
    } finally {Pop-Location}
} finally {
    $parent=[IO.Path]::GetFullPath((Join-Path $repository 'out'))+'\'
    $resolved=[IO.Path]::GetFullPath($scratch)
    if(!$resolved.StartsWith($parent,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolved) -notmatch '^gn-command-test-[0-9a-f]{32}$'){
        throw 'Unsafe GN test cleanup target'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
Write-Host 'CI GN command: actual production construction and real GN parsing of all pinned arguments PASS'
