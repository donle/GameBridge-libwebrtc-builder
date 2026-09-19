function Test-NativeRtcGate {
    param([Parameter(Mandatory)]$Report, [Parameter(Mandatory)][int]$DurationSeconds)
    $minimum = if ($DurationSeconds -eq 1800) { 107900 } else { [math]::Floor($DurationSeconds * 60 - [math]::Max(5, $DurationSeconds / 18)) }
    $ranges = @{
        duration_seconds=@($DurationSeconds, ($DurationSeconds + 5))
        frames_delivered=@($minimum, ($DurationSeconds * 60))
        frames_submitted=@($minimum, ($DurationSeconds * 60))
        bridge_latency_samples=@($minimum, ($DurationSeconds * 60))
        bridge_p95_ms=@(0,2); video_queue_depth_max=@(1,1); memory_growth_mib=@(0,64)
        fatal_errors=@(0,0); stale_handle_callbacks=@(0,0); metrics_errors=@(0,0)
        throughput_bps=@(23750000,26250000); gc_pause_ms_max=@(0,0); gc_cycles=@(0,0)
        frames_dropped_bridge=@(0,($DurationSeconds*60)); frames_dropped_receiver=@(0,($DurationSeconds*60))
        submission_failures=@(0,($DurationSeconds*60))
    }
    foreach ($name in $ranges.Keys) {
        $property=$Report.PSObject.Properties[$name]
        if ($null -eq $property -or $null -eq $property.Value -or $property.Value -is [string] -or $property.Value -is [bool]) { throw "Missing/non-numeric native RTC metric: $name" }
        $number=[double]$property.Value
        if ([double]::IsNaN($number) -or [double]::IsInfinity($number) -or $number -lt $ranges[$name][0] -or $number -gt $ranges[$name][1]) { throw "Native RTC gate assertion failed: $name" }
        if ($name -match '^(frames_|bridge_latency_samples|video_queue_depth_max|fatal_errors|stale_handle_callbacks|metrics_errors|submission_failures|gc_cycles)' -and [math]::Floor($number) -ne $number) { throw "Non-integer native RTC counter: $name" }
    }
    if ($Report.rtc_backend -cne 'libwebrtc' -or $Report.libwebrtc_revision -cne 'c250ac7568212f05892447d8e8673f8e55d716d9' -or $Report.gc_applicable -isnot [bool] -or $Report.gc_applicable) { throw 'Native gate requires the pinned non-Go backend' }
    $residual=$Report.frames_submitted-$Report.frames_delivered-$Report.frames_dropped_bridge-$Report.frames_dropped_receiver
    if ($residual -lt 0 -or ($Report.frames_submitted+$Report.submission_failures) -ne ($DurationSeconds*60)) { throw 'Inconsistent native RTC drop accounting' }
    [pscustomobject]@{ frames_expected_min=$minimum; frames_unaccounted_after_drain=$residual; passed=$true }
}
