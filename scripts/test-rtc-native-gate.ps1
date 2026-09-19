param(
    [ValidateRange(5,7200)][int]$DurationSeconds=1800,
    [Parameter(Mandatory)][string]$ArtifactDirectory,
    [string]$Output='artifacts/rtc-native-gate.json',
    [string]$FFmpeg='ffmpeg'
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. "$PSScriptRoot/validate-rtc-native-gate.ps1"
$repository=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$artifact=[IO.Path]::GetFullPath($ArtifactDirectory)
$resultPath=[IO.Path]::GetFullPath((Join-Path $repository $Output))
if ($env:GITHUB_ACTIONS -ne 'true' -and ([IO.Path]::GetPathRoot($repository) -ine 'D:\' -or [IO.Path]::GetPathRoot($artifact) -ine 'D:\' -or [IO.Path]::GetPathRoot($resultPath) -ine 'D:\')) { throw 'Local native RTC artifacts and output must remain on D:' }
New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($resultPath)) | Out-Null
$report=[pscustomobject]@{passed=$false}
$stage='preparation'
$failed=$false
# Invalidate stale success before any external preparation. Finally replaces
# this with the complete allowlisted report even when the consumer exits nonzero.
$initial=ConvertTo-NativeRtcDiagnostic $report $DurationSeconds $stage
Write-NativeRtcDiagnostic $initial $resultPath -Quiet
# Keep raw consumer output separate: even cancellation before finally must
# never let the workflow upload an unsanitized or stale report.
$rawResultPath=$resultPath+'.consumer.json'
$initial | ConvertTo-Json | Set-Content -LiteralPath $rawResultPath -Encoding UTF8
try {
$fixture=Join-Path $repository 'out/rtc-native-fixture/h264-1080p60-25mbps.annexb'
New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($fixture)) | Out-Null
$stage='fixture_generation'
& $FFmpeg -hide_banner -loglevel error -y -f lavfi -i 'testsrc2=size=1920x1080:rate=60,noise=alls=20:allf=t+u:all_seed=7301' -frames:v 120 -an -c:v libx264 -preset ultrafast -tune zerolatency -profile:v baseline -level:v 4.2 -threads 1 -b:v 25000000 -minrate 25000000 -maxrate 25000000 -bufsize 2500000 -x264-params 'nal-hrd=cbr:force-cfr=1:aud=1:repeat-headers=1:keyint=120:min-keyint=120:scenecut=0' -f h264 $fixture
if ($LASTEXITCODE -ne 0) { throw 'Synthetic fixture generation failed' }
$stage='fixture_decode'
& $FFmpeg -hide_banner -loglevel error -xerror -i $fixture -f null -
if ($LASTEXITCODE -ne 0) { throw 'Synthetic fixture decode failed' }
$consumer=Join-Path $artifact 'rtc_bridge_bench.exe'
$dll=Join-Path $artifact 'gamebridge_rtc_bench.dll'
$stage='consumer'
& $consumer $DurationSeconds $fixture $dll $rawResultPath
$consumerResult=$LASTEXITCODE
$report | Add-Member consumer_exit_code $consumerResult -Force
$stage='report_read'
$parsed=Get-Content -Raw -LiteralPath $rawResultPath | ConvertFrom-Json
if($parsed -isnot [pscustomobject]){throw 'Native consumer report must be an object'}
$report=$parsed
$report | Add-Member report_schema_version 2 -Force
$report | Add-Member passed $false -Force
$report | Add-Member consumer_exit_code $consumerResult -Force
$report | Add-Member fixture_sha256 (Get-FileHash -Algorithm SHA256 -LiteralPath $fixture).Hash.ToLowerInvariant() -Force
$report | Add-Member rtc_dll_sha256 (Get-FileHash -Algorithm SHA256 -LiteralPath $dll).Hash.ToLowerInvariant() -Force
$report | Add-Member native_consumer_sha256 (Get-FileHash -Algorithm SHA256 -LiteralPath $consumer).Hash.ToLowerInvariant() -Force
$report | Add-Member gc_measurement 'not applicable: native libwebrtc DLL has no Go garbage collector' -Force
$stage='consumer'
if ($consumerResult -ne 0) { throw 'Native RTC consumer failed; no qualification claimed' }
$stage='validation'
    $validation=Test-NativeRtcGate $report $DurationSeconds
    $report | Add-Member frames_dropped_callback_queue $report.frames_dropped_receiver -Force
    $report | Add-Member frames_unaccounted_after_drain $validation.frames_unaccounted_after_drain -Force
    $report | Add-Member frames_expected_min $validation.frames_expected_min -Force
    $report.passed=$true
    $stage='complete'
} catch {
    $failed=$true
    $report | Add-Member passed $false -Force
    # Only validation's fixed metric names/numeric bounds may be echoed.
    if($stage -ceq 'validation' -and $_.Exception.Message -cmatch '^Native RTC gate assertion failed: [a-z_]+ \(actual=[0-9.eE+;-]+; minimum=[0-9.eE+;-]+; maximum=[0-9.eE+;-]+\)$'){Write-Host $_.Exception.Message}
} finally {
    $report=ConvertTo-NativeRtcDiagnostic $report $DurationSeconds $stage
    Write-NativeRtcDiagnostic $report $resultPath
}
if($failed){throw "NATIVE RTC GATE FAIL stage=$stage; no qualification claimed"}
Write-Host "NATIVE RTC GATE PASS duration=$DurationSeconds frames=$($report.frames_delivered) p95_ms=$($report.bridge_p95_ms)"
