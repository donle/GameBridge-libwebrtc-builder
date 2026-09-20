$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$testRoot = [IO.Path]::GetFullPath((Join-Path $repository 'out/gate-experience-test'))
$source = Join-Path $testRoot 'source.json'
$decision = Join-Path $testRoot 'decision.json'
$reparseTarget = [IO.Path]::GetFullPath((Join-Path $repository 'out/gate-experience-test-target'))
$junction = Join-Path $testRoot 'report-junction'
$revalidator = Join-Path $repository 'scripts/revalidate-rtc-native-gate.ps1'

$safeOut = [IO.Path]::GetFullPath((Join-Path $repository 'out')).TrimEnd('\') + '\'
if (-not $testRoot.StartsWith($safeOut, [StringComparison]::OrdinalIgnoreCase) -or
    -not $reparseTarget.StartsWith($safeOut, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Unsafe experience-gate test root'
}
Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $reparseTarget -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $testRoot | Out-Null

$report = [ordered]@{
    report_schema_version = 2
    passed = $false
    diagnostic_stage = 'validation'
    requested_duration_seconds = 1800
    rtc_backend = 'libwebrtc'
    gc_applicable = $false
    gc_pause_ms_max = 0
    gc_cycles = 0
    libwebrtc_revision = 'c250ac7568212f05892447d8e8673f8e55d716d9'
    duration_seconds = 1800.0002
    frames_delivered = 107888
    frames_submitted = 108000
    bridge_latency_samples = 107985
    bridge_p95_ms = 0.06
    video_queue_depth_max = 1
    memory_growth_mib = 13.8
    fatal_errors = 0
    stale_handle_callbacks = 0
    metrics_errors = 0
    throughput_bps = 25167448
    frames_dropped_bridge = 15
    frames_dropped_receiver = 89
    submission_failures = 0
    consumer_exit_code = 0
    missing_frame_count = 112
    missing_frame_runs = 99
    missing_longest_run = 8
    fatal_header_errors = 0
    fatal_timestamp_errors = 0
    fatal_range_errors = 0
    fatal_duplicate_errors = 0
    fatal_payload_errors = 0
    fatal_bridge_errors = 0
    fatal_other_errors = 0
    fatal_setup_errors = 0
    fatal_prewarm_errors = 0
    fatal_measurement_errors = 0
    fatal_drain_errors = 0
    fatal_teardown_errors = 0
    fatal_bridge_control_closed_errors = 0
    fatal_bridge_pointer_closed_errors = 0
    fatal_bridge_connection_errors = 0
    fatal_bridge_other_errors = 0
    payload_size_errors = 0
    payload_content_errors = 0
    payload_matches_other_fixture = 0
    bridge_error_code_mask = 0
    fixture_sha256 = ('a' * 64)
    rtc_dll_sha256 = ('b' * 64)
    native_consumer_sha256 = ('c' * 64)
    route = 'direct'
    transport = 'C ABI/RTC/DTLS-SRTP/UDP/C callback'
    payload_validation = 'all complete AUs matched fixture FNV-1a64'
    gc_measurement = 'not applicable: native libwebrtc DLL has no Go garbage collector'
}

try {
    $report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $source -Encoding UTF8
    $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()

    & $revalidator -Input $source -Output $decision -DurationSeconds 1800

    $result = Get-Content -LiteralPath $decision -Raw | ConvertFrom-Json
    if (-not $result.passed -or $result.validation_policy -cne 'native-rtc-experience-v2') {
        throw 'Experience decision did not publish a v2 pass'
    }
    if ($result.source_report_sha256 -cne $sourceHash) { throw 'Experience decision source hash mismatch' }
    if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -cne $sourceHash) {
        throw 'Experience revalidation modified its source report'
    }

    $collisionRejected = $false
    try { & $revalidator -Input $source -Output $source -DurationSeconds 1800 }
    catch { $collisionRejected = $_.Exception.Message -match 'different' }
    if (-not $collisionRejected) { throw 'Experience revalidator accepted identical input/output paths' }

    New-Item -ItemType Directory -Path $reparseTarget | Out-Null
    New-Item -ItemType Junction -Path $junction -Target $reparseTarget | Out-Null
    $reparseRejected = $false
    try { & $revalidator -Input $source -Output (Join-Path $junction 'decision.json') -DurationSeconds 1800 }
    catch { $reparseRejected = $_.Exception.Message -match 'reparse' }
    if (-not $reparseRejected) { throw 'Experience revalidator accepted a reparse-point output path' }

    Write-Host 'native experience gate: immutable hash-linked revalidation PASS'
} finally {
    if (Test-Path -LiteralPath $junction) { Remove-Item -LiteralPath $junction -Force }
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $reparseTarget -Recurse -Force -ErrorAction SilentlyContinue
}
