param()
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted' -or $env:RUNNER_OS -ne 'Windows') { throw 'This installer/build script is restricted to ephemeral GitHub-hosted Windows; never run locally' }
$repository=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$pins=Get-Content -Raw -LiteralPath (Join-Path $repository 'src/rtcbridge-native/pins.json') | ConvertFrom-Json
if (Test-Path -LiteralPath (Join-Path $repository 'export-manifest.json')) {
    $export=Get-Content -Raw -LiteralPath (Join-Path $repository 'export-manifest.json') | ConvertFrom-Json
    foreach($entry in $export.files) {
        $path=[IO.Path]::GetFullPath((Join-Path $repository $entry.path))
        if (!$path.StartsWith($repository+'\',[StringComparison]::OrdinalIgnoreCase) -or (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash -ine $entry.sha256) {throw 'Builder export manifest differs from reviewed source'}
    }
}
function Check-Exit([string]$Stage) { if ($LASTEXITCODE -ne 0) { throw "$Stage failed with exit $LASTEXITCODE" } }
function Download-Pinned([string]$Uri,[string]$Path,[string]$Sha) {
    Invoke-WebRequest -Uri $Uri -OutFile $Path
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash -ine $Sha) { throw 'Pinned build dependency SHA-256 mismatch' }
}
function Disk-Snapshot { Get-PSDrive -PSProvider FileSystem | Select-Object Name,Free,Used }
$before=@(Disk-Snapshot)
$before | Format-Table | Out-Host
# Fixed allowlist: these are unrelated preinstalled toolchains/caches on this
# disposable VM. Preserve Visual Studio, Windows SDKs, Git, Node and gh.
$reclaimed=@()
foreach($path in @('C:\hostedtoolcache','C:\Android','C:\Program Files\Android','C:\Program Files\Java','C:\Program Files\Unity','C:\Program Files\Unity Hub','C:\SeleniumWebDrivers','C:\vcpkg')) {
    if (!(Test-Path -LiteralPath $path)) { continue }
    $resolved=(Get-Item -LiteralPath $path -Force).FullName
    if ($resolved -ine $path -or ($resolved -notlike 'C:\*') -or $resolved.Length -lt 10 -or $resolved -like '*Visual Studio*' -or $resolved -like '*Windows Kits*') { throw 'Unsafe ephemeral cleanup target' }
    if (((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Refusing ephemeral cleanup through a reparse point' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
    $reclaimed += $resolved
}
$after=@(Disk-Snapshot)
$after | Format-Table | Out-Host
$volume=Get-PSDrive -PSProvider FileSystem | Where-Object {$_.Name -in @('C','D')} | Sort-Object Free -Descending | Select-Object -First 1
if ($volume.Free -lt ($pins.minimum_free_gib * 1GB)) { throw "Insufficient hosted runner disk after bounded cache reclamation: need $($pins.minimum_free_gib) GiB; a larger ephemeral hosted runner is required" }
$buildRoot="$($volume.Name):\gb-rtc-ci"
if (Test-Path -LiteralPath $buildRoot) { throw 'CI source directory unexpectedly exists' }
New-Item -ItemType Directory -Path $buildRoot | Out-Null
$env:TEMP=Join-Path $buildRoot 'temp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Path $env:TEMP | Out-Null
$vswhere='C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vs=(& $vswhere -latest -products '*' -version '[18.0,19.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.ATLMFC -property installationPath).Trim()
if (!$vs) { throw 'The VS2026 hosted image must provide C++ and ATL/MFC components' }
$env:vs2026_install=$vs
$sdkInstaller=Join-Path $buildRoot 'winsdksetup.exe'
Download-Pinned $pins.windows_sdk_installer_url $sdkInstaller $pins.windows_sdk_installer_sha256
$signature=Get-AuthenticodeSignature -LiteralPath $sdkInstaller
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') { throw 'Windows SDK installer signature rejected' }
$installer=Start-Process -FilePath $sdkInstaller -ArgumentList @('/quiet','/norestart','/features','+') -WindowStyle Hidden -Wait -PassThru
if ($installer.ExitCode -notin @(0,3010)) { throw "Windows SDK setup failed: $($installer.ExitCode)" }
$sdkRoot='C:\Program Files (x86)\Windows Kits\10'
foreach($file in @("Include\$($pins.windows_sdk_directory_version)\um\Windows.h", "Lib\$($pins.windows_sdk_directory_version)\um\x64\kernel32.lib",'Debuggers\x64\dbghelp.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $sdkRoot $file))) { throw 'Pinned Windows SDK/debugging tools were not installed' }
}
$depot=Join-Path $buildRoot 'depot_tools'
git -c core.longpaths=true clone --filter=blob:none --no-checkout https://chromium.googlesource.com/chromium/tools/depot_tools.git $depot
Check-Exit 'depot_tools clone'
git -C $depot checkout --detach $pins.depot_tools_revision
Check-Exit 'depot_tools pinned checkout'
$env:DEPOT_TOOLS_UPDATE='0';$env:DEPOT_TOOLS_WIN_TOOLCHAIN='0'
$env:VPYTHON_VIRTUALENV_ROOT=Join-Path $buildRoot 'vpython';$env:CIPD_CACHE_DIR=Join-Path $buildRoot 'cipd-cache'
$env:Path="$depot;$env:Path"
if ((Get-PSDrive -Name $volume.Name).Free -lt ($pins.minimum_free_gib * 1GB)) { throw 'SDK provisioning left insufficient disk for pinned libwebrtc sync/build' }
$sourceRoot=Join-Path $buildRoot 'webrtc'
New-Item -ItemType Directory -Path $sourceRoot | Out-Null
Push-Location $sourceRoot
try {
    # Bootstrap Windows depot_tools through cmd, per upstream instructions.
    cmd /c gclient --version
    Check-Exit 'depot_tools bootstrap'
    $spec="solutions = [{'name':'src','url':'https://webrtc.googlesource.com/src.git','managed':False,'custom_deps':{},'custom_vars':{'checkout_android':False,'checkout_ios':False}}]"
    [IO.File]::WriteAllText((Join-Path $sourceRoot '.gclient'),$spec,[Text.UTF8Encoding]::new($false))
    git -c core.longpaths=true clone --filter=blob:none --no-checkout --depth 1 https://webrtc.googlesource.com/src.git src
    Check-Exit 'libwebrtc clone'
    git -C src fetch --depth 1 origin $pins.webrtc_revision
    Check-Exit 'libwebrtc pinned fetch'
    git -C src checkout --detach $pins.webrtc_revision
    Check-Exit 'libwebrtc pinned checkout'
    gclient sync --no-history --shallow --revision "src@$($pins.webrtc_revision)" --jobs 4
    Check-Exit 'libwebrtc dependency sync'
    $actual=(git -C src rev-parse HEAD).Trim()
    if ($actual -cne $pins.webrtc_revision -or (git -C src/build rev-parse HEAD).Trim() -cne $pins.chromium_build_revision) { throw 'Upstream source/build revision mismatch' }
    # The pinned upstream only drains startup frames on a later InjectFrame.
    # Bind our narrowly scoped encoder-ready fix to the attested source build.
    $startupPatch=Join-Path $repository 'src/rtcbridge-native/patches/encoder-ready-drain.patch'
    if ((Get-FileHash -LiteralPath $startupPatch -Algorithm SHA256).Hash -ine $pins.encoder_ready_patch_sha256) { throw 'Encoder-ready patch hash mismatch' }
    git -C src apply --check $startupPatch
    Check-Exit 'Encoder-ready patch applicability'
    git -C src apply $startupPatch
    Check-Exit 'Encoder-ready patch application'
    $bridgeRoot=Join-Path $sourceRoot 'src/gamebridge'
    New-Item -ItemType Directory -Path $bridgeRoot | Out-Null
    foreach($component in @('rtcbridge-native','rtcbridge','native')) {
        # Export manifest forbids desktop, signaling and unrelated source.
        if($component -eq 'rtcbridge-native'){Copy-Item -LiteralPath (Join-Path $repository 'src/rtcbridge-native') -Destination $bridgeRoot -Recurse}
        elseif($component -eq 'rtcbridge'){
            New-Item -ItemType Directory -Path (Join-Path $bridgeRoot 'rtcbridge/include') -Force | Out-Null
            Copy-Item -LiteralPath (Join-Path $repository 'src/rtcbridge/include/gamebridge_rtc.h') -Destination (Join-Path $bridgeRoot 'rtcbridge/include')
        }else{
            foreach($folder in @('tests','bench')){New-Item -ItemType Directory -Path (Join-Path $bridgeRoot "native/$folder") -Force | Out-Null}
            foreach($file in @('tests/rtc_abi_tests.cpp','tests/rtc_abi_c_layout.c','tests/rtc_media_fixture.h','bench/rtc_bridge_bench.cpp','bench/pacing.h')){Copy-Item -LiteralPath (Join-Path $repository "src/native/$file") -Destination (Join-Path $bridgeRoot "native/$file")}
        }
    }
    & "$bridgeRoot/rtcbridge-native/tests/prepare_injector_test.ps1" -SourceRoot (Join-Path $sourceRoot 'src') -Output (Join-Path $bridgeRoot 'rtcbridge-native/tests/pinned_injector_methods.h')
    Push-Location src
    try {
        gn gen out/Release --root-target=//gamebridge/rtcbridge-native:all --args=$pins.gn_args
        Check-Exit 'GN generation'
        autoninja -C out/Release gamebridge/rtcbridge-native:all -j 4
        Check-Exit 'Native bridge build'
        $binary=Join-Path $sourceRoot 'src/out/Release'
        & "$binary/rtc_native_core_tests.exe";Check-Exit 'Native core tests'
        & "$binary/rtc_injector_cold_start_tests.exe";Check-Exit 'Pinned injector single-frame cold start'
        & "$binary/rtc_abi_tests.exe" "$binary/gamebridge_rtc.dll" production;Check-Exit 'Production RTC ABI'
        & "$binary/rtc_abi_tests.exe" "$binary/gamebridge_rtc_probe.dll" probe;Check-Exit 'Probe RTC ABI'
    } finally {Pop-Location}
} finally {Pop-Location}
$ffmpegArchive=Join-Path $buildRoot 'ffmpeg.zip'
Download-Pinned $pins.ffmpeg_url $ffmpegArchive $pins.ffmpeg_sha256
Expand-Archive -LiteralPath $ffmpegArchive -DestinationPath (Join-Path $buildRoot 'ffmpeg')
$ffmpeg=Join-Path $buildRoot 'ffmpeg/ffmpeg-8.1.1-essentials_build/bin/ffmpeg.exe'
& "$PSScriptRoot/test-rtc-native-gate.ps1" -DurationSeconds 5 -ArtifactDirectory $binary -Output 'out/rtc-short-gate.json' -FFmpeg $ffmpeg
$package=Join-Path $repository 'out/rtc-package'
$payload=Join-Path $package 'payload'
New-Item -ItemType Directory -Path $payload -Force | Out-Null
Push-Location (Join-Path $sourceRoot 'src')
try {
    # Upstream's scanner follows the actual GN dependency graph. The pinned
    # upstream table lacks its optional H.264 dependencies; name their licenses
    # explicitly and retain fail-closed behavior for any other unknown library.
    vpython3 -c "import sys; sys.path.insert(0, 'tools_webrtc/libs'); import generate_licenses as g; licenses=dict(g.LIB_TO_LICENSES_DICT); licenses['openh264']=['third_party/openh264/src/LICENSE']; licenses['ffmpeg']=['third_party/ffmpeg/COPYING.LGPLv2.1']; g.LicenseBuilder(['out/Release'], ['//gamebridge/rtcbridge-native:gamebridge_rtc'], licenses).generate_license_text(sys.argv[1])" $payload
    Check-Exit 'Third-party license collection'
    Copy-Item -LiteralPath 'PATENTS' -Destination (Join-Path $payload 'PATENTS.txt')
} finally {Pop-Location}
foreach($name in @('gamebridge_rtc.dll','gamebridge_rtc_probe.dll','gamebridge_rtc_bench.dll','rtc_abi_tests.exe','rtc_bridge_bench.exe','rtc_native_core_tests.exe')) {Copy-Item -LiteralPath (Join-Path $binary $name) -Destination $payload}
Copy-Item -LiteralPath (Join-Path $repository 'out/rtc-short-gate.json') -Destination $payload
$metadata=@{
    run_id=$env:GITHUB_RUN_ID;run_attempt=$env:GITHUB_RUN_ATTEMPT
    source_revision=$env:GITHUB_SHA;webrtc_revision=$pins.webrtc_revision;depot_tools_revision=$pins.depot_tools_revision
    chromium_build_revision=$pins.chromium_build_revision;gn_args=$pins.gn_args
    encoder_ready_patch_sha256=$pins.encoder_ready_patch_sha256
    runner_image=$env:ImageOS;runner_image_version=$env:ImageVersion
    visual_studio_path=$vs;sdk_installer_sha256=$pins.windows_sdk_installer_sha256
    disk_before=$before;disk_after=$after;reclaimed_paths=$reclaimed
    core_tests='passed';injector_cold_start='passed';abi_production='passed';abi_probe='passed';short_gate='passed'
}
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $payload 'native-build.json') -Encoding UTF8
$archive=Join-Path $package 'gamebridge-rtc-windows-x64.zip'
Compress-Archive -Path "$payload/*" -DestinationPath $archive
$hash=(Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $package 'SHA256SUMS'),"$hash  gamebridge-rtc-windows-x64.zip`n",[Text.UTF8Encoding]::new($false))
Write-Host "Native build, ABI, and five-second gate passed; archive SHA-256 $hash"
