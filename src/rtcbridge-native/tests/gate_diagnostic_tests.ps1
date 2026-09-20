$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$repository=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
. "$repository/scripts/validate-rtc-native-gate.ps1"
$scratch=Join-Path $repository ('out/native-gate-diagnostic-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
try {
    $result=Join-Path $scratch 'report.json'
    $relativeResult=$result.Substring($repository.Length+1)
    [IO.File]::WriteAllText($result,'{"passed":true,"sdp":"PRIVATE_SENTINEL"}')
    [IO.File]::WriteAllText(($result+'.consumer.json'),'{"passed":true,"sdp":"PRIVATE_SENTINEL"}')
    $script:gateLog=''
    $failed=$false
    try {
        & "$repository/scripts/test-rtc-native-gate.ps1" -DurationSeconds 5 -ArtifactDirectory $scratch -Output $relativeResult -FFmpeg (Join-Path $scratch 'missing-ffmpeg.exe') 6>&1 | ForEach-Object { $script:gateLog += [string]$_ + "`n" }
    } catch { $failed=$true }
    if(!$failed){throw 'Preparation failure was accepted'}
    $saved=Get-Content -Raw -LiteralPath $result | ConvertFrom-Json
    if($saved.passed -ne $false){throw 'Preparation failure retained PASS'}
    if($script:gateLog -notmatch 'NATIVE RTC REPORT BEGIN' -or $script:gateLog -notmatch '"passed":\s*false'){throw 'Preparation failure did not publish its complete safe diagnostic report'}
    if($saved.diagnostic_stage -cne 'fixture_generation'){throw 'Preparation failure stage was lost'}
    if($script:gateLog.Contains('PRIVATE_SENTINEL') -or (Get-Content -Raw -LiteralPath $result).Contains('PRIVATE_SENTINEL')){throw 'Stale report data escaped'}

    $report=[pscustomobject]@{
        passed=$false;frames_delivered=291;frames_submitted=300;frames_dropped_bridge=2;frames_dropped_receiver=3
        bridge_p95_ms=0.75;producer_late_frames=4;producer_bursts_under_1ms=0;producer_max_lateness_ms=24
        bridge_latency_histogram=@([pscustomobject]@{upper_us=750;count=298;secret='PRIVATE_SENTINEL'})
        rtc_backend='libwebrtc';libwebrtc_revision='c250ac7568212f05892447d8e8673f8e55d716d9'
        candidate='PRIVATE_SENTINEL';sdp='PRIVATE_SENTINEL';benchmark_error='PRIVATE_SENTINEL'
        throughput_bps='PRIVATE_SENTINEL';transport='PRIVATE_SENTINEL'
    }
    $safe=ConvertTo-NativeRtcDiagnostic $report 5 'validation'
    $log=Write-NativeRtcDiagnostic $safe $result 6>&1 | Out-String
    $json=Get-Content -Raw -LiteralPath $result
    if($json.Contains('PRIVATE_SENTINEL') -or $log.Contains('PRIVATE_SENTINEL')){throw 'Private/unknown report content leaked'}
    $saved=$json | ConvertFrom-Json
    if($saved.frames_delivered -ne 291 -or $saved.frames_dropped_callback_queue -ne 3 -or $saved.frames_unaccounted_after_drain -ne 4 -or $saved.frames_expected_min -ne 295){throw 'Failure accounting was omitted'}
    if($saved.bridge_latency_histogram[0].count -ne 298 -or $saved.producer_late_frames -ne 4){throw 'Safe measurement detail was omitted'}
    if($saved.throughput_bps -ne $null -or $saved.redacted_field_count -lt 3 -or $saved.passed -ne $false){throw 'Malformed metrics or verdict not preserved safely'}
    $incomplete=ConvertTo-NativeRtcDiagnostic ([pscustomobject]@{passed=$true}) 1800 'validation'
    if($incomplete.passed -ne $false -or $incomplete.frames_expected_min -ne 107784){throw 'Incomplete stage retained PASS or changed the full gate'}
    $consumerFailed=ConvertTo-NativeRtcDiagnostic ([pscustomobject]@{passed=$true;consumer_exit_code=1}) 5 'consumer'
    if($consumerFailed.passed -ne $false -or $consumerFailed.consumer_exit_code -ne 1){throw 'Consumer failure became a pass'}
    $fatalFields=@('fatal_header_errors','fatal_timestamp_errors','fatal_range_errors','fatal_duplicate_errors','fatal_payload_errors','fatal_bridge_errors','fatal_other_errors',
        'fatal_setup_errors','fatal_prewarm_errors','fatal_measurement_errors','fatal_drain_errors','fatal_teardown_errors',
        'fatal_bridge_control_closed_errors','fatal_bridge_pointer_closed_errors','fatal_bridge_connection_errors','fatal_bridge_other_errors',
        'payload_size_errors','payload_content_errors','payload_matches_other_fixture','bridge_error_code_mask')
    $reasons=@{passed=$false;fatal_errors=2}
    foreach($name in $fatalFields){$reasons[$name]=2}
    $reasonReport=ConvertTo-NativeRtcDiagnostic ([pscustomobject]$reasons) 5 'consumer'
    foreach($name in $fatalFields){
        $property=$reasonReport.PSObject.Properties[$name]
        if($null -eq $property -or $property.Value -ne 2){throw "Safe fatal diagnostic omitted: $name"}
        $private=$reasons.Clone();$private[$name]='PRIVATE_SENTINEL'
        $sanitized=ConvertTo-NativeRtcDiagnostic ([pscustomobject]$private) 5 'consumer' | ConvertTo-Json -Depth 10
        if($sanitized.Contains('PRIVATE_SENTINEL')){throw 'Opaque fatal detail leaked'}
    }
    $deliveryFields=@('missing_frame_count','missing_head_frames','missing_tail_frames','missing_interior_frames',
        'missing_frame_runs','missing_longest_run','missing_indices_omitted','measurement_first_rtp_timestamp','measurement_rtp_timestamp_step',
        'frames_delivered_before_drain','frames_delivered_during_drain','frames_delivered_after_drain','drain_elapsed_ms',
        'prewarm_frames_delivered','after_window_frames_delivered','keyframe_requests_total','last_frame_age_at_drain_end_ms',
        'native_video_injections_total','native_video_sender_transforms_total','native_video_receiver_transforms_total',
        'native_allocated_bitrate_bps','native_bandwidth_allocation_bps','native_bitrate_updates_total')
    foreach($role in @('sender','receiver')) {
        foreach($metric in @('outbound_packets_sent','outbound_bytes_sent','outbound_frames_encoded','outbound_frames_sent',
            'outbound_retransmitted_packets_sent','outbound_nack_count','outbound_total_packet_send_delay_seconds','outbound_target_bitrate_bps',
            'inbound_packets_received','inbound_bytes_received','inbound_packets_lost','inbound_packets_discarded','inbound_frames_received',
            'inbound_nack_count','available_outgoing_bitrate_bps','stats_age_ms')) {$deliveryFields += "native_${role}_$metric"}
    }
    $delivery=@{passed=$false;missing_frame_indices=@(0,1,119,238,299)}
    foreach($name in $deliveryFields){$delivery[$name]=7}
    $projected=ConvertTo-NativeRtcDiagnostic ([pscustomobject]$delivery) 5 'validation'
    foreach($name in $deliveryFields){if($null -eq $projected.PSObject.Properties[$name] -or $projected.$name -ne 7){throw "Safe delivery diagnostic omitted: $name"}}
    if(($projected.missing_frame_indices -join ',') -cne '0,1,119,238,299'){throw 'Missing index distribution was not retained'}
    foreach($invalidIndices in @(@('PRIVATE_SENTINEL',-1,0.5,300,[double]::NaN,[pscustomobject]@{sdp='PRIVATE_SENTINEL'}),@(0..64))) {
        $delivery.missing_frame_indices=$invalidIndices
        $projected=ConvertTo-NativeRtcDiagnostic ([pscustomobject]$delivery) 5 'validation'
        if(@($projected.missing_frame_indices).Count -ne 0 -or ($projected | ConvertTo-Json -Depth 10).Contains('PRIVATE_SENTINEL')){throw 'Unsafe/unbounded missing indices escaped'}
    }
    $delivery.missing_frame_indices=@(0..63)
    $projected=ConvertTo-NativeRtcDiagnostic ([pscustomobject]$delivery) 5 'validation'
    if(@($projected.missing_frame_indices).Count -ne 64){throw 'Valid bounded missing index sample rejected'}

    $launcher=Get-Content -Raw -LiteralPath "$repository/scripts/test-rtc-native-gate.ps1"
    if($launcher -notmatch '& \$consumer \$DurationSeconds \$fixture \$dll \$rawResultPath' -or $launcher -match '& \$consumer .* \$resultPath'){throw 'Raw consumer output overlaps the published sanitized report'}

    $workflow=Get-Content -Raw -LiteralPath "$repository/.github/workflows/build-libwebrtc.yml"
    if($workflow -notmatch "if:.*always\(\)" -or $workflow -notmatch 'path: out/rtc-short-gate\.json' -or $workflow -match 'continue-on-error: true'){throw 'Workflow does not retain only the diagnostic report without ignoring failure'}
    Write-Host 'Native diagnostics: real preparation failure, full safe numeric report, private-field rejection and failure upload policy PASS'
} finally {
    $parent=[IO.Path]::GetFullPath((Join-Path $repository 'out'))+'\'
    $resolved=[IO.Path]::GetFullPath($scratch)
    if(!$resolved.StartsWith($parent,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolved) -notmatch '^native-gate-diagnostic-test-[0-9a-f]{32}$'){throw 'Unsafe diagnostic fixture cleanup'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
