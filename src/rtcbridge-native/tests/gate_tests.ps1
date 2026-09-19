$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. "$PSScriptRoot/../../../scripts/validate-rtc-native-gate.ps1"
function Expect-Failure($Action) {
    $failed = $false
    try { & $Action } catch { $failed = $true }
    if (!$failed) { throw 'Expected gate rejection' }
}
$valid = @{
    rtc_backend='libwebrtc'; gc_applicable=$false; gc_pause_ms_max=0; gc_cycles=0
    libwebrtc_revision='c250ac7568212f05892447d8e8673f8e55d716d9'
    duration_seconds=5; frames_delivered=300; frames_submitted=300; bridge_latency_samples=300
    bridge_p95_ms=0.5; video_queue_depth_max=1; memory_growth_mib=5
    fatal_errors=0; stale_handle_callbacks=0; metrics_errors=0; throughput_bps=25000000
    frames_dropped_bridge=0; frames_dropped_receiver=0; submission_failures=0
}
Test-NativeRtcGate ([pscustomobject]$valid) 5 | Out-Null
foreach ($case in @(@('frames_delivered',294),@('bridge_p95_ms',2.01),@('memory_growth_mib',65),
    @('video_queue_depth_max',2),@('fatal_errors',1),@('throughput_bps',22000000),
    @('rtc_backend','pion'),@('gc_applicable',$true),@('metrics_errors',1),
    @('duration_seconds',4.99),@('bridge_latency_samples',294))) {
    $changed = $valid.Clone(); $changed[$case[0]]=$case[1]
    Expect-Failure { Test-NativeRtcGate ([pscustomobject]$changed) 5 }
}
$missing=$valid.Clone(); $missing.Remove('bridge_p95_ms')
Expect-Failure { Test-NativeRtcGate ([pscustomobject]$missing) 5 }
$invalid=$valid.Clone();$invalid.bridge_p95_ms=[double]::NaN
Expect-Failure { Test-NativeRtcGate ([pscustomobject]$invalid) 5 }
$full=$valid.Clone();$full.duration_seconds=1800;$full.frames_submitted=108000;$full.frames_delivered=107899;$full.bridge_latency_samples=108000
Expect-Failure { Test-NativeRtcGate ([pscustomobject]$full) 1800 }
$full.frames_delivered=107900
Test-NativeRtcGate ([pscustomobject]$full) 1800 | Out-Null
Write-Host 'native gate validator: threshold, missing metric, backend, NaN, full-duration boundary PASS'
