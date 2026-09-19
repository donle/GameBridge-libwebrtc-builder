$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$repository=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$writer=Join-Path $repository 'scripts/validate-rtc-native-gate.ps1'
. $writer
$scratch=Join-Path $repository ('out/native-gate-atomic-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
$child=$null
function Assert-PreviousReport([string]$Expected) {
    $text=[IO.File]::ReadAllText($result)
    if($text -cne $Expected -or ($text | ConvertFrom-Json).passed -ne $false){throw 'Interrupted publication changed the previous valid failed report'}
    if($text.Contains('PRIVATE_SENTINEL')){throw 'Raw data entered the published report'}
}
try {
    $result=Join-Path $scratch 'report.json'
    $first=ConvertTo-NativeRtcDiagnostic ([pscustomobject]@{passed=$false;frames_delivered=291}) 5 'validation'
    $second=ConvertTo-NativeRtcDiagnostic ([pscustomobject]@{passed=$false;frames_delivered=292;sdp='PRIVATE_SENTINEL'}) 5 'validation'
    Write-NativeRtcDiagnostic $first $result 6>$null
    $previous=[IO.File]::ReadAllText($result)
    # Existing readers may allow rename/delete but forbid in-place writes. A
    # real atomic replacement must publish and preserve their old file handle.
    $reader=[IO.FileStream]::new($result,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::Read -bor [IO.FileShare]::Delete))
    try {
        try { Write-NativeRtcDiagnostic $second $result 6>$null }
        catch { throw 'Report publication still overwrites the open destination instead of replacing it' }
        $oldReader=[IO.StreamReader]::new($reader)
        try { if($oldReader.ReadToEnd() -cne $previous){throw 'Atomic publication changed an existing reader snapshot'} }
        finally { $oldReader.Dispose() }
    } finally { $reader.Dispose() }
    if((Get-Content -Raw -LiteralPath $result | ConvertFrom-Json).frames_delivered -ne 292){throw 'Complete replacement was not published'}
    $previous=[IO.File]::ReadAllText($result)

    # Actual replacement failure: permit writes but deny delete/rename. No
    # delete-then-move or in-place fallback may damage the published report.
    $reader=[IO.FileStream]::new($result,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    try {
        $rejected=$false
        try { Write-NativeRtcDiagnostic $first $result 6>$null } catch { $rejected=$true }
        if(!$rejected){throw 'A blocked atomic replacement was silently bypassed'}
        Assert-PreviousReport $previous
    } finally { $reader.Dispose() }

    # Exercise the real serializer's failure before touching any destination.
    $unsupported=[Collections.Generic.Dictionary[int,string]]::new()
    $unsupported.Add(1,'PRIVATE_SENTINEL')
    $rejected=$false
    try { Write-NativeRtcDiagnostic ([pscustomobject]@{passed=$false;unsupported=$unsupported}) $result 6>$null } catch { $rejected=$true }
    if(!$rejected){throw 'Expected real JSON serialization failure'}
    Assert-PreviousReport $previous
    if(@(Get-ChildItem -LiteralPath $scratch -File -Filter '.rtc-report-*.tmp').Count){throw 'Handled publication failure left a temporary file'}

    foreach($phase in @('BeforeFlush','BeforeReplace')) {
        $priorTemporaryNames=@(Get-ChildItem -LiteralPath $scratch -File -Filter '.rtc-report-*.tmp' | Select-Object -ExpandProperty Name)
        $eventName='Local\GameBridgeAtomicReport'+[guid]::NewGuid().ToString('N')
        $ready=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$eventName)
        try {
            $shell=[Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
            $arguments=@('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',('"'+(Join-Path $PSScriptRoot 'gate_atomic_child.ps1')+'"'),'-Writer',('"'+$writer+'"'),'-Output',('"'+$result+'"'),'-ReadyEvent',$eventName,'-Phase',$phase)
            $child=Start-Process -FilePath $shell -ArgumentList $arguments -WindowStyle Hidden -PassThru
            if(!$ready.WaitOne(10000)){throw "Atomic writer did not reach $phase"}
            Assert-PreviousReport $previous
            $pending=@(Get-ChildItem -LiteralPath $scratch -File -Filter '.rtc-report-*.tmp' | Where-Object { $_.Name -notin $priorTemporaryNames })
            if($pending.Count -ne 1){throw 'Writer did not stage exactly one same-directory temporary file'}
            if($phase -ceq 'BeforeReplace'){
                $exclusive=[IO.FileStream]::new($pending[0].FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None)
                $stagedReader=[IO.StreamReader]::new($exclusive)
                try {
                    $staged=$stagedReader.ReadToEnd()
                    if(($staged | ConvertFrom-Json).frames_delivered -ne 300 -or $staged.Contains('PRIVATE_SENTINEL')){throw 'Staged report is incomplete or unsanitized'}
                } finally {$stagedReader.Dispose();$exclusive.Dispose()}
            }else{
                $locked=$false
                try { $exclusive=[IO.FileStream]::new($pending[0].FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None);$exclusive.Dispose() } catch {$locked=$true}
                if(!$locked){throw 'Pre-flush cancellation did not interrupt an active temp write'}
            }
            # Simulate cancellation/termination while a fully isolated temp
            # write is pending; deliberately do not let finally run in child.
            Stop-Process -InputObject $child -Force
            if(!$child.WaitForExit(5000)){throw 'Atomic writer child did not terminate'}
            $child.Dispose();$child=$null
            Assert-PreviousReport $previous
        } finally {
            if($null -ne $child){if(!$child.HasExited){Stop-Process -InputObject $child -Force};$child.Dispose();$child=$null}
            $ready.Dispose()
        }
    }
    $launcher=Get-Content -Raw -LiteralPath (Join-Path $repository 'scripts/test-rtc-native-gate.ps1')
    if($launcher -match 'Set-Content -LiteralPath \$resultPath' -or $launcher -notmatch 'Write-NativeRtcDiagnostic \$initial \$resultPath'){throw 'Initial report bypasses atomic publication'}
    Write-Host 'Native report atomicity: real reader snapshots, replace failure, serialization failure and killed pre-flush/pre-replace writers PASS'
} finally {
    if($null -ne $child){if(!$child.HasExited){Stop-Process -InputObject $child -Force};$child.Dispose()}
    $parent=[IO.Path]::GetFullPath((Join-Path $repository 'out'))+'\'
    $resolved=[IO.Path]::GetFullPath($scratch)
    if(!$resolved.StartsWith($parent,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolved) -notmatch '^native-gate-atomic-test-[0-9a-f]{32}$'){throw 'Unsafe atomic-test cleanup'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
