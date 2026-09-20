param([Alias('Report')][string]$ValidationReportPath,
      [Alias('DurationSeconds')][ValidateRange(5,7200)][int]$ValidationDurationSeconds=1800)

function Get-NativeRtcMinimumFrames([int]$DurationSeconds) {
    if ($DurationSeconds -eq 1800) { return [math]::Ceiling(($DurationSeconds * 60) * 0.998) }
    [math]::Floor($DurationSeconds * 60 - [math]::Max(5, $DurationSeconds / 18))
}

function Test-NativeRtcGate {
    param([Parameter(Mandatory)]$Report, [Parameter(Mandatory)][int]$DurationSeconds)
    $minimum = Get-NativeRtcMinimumFrames $DurationSeconds
    $ranges = [ordered]@{
        fatal_errors=@(0,0); stale_handle_callbacks=@(0,0); metrics_errors=@(0,0)
        duration_seconds=@($DurationSeconds, ($DurationSeconds + 5))
        frames_delivered=@($minimum, ($DurationSeconds * 60))
        frames_submitted=@($minimum, ($DurationSeconds * 60))
        bridge_latency_samples=@($minimum, ($DurationSeconds * 60))
        bridge_p95_ms=@(0,2); video_queue_depth_max=@(1,1); memory_growth_mib=@(0,64)
        throughput_bps=@(23750000,26250000); gc_pause_ms_max=@(0,0); gc_cycles=@(0,0)
        frames_dropped_bridge=@(0,($DurationSeconds*60)); frames_dropped_receiver=@(0,($DurationSeconds*60))
        submission_failures=@(0,($DurationSeconds*60))
    }
    foreach ($name in @('consumer_exit_code','fatal_header_errors','fatal_timestamp_errors','fatal_range_errors',
        'fatal_duplicate_errors','fatal_payload_errors','fatal_bridge_errors','fatal_other_errors',
        'fatal_setup_errors','fatal_prewarm_errors','fatal_measurement_errors','fatal_drain_errors','fatal_teardown_errors',
        'fatal_bridge_control_closed_errors','fatal_bridge_pointer_closed_errors','fatal_bridge_connection_errors','fatal_bridge_other_errors',
        'payload_size_errors','payload_content_errors','payload_matches_other_fixture','bridge_error_code_mask')) {
        $ranges[$name]=@(0,0)
    }
    $ranges.missing_frame_count=@(0,($DurationSeconds*60))
    $ranges.missing_frame_runs=@(0,($DurationSeconds*60))
    $ranges.missing_longest_run=@(0, $(if ($DurationSeconds -eq 1800) { 12 } else { $DurationSeconds*60 }))
    foreach ($name in $ranges.Keys) {
        $property=$Report.PSObject.Properties[$name]
        if ($null -eq $property -or $null -eq $property.Value -or $property.Value -is [string] -or $property.Value -is [bool]) { throw "Missing/non-numeric native RTC metric: $name" }
        $number=[double]$property.Value
        if ([double]::IsNaN($number) -or [double]::IsInfinity($number) -or $number -lt $ranges[$name][0] -or $number -gt $ranges[$name][1]) { throw "Native RTC gate assertion failed: $name (actual=$number; minimum=$($ranges[$name][0]); maximum=$($ranges[$name][1]))" }
        if ($name -match '^(frames_|missing_|bridge_latency_samples|video_queue_depth_max|fatal_|stale_handle_callbacks|metrics_errors|submission_failures|gc_cycles|consumer_exit_code|payload_|bridge_error_code_mask)' -and [math]::Floor($number) -ne $number) { throw "Non-integer native RTC counter: $name" }
    }
    if ($Report.rtc_backend -cne 'libwebrtc' -or $Report.libwebrtc_revision -cne 'c250ac7568212f05892447d8e8673f8e55d716d9' -or $Report.gc_applicable -isnot [bool] -or $Report.gc_applicable) { throw 'Native gate requires the pinned non-Go backend' }
    $requiredStrings=[ordered]@{
        fixture_sha256='^[a-f0-9]{64}$';rtc_dll_sha256='^[a-f0-9]{64}$';native_consumer_sha256='^[a-f0-9]{64}$'
        route='^direct$';transport='^C ABI/RTC/DTLS-SRTP/UDP/C callback$'
        payload_validation='^all complete AUs matched fixture FNV-1a64$'
        gc_measurement='^not applicable: native libwebrtc DLL has no Go garbage collector$'
    }
    foreach($name in $requiredStrings.Keys){
        $property=$Report.PSObject.Properties[$name]
        if($null -eq $property -or $property.Value -isnot [string] -or $property.Value -cnotmatch $requiredStrings[$name]){throw "Missing/invalid native RTC identity: $name"}
    }
    $residual=$Report.frames_submitted-$Report.frames_delivered-$Report.frames_dropped_bridge-$Report.frames_dropped_receiver
    if ($residual -lt 0 -or ($Report.frames_submitted+$Report.submission_failures) -ne ($DurationSeconds*60)) { throw 'Inconsistent native RTC drop accounting' }
    $missing=($DurationSeconds*60)-$Report.frames_delivered
    if($Report.missing_frame_count -ne $missing -or
       ($missing -eq 0 -and ($Report.missing_frame_runs -ne 0 -or $Report.missing_longest_run -ne 0)) -or
       ($missing -gt 0 -and ($Report.missing_frame_runs -lt 1 -or $Report.missing_frame_runs -gt $missing -or
                            $Report.missing_longest_run -lt 1 -or $Report.missing_longest_run -gt $missing -or
                            $Report.missing_longest_run -gt ($missing-$Report.missing_frame_runs+1) -or
                            $missing -gt ($Report.missing_frame_runs*$Report.missing_longest_run)))){
        throw 'Inconsistent native RTC missing-frame distribution'
    }
    [pscustomobject]@{ frames_expected_min=$minimum; frames_unaccounted_after_drain=$residual; passed=$true }
}

function Test-NativeRtcDiagnosticNumber($Value) {
    if($null -eq $Value -or $Value.GetType().Name -notin @('Byte','SByte','Int16','UInt16','Int32','UInt32','Int64','UInt64','Single','Double','Decimal')){return $false}
    ![double]::IsNaN([double]$Value) -and ![double]::IsInfinity([double]$Value)
}

# Evidence integrity is separate from Test-NativeRtcGate's unchanged acceptance
# thresholds. Schema 2 adds routine-probe sample partitions; schema 1 remains
# valid for preserved historical evidence. Both contain fixed numbers only.
function Get-NativeRtcEvidenceFields([int]$NetworkSchema=2) {
    $fields=@('network_evidence_schema','producer_evidence_schema','fatal_producer_errors','fatal_callback_other_errors')
    foreach($source in @('video','audio','data')){foreach($result in @('attempts','ok','backpressure','state','invalid','closed','other')){$fields+="producer_${source}_$result"}}
    foreach($peer in @('sender','receiver')){
        if($NetworkSchema-ge 2){foreach($sample in @('samples','succeeded_samples','retained_routine_probes','unproven_samples')){$fields+="native_${peer}_proof_$sample"}}
        foreach($transition in @('initial','regressions','recoveries','terminal','transitions')){$fields+="native_${peer}_proof_$transition"}
        foreach($reason in @('missing_selected_pair','missing_pair_record','missing_candidate_record','missing_candidate_type','pair_not_succeeded','relay','timeout','other')){
            $fields+="native_${peer}_regression_$reason";$fields+="native_${peer}_terminal_$reason"
        }
    }
    $fields
}
function Test-NativeRtcEvidence($Report) {
    if($Report.network_evidence_schema-notin @(1,2) -or $Report.producer_evidence_schema-ne 1){throw 'Unsupported numeric network evidence schema'}
    foreach($field in ((Get-NativeRtcEvidenceFields -NetworkSchema $Report.network_evidence_schema)+@('fatal_other_errors','frames_submitted','submission_failures'))){
        $property=$Report.PSObject.Properties[$field]
        if($null-eq $property -or !(Test-NativeRtcDiagnosticNumber $property.Value) -or
           $property.Value-lt 0 -or $property.Value-gt 9007199254740991 -or [math]::Floor($property.Value)-ne $property.Value){throw "Invalid numeric network evidence: $field"}
    }
    [long]$producerFatal=0
    foreach($source in @('video','audio','data')){
        [long]$sum=0
        foreach($result in @('ok','backpressure','state','invalid','closed','other')){$sum+=$Report."producer_${source}_$result"}
        if($sum-ne $Report."producer_${source}_attempts"){throw 'Producer result partition mismatch'}
        foreach($result in @('state','invalid','closed','other')){$producerFatal+=$Report."producer_${source}_$result"}
    }
    if($Report.producer_video_ok-ne $Report.frames_submitted -or
       ($Report.producer_video_attempts-$Report.producer_video_ok)-ne $Report.submission_failures -or
       $producerFatal-ne $Report.fatal_producer_errors -or
       ($Report.fatal_producer_errors+$Report.fatal_callback_other_errors)-ne $Report.fatal_other_errors){throw 'Producer/frame/fatal origin partition mismatch'}
    foreach($peer in @('sender','receiver')){
        if($Report.network_evidence_schema-eq 2){
            $samples=$Report."native_${peer}_proof_succeeded_samples"+$Report."native_${peer}_proof_retained_routine_probes"+$Report."native_${peer}_proof_unproven_samples"
            if($samples-ne $Report."native_${peer}_proof_samples" -or
               ($Report."native_${peer}_proof_retained_routine_probes"-gt 0 -and $Report."native_${peer}_proof_initial"-ne 1)){
                throw 'Direct proof sample partition mismatch'
            }
        }
        [long]$sum=0;foreach($transition in @('initial','regressions','recoveries','terminal')){$sum+=$Report."native_${peer}_proof_$transition"}
        if($sum-ne $Report."native_${peer}_proof_transitions" -or $Report."native_${peer}_proof_initial"-gt 1 -or
           $Report."native_${peer}_proof_terminal"-gt 1 -or $Report."native_${peer}_proof_recoveries"-gt $Report."native_${peer}_proof_regressions"){throw 'Direct proof transition partition mismatch'}
        foreach($kind in @('regression','terminal')){
            [long]$reasons=0
            foreach($reason in @('missing_selected_pair','missing_pair_record','missing_candidate_record','missing_candidate_type','pair_not_succeeded','relay','timeout','other')){$reasons+=$Report."native_${peer}_${kind}_$reason"}
            $count=if($kind-ceq 'regression'){'regressions'}else{'terminal'}
            if($reasons-ne $Report."native_${peer}_proof_$count"){throw 'Direct proof reason partition mismatch'}
        }
    }
}

function ConvertTo-NativeRtcDiagnostic {
    param([Parameter(Mandatory)]$Report,[int]$DurationSeconds,
        [ValidateSet('preparation','fixture_generation','fixture_decode','consumer','report_read','validation','complete')][string]$Stage)
    # Only fixed-schema numbers, booleans, content hashes and known constant
    # strings reach logs/artifacts. Never serialize opaque exceptions/SDP/ICE.
    $numeric=@('duration_seconds','requested_duration_seconds','prewarm_seconds','frames_submitted','submission_failures',
        'frames_delivered','throughput_bps','submitted_bps','bridge_p95_ms','bridge_latency_samples','video_queue_depth_max',
        'memory_growth_mib','memory_ending_growth_mib','native_memory_baseline_bytes','native_memory_peak_bytes','native_memory_ending_bytes',
        'fatal_errors','stale_handle_callbacks','metrics_errors','gc_pause_ms_max','gc_cycles','frames_dropped_bridge','frames_dropped_receiver',
        'fatal_header_errors','fatal_timestamp_errors','fatal_range_errors','fatal_duplicate_errors','fatal_payload_errors','fatal_bridge_errors','fatal_other_errors',
        'fatal_setup_errors','fatal_prewarm_errors','fatal_measurement_errors','fatal_drain_errors','fatal_teardown_errors',
        'fatal_bridge_control_closed_errors','fatal_bridge_pointer_closed_errors','fatal_bridge_connection_errors','fatal_bridge_other_errors',
        'payload_size_errors','payload_content_errors','payload_matches_other_fixture','bridge_error_code_mask',
        'producer_late_frames','producer_bursts_under_1ms','producer_max_lateness_ms','producer_catchup_minimum_interval_ms',
        'missing_frame_count','missing_head_frames','missing_tail_frames','missing_interior_frames','missing_frame_runs','missing_longest_run','missing_indices_omitted',
        'measurement_first_rtp_timestamp','measurement_rtp_timestamp_step','frames_delivered_before_drain','frames_delivered_during_drain','frames_delivered_after_drain',
        'drain_elapsed_ms','prewarm_frames_delivered','after_window_frames_delivered','keyframe_requests_total','last_frame_age_at_drain_end_ms',
        'native_video_injections_total','native_video_sender_transforms_total','native_video_receiver_transforms_total',
        'native_allocated_bitrate_bps','native_bandwidth_allocation_bps','native_bitrate_updates_total',
        'width','height','fps','consumer_exit_code')
    foreach($role in @('sender','receiver')) {
        foreach($metric in @('outbound_packets_sent','outbound_bytes_sent','outbound_frames_encoded','outbound_frames_sent',
            'outbound_retransmitted_packets_sent','outbound_nack_count','outbound_total_packet_send_delay_seconds','outbound_target_bitrate_bps',
            'inbound_packets_received','inbound_bytes_received','inbound_packets_lost','inbound_packets_discarded','inbound_frames_received',
            'inbound_nack_count','available_outgoing_bitrate_bps','stats_age_ms')) {$numeric += "native_${role}_$metric"}
    }
    $evidenceFields=@(Get-NativeRtcEvidenceFields)
    $numeric+=$evidenceFields
    $safe=[ordered]@{report_schema_version=2;passed=$false;diagnostic_stage=$Stage;requested_duration_seconds=$DurationSeconds;frames_expected_min=(Get-NativeRtcMinimumFrames $DurationSeconds)}
    $verdict=$Report.PSObject.Properties['passed']
    if($Stage -ceq 'complete' -and $null -ne $verdict -and $verdict.Value -is [bool]){$safe.passed=$verdict.Value}
    $redacted=0
    foreach($name in $numeric){
        $property=$Report.PSObject.Properties[$name]
        if($null -eq $property){continue}
        $validNumber=Test-NativeRtcDiagnosticNumber $property.Value
        if($validNumber -and $name -cin $evidenceFields){$validNumber=$property.Value-ge 0 -and $property.Value-le 9007199254740991 -and [math]::Floor($property.Value)-eq $property.Value}
        if($validNumber){$safe[$name]=$property.Value}else{$safe[$name]=$null;$redacted++}
    }
    $strings=@{
        rtc_backend='^(libwebrtc|pion)$';libwebrtc_revision='^[a-f0-9]{40}$'
        fixture_sha256='^[a-f0-9]{64}$';rtc_dll_sha256='^[a-f0-9]{64}$';native_consumer_sha256='^[a-f0-9]{64}$'
        route='^(direct|relay)$';transport='^C ABI/RTC/DTLS-SRTP/UDP/C callback$'
        payload_validation='^all complete AUs matched fixture FNV-1a64$'
        gc_measurement='^not applicable: native libwebrtc DLL has no Go garbage collector$'
        validation_policy='^native-rtc-experience-v2$'
        source_report_sha256='^[a-f0-9]{64}$'
    }
    foreach($name in $strings.Keys){
        $property=$Report.PSObject.Properties[$name]
        if($null -eq $property){continue}
        if($property.Value -is [string] -and $property.Value -cmatch $strings[$name]){$safe[$name]=$property.Value}else{$safe[$name]=$null;$redacted++}
    }
    $gc=$Report.PSObject.Properties['gc_applicable']
    if($null -ne $gc -and $gc.Value -is [bool]){$safe.gc_applicable=$gc.Value}
    $histogram=$Report.PSObject.Properties['bridge_latency_histogram']
    if($null -ne $histogram){
        $buckets=@()
        $items=@($histogram.Value)
        if($items.Count -le 4096){
            foreach($item in $items){
                if($null -eq $item){$redacted++;continue}
                $upper=$item.PSObject.Properties['upper_us'];$count=$item.PSObject.Properties['count']
                if($null -ne $upper -and $null -ne $count -and (Test-NativeRtcDiagnosticNumber $upper.Value) -and (Test-NativeRtcDiagnosticNumber $count.Value)){
                    $buckets += [ordered]@{upper_us=$upper.Value;count=$count.Value}
                }else{$redacted++}
            }
        }else{$redacted++}
        $safe.bridge_latency_histogram=$buckets
    }
    $missing=$Report.PSObject.Properties['missing_frame_indices']
    if($null -ne $missing){
        $indices=@();$previous=-1
        if($missing.Value -is [Array] -and $missing.Value.Count -le 64){
            foreach($index in $missing.Value){
                if((Test-NativeRtcDiagnosticNumber $index) -and $index -gt $previous -and $index -ge 0 -and $index -lt ($DurationSeconds*60) -and [math]::Floor($index) -eq $index){
                    $indices += $index;$previous=$index
                }else{$redacted++}
            }
        }else{$redacted++}
        $safe.missing_frame_indices=$indices
    }
    $known=$numeric+@($strings.Keys)+@('report_schema_version','passed','gc_applicable','bridge_latency_histogram','missing_frame_indices','frames_expected_min','frames_dropped_callback_queue','frames_unaccounted_after_drain','diagnostic_stage','redacted_field_count')
    foreach($property in $Report.PSObject.Properties){if($property.Name -notin $known){$redacted++}}
    if($safe.Contains('frames_dropped_receiver')){$safe.frames_dropped_callback_queue=$safe.frames_dropped_receiver}
    $accounting=@('frames_submitted','frames_delivered','frames_dropped_bridge','frames_dropped_receiver')
    if(@($accounting | Where-Object { !$safe.Contains($_) -or !(Test-NativeRtcDiagnosticNumber $safe[$_]) }).Count -eq 0){
        $safe.frames_unaccounted_after_drain=$safe.frames_submitted-$safe.frames_delivered-$safe.frames_dropped_bridge-$safe.frames_dropped_receiver
    }
    $safe.redacted_field_count=$redacted
    [pscustomobject]$safe
}

function Test-NativeRtcTransientReplaceError([Exception]$Exception) {
    # Static .NET calls are wrapped by PowerShell; only unwrap that invocation
    # wrapper, never convert arbitrary failures into retryable IO failures.
    while($Exception -is [Management.Automation.MethodInvocationException] -and $null -ne $Exception.InnerException){$Exception=$Exception.InnerException}
    if($Exception -isnot [IO.IOException]){return $false}
    $hresult=[long]$Exception.HResult -band 0xffffffffL
    if(($hresult -band 0xffff0000L) -ne 0x80070000L){return $false}
    # Access/sharing/lock and unable-to-remove-replaced preserve the original
    # names. Do NOT retry 1176/1177: ReplaceFile documents different recovery.
    ($hresult -band 0xffffL) -in @(5,32,33,1175)
}

function Write-NativeRtcDiagnostic($Report,[string]$Path,[switch]$Quiet) {
    # Serialize first. A serializer exception cannot touch the last valid JSON.
    $json=$Report | ConvertTo-Json -Depth 10 -ErrorAction Stop
    $bytes=[Text.UTF8Encoding]::new($false,$true).GetBytes($json+"`n")
    $destination=[IO.Path]::GetFullPath($Path)
    if([IO.Directory]::Exists($destination)){throw 'Published report path must be a file'}
    $directory=[IO.Path]::GetDirectoryName($destination)
    $temporary=Join-Path $directory ('.rtc-report-'+[guid]::NewGuid().ToString('N')+'.tmp')
    $stream=$null
    try {
        $stream=[IO.FileStream]::new($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None,4096,[IO.FileOptions]::WriteThrough)
        $stream.Write($bytes,0,$bytes.Length)
        $stream.Flush($true)
        $stream.Dispose();$stream=$null
        if([IO.File]::Exists($destination)) {
            # Move-Item -Force deletes an existing destination before moving
            # in PowerShell's provider. ReplaceFile preserves atomic overwrite.
            # Retry only this same atomic operation: <=41 attempts, <=1000 ms
            # elapsed retry budget, <=25 ms between attempts. An OS call itself
            # is synchronous, so this is not a filesystem-call timeout. Neither
            # serialization, preparation nor the benchmark is repeated.
            $retryTimer=[Diagnostics.Stopwatch]::StartNew()
            $attempt=0
            while($true){
                $attempt++
                try {
                    [IO.File]::Replace($temporary,$destination,[NullString]::Value)
                    break
                } catch {
                    if(!(Test-NativeRtcTransientReplaceError $_.Exception) -or $attempt -ge 41 -or $retryTimer.ElapsedMilliseconds -ge 1000 -or ![IO.File]::Exists($temporary) -or ![IO.File]::Exists($destination)){throw}
                    $delay=[math]::Min(25,[math]::Max(1,1000-$retryTimer.ElapsedMilliseconds))
                    Start-Sleep -Milliseconds $delay
                    if($retryTimer.ElapsedMilliseconds -ge 1000){throw}
                }
            }
        } else {
            # Same-directory first publication is a rename, without -Force or
            # a delete/copy fallback if another publisher creates the target.
            Move-Item -LiteralPath $temporary -Destination $destination -ErrorAction Stop
        }
    } finally {
        if($null -ne $stream){$stream.Dispose()}
        # Only our exact GUID-scoped temporary file is removed. Cancellation
        # may leave it behind; the workflow uploads only the published filename.
        if([IO.File]::Exists($temporary)){Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue}
    }
    if(!$Quiet){
        Write-Host 'NATIVE RTC REPORT BEGIN'
        Write-Host $json
        Write-Host 'NATIVE RTC REPORT END'
    }
}

if ($ValidationReportPath) {
    $validatedReport=Get-Content -Raw -LiteralPath $ValidationReportPath | ConvertFrom-Json
    Test-NativeRtcGate $validatedReport $ValidationDurationSeconds | Out-Null
    if($null-ne $validatedReport.PSObject.Properties['network_evidence_schema']){Test-NativeRtcEvidence $validatedReport}
    if ($validatedReport.passed -isnot [bool] -or !$validatedReport.passed) { throw 'Native RTC report does not claim a completed passing gate' }
    Write-Host "NATIVE RTC VALIDATION PASS duration=$ValidationDurationSeconds route=direct"
}
