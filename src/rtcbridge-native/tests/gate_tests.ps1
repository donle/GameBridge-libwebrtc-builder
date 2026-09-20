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
    consumer_exit_code=0; missing_frame_count=0; missing_frame_runs=0; missing_longest_run=0
    fatal_header_errors=0; fatal_timestamp_errors=0; fatal_range_errors=0; fatal_duplicate_errors=0
    fatal_payload_errors=0; fatal_bridge_errors=0; fatal_other_errors=0
    fatal_setup_errors=0; fatal_prewarm_errors=0; fatal_measurement_errors=0; fatal_drain_errors=0; fatal_teardown_errors=0
    fatal_bridge_control_closed_errors=0; fatal_bridge_pointer_closed_errors=0; fatal_bridge_connection_errors=0; fatal_bridge_other_errors=0
    payload_size_errors=0; payload_content_errors=0; payload_matches_other_fixture=0; bridge_error_code_mask=0
    fixture_sha256=('a'*64); rtc_dll_sha256=('b'*64); native_consumer_sha256=('c'*64)
    route='direct'; transport='C ABI/RTC/DTLS-SRTP/UDP/C callback'
    payload_validation='all complete AUs matched fixture FNV-1a64'
    gc_measurement='not applicable: native libwebrtc DLL has no Go garbage collector'
}
Test-NativeRtcGate ([pscustomobject]$valid) 5 | Out-Null
$relay=$valid.Clone();$relay.route='relay'
Expect-Failure { Test-NativeRtcGate ([pscustomobject]$relay) 5 }
$boundary=$valid.Clone();$boundary.frames_delivered=295;$boundary.missing_frame_count=5;$boundary.missing_frame_runs=5;$boundary.missing_longest_run=1
Test-NativeRtcGate ([pscustomobject]$boundary) 5 | Out-Null
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
$full=$valid.Clone();$full.duration_seconds=1800;$full.frames_submitted=108000;$full.frames_delivered=107783;$full.bridge_latency_samples=108000;$full.missing_frame_count=217;$full.missing_frame_runs=100;$full.missing_longest_run=12
Expect-Failure { Test-NativeRtcGate ([pscustomobject]$full) 1800 }
$full.frames_delivered=107784
$full.missing_frame_count=216
Test-NativeRtcGate ([pscustomobject]$full) 1800 | Out-Null
$full.missing_longest_run=13
Expect-Failure { Test-NativeRtcGate ([pscustomobject]$full) 1800 }
$forged=$full.Clone();$forged.missing_longest_run=0;$forged.frames_submitted=107987;$forged.submission_failures=13;$forged.frames_delivered=107987;$forged.bridge_latency_samples=107987;$forged.missing_frame_count=13;$forged.missing_frame_runs=1
Expect-Failure { Test-NativeRtcGate ([pscustomobject]$forged) 1800 }
$forged.missing_longest_run=12
Expect-Failure { Test-NativeRtcGate ([pscustomobject]$forged) 1800 }
$forged=$full.Clone();$forged.missing_longest_run=8.5
Expect-Failure { Test-NativeRtcGate ([pscustomobject]$forged) 1800 }
foreach($field in @('consumer_exit_code','payload_content_errors','fatal_payload_errors')) {
    $forged=$full.Clone();$forged.missing_longest_run=12;$forged[$field]=1
    Expect-Failure { Test-NativeRtcGate ([pscustomobject]$forged) 1800 }
}
foreach($field in @('fixture_sha256','rtc_dll_sha256','native_consumer_sha256','payload_validation','transport','route','gc_measurement')) {
    $forged=$full.Clone();$forged.missing_longest_run=12;$forged.Remove($field)
    Expect-Failure { Test-NativeRtcGate ([pscustomobject]$forged) 1800 }
}
$changed=$valid.Clone();$changed.frames_delivered=294
try { Test-NativeRtcGate ([pscustomobject]$changed) 5; throw 'Expected frame rejection' }
catch { if($_.Exception.Message -notmatch 'frames_delivered.*actual=294.*minimum=295.*maximum=300'){throw 'Gate failure must report the actual value and unchanged bounds'} }
# An error must not be hidden by whichever hashtable key happens to enumerate
# first. Reproduce the failed hosted shape plus a simultaneous frame failure.
for($iteration=0;$iteration -lt 100;$iteration++) {
    $changed=$valid.Clone();$changed.frames_delivered=294;$changed.fatal_errors=2
    try { Test-NativeRtcGate ([pscustomobject]$changed) 5; throw 'Expected fatal rejection' }
    catch { if($_.Exception.Message -notmatch 'fatal_errors.*actual=2.*minimum=0.*maximum=0'){throw 'Fatal validation must deterministically precede throughput/frame thresholds'} }
}
$successful=$valid.Clone();$successful.passed=$true
$projected=ConvertTo-NativeRtcDiagnostic ([pscustomobject]$successful) 5 'complete'
if(!$projected.passed){throw 'Validated success lost its verdict'}
Test-NativeRtcGate $projected 5 | Out-Null
Write-Host 'native gate validator: delivery rate, freeze run, missing metric, backend, NaN PASS'
