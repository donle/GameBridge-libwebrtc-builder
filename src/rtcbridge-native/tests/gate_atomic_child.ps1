param([string]$Writer,[string]$Output,[string]$ReadyEvent,[ValidateSet('BeforeFlush','BeforeReplace')][string]$Phase)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. $Writer
$ready=[Threading.EventWaitHandle]::OpenExisting($ReadyEvent)
$lines=Get-Content -LiteralPath $Writer
$pattern=if($Phase -ceq 'BeforeFlush'){'^\s*\$stream\.Flush\(\$true\)'}else{'^\s*\[IO\.File\]::Replace\('}
$boundaryLines=@(for($index=0;$index -lt $lines.Count;$index++){if($lines[$index] -match $pattern){$index+1}})
if($boundaryLines.Count -ne 1){throw 'Actual atomic publication boundary not found'}
# A debugger line breakpoint pauses the exact production writer: no alternate
# write implementation, mocked file operation or production test hook is used.
$null=Set-PSBreakpoint -Script $Writer -Line $boundaryLines[0] -Action {
    $null=$ready.Set()
    $pause=[Threading.ManualResetEvent]::new($false)
    $null=$pause.WaitOne(30000)
    throw 'Atomic test child must be terminated before resuming publication'
}
$report=ConvertTo-NativeRtcDiagnostic ([pscustomobject]@{passed=$true;frames_delivered=300;sdp='PRIVATE_SENTINEL'}) 5 'complete'
Write-NativeRtcDiagnostic $report $Output
