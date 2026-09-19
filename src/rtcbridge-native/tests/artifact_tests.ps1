$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. "$PSScriptRoot/../../../scripts/rtc-artifact-security.ps1"
function Expect-Failure($Action) {
    $failed=$false;try{& $Action}catch{$failed=$true}
    if(!$failed){throw 'Expected artifact rejection'}
}
Expect-Failure { Resolve-RtcDPath 'C:\test' }
Expect-Failure { Resolve-RtcDPath '\\server\share\file' }
Expect-Failure { Resolve-RtcDPath 'D:relative' }
Expect-Failure { Resolve-RtcDPath 'D:\projects\GameBridge\..\..\..\C:\test' }
$root=Join-Path ([IO.Path]::GetFullPath("$PSScriptRoot/../../../out")) ('rtc-artifact-tests-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
Add-Type -AssemblyName System.IO.Compression
function New-TestZip($Path,$Entry) {
    $file=[IO.File]::Open($Path,[IO.FileMode]::CreateNew)
    try {
        $zip=[IO.Compression.ZipArchive]::new($file,[IO.Compression.ZipArchiveMode]::Create,$true)
        try {
            $item=$zip.CreateEntry($Entry)
            $stream=$item.Open();try{$data=[Text.Encoding]::UTF8.GetBytes('test');$stream.Write($data,0,$data.Length)}finally{$stream.Dispose()}
        } finally {$zip.Dispose()}
    } finally {$file.Dispose()}
}
try {
    $bad=Join-Path $root 'bad.zip';New-TestZip $bad '../outside.dll'
    Expect-Failure { Test-RtcZip $bad @('gamebridge_rtc.dll') }
    $good=Join-Path $root 'good.zip';New-TestZip $good 'gamebridge_rtc.dll'
    Test-RtcZip $good @('gamebridge_rtc.dll')
    # Central-directory Length is attacker controlled. .NET's inflater can
    # emit all four bytes even when that field advertises only one byte.
    $forged=Join-Path $root 'forged-length.zip'
    $zipBytes=[IO.File]::ReadAllBytes($good)
    $central=-1
    for($i=0;$i -le $zipBytes.Length-4;++$i) {
        if([BitConverter]::ToUInt32($zipBytes,$i) -eq 0x02014b50){$central=$i;break}
    }
    if($central -lt 0){throw 'Test ZIP central directory not found'}
    [Array]::Copy([BitConverter]::GetBytes([uint32]1),0,$zipBytes,$central+24,4)
    [IO.File]::WriteAllBytes($forged,$zipBytes)
    Expect-Failure { Test-RtcZip $forged @('gamebridge_rtc.dll') }
    Expect-Failure { Copy-RtcZipEntry $forged 'gamebridge_rtc.dll' (Join-Path $root 'forged.dll') }
    $longDeclared=Join-Path $root 'overstated-length.zip'
    [Array]::Copy([BitConverter]::GetBytes([uint32]8),0,$zipBytes,$central+24,4)
    [IO.File]::WriteAllBytes($longDeclared,$zipBytes)
    Expect-Failure { Test-RtcZip $longDeclared @('gamebridge_rtc.dll') }
    Expect-Failure { Test-RtcZip $good @('gamebridge_rtc.dll') -MaximumExpandedBytes 3 }
    Expect-Failure { Test-RtcZip $good @('gamebridge_rtc.dll') -MaximumEntryBytes 3 }
    [long]$expanded=0
    Copy-RtcZipEntry $good 'gamebridge_rtc.dll' (Join-Path $root 'first.dll') -TotalBytes ([ref]$expanded) -MaximumExpandedBytes 7
    if([IO.File]::ReadAllText((Join-Path $root 'first.dll')) -cne 'test'){throw 'Extraction corrupted payload'}
    Expect-Failure { Copy-RtcZipEntry $good 'gamebridge_rtc.dll' (Join-Path $root 'second.dll') -TotalBytes ([ref]$expanded) -MaximumExpandedBytes 7 }
    if($expanded -gt 7){throw 'Extraction exceeded actual aggregate byte budget'}
    $child=(Get-Process -Id $PID).Path
    $encoded=[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes('$ProgressPreference="SilentlyContinue";[Console]::OpenStandardOutput().Write([byte[]]::new(8192),0,8192)'))
    $download=Join-Path $root 'download.bin'
    Expect-Failure { Save-RtcBoundedProcessOutput $child "-NoProfile -NonInteractive -EncodedCommand $encoded" $download 1024 }
    if(!(Test-Path -LiteralPath $download) -or (Get-Item -LiteralPath $download).Length -gt 1024){throw 'Outer download was not byte bounded'}
    $complete=Join-Path $root 'complete.bin'
    Save-RtcBoundedProcessOutput $child "-NoProfile -NonInteractive -EncodedCommand $encoded" $complete 8192
    if((Get-Item -LiteralPath $complete).Length -ne 8192){throw 'Bounded binary download corrupted output'}
    # This tests policy on gh's verified JSON, not the cryptographic verifier.
    $invocation='https://github.com/donle/GameBridge-libwebrtc-builder/actions/runs/123/attempts/2'
    $provenance=[pscustomobject]@{verificationResult=[pscustomobject]@{
        statement=[pscustomobject]@{predicateType='https://slsa.dev/provenance/v1';predicate=[pscustomobject]@{runDetails=[pscustomobject]@{metadata=[pscustomobject]@{invocationId=$invocation}}}}
        signature=[pscustomobject]@{certificate=[pscustomobject]@{runInvocationURI=$invocation}}
    }}
    Assert-RtcProvenanceRun @($provenance) 'donle/GameBridge-libwebrtc-builder' 123 2
    Expect-Failure { Assert-RtcProvenanceRun @($provenance) 'donle/GameBridge-libwebrtc-builder' 124 2 }
    Expect-Failure { Assert-RtcProvenanceRun @($provenance) 'donle/GameBridge-libwebrtc-builder' 123 1 }
    $provenance.verificationResult.signature.certificate.runInvocationURI=$invocation.Replace('/attempts/2','/attempts/1')
    Expect-Failure { Assert-RtcProvenanceRun @($provenance) 'donle/GameBridge-libwebrtc-builder' 123 2 }
    Expect-Failure { Assert-RtcProvenanceRun @([pscustomobject]@{untrusted='nonempty'}) 'donle/GameBridge-libwebrtc-builder' 123 2 }
    Expect-Failure { Assert-RtcSha256 $good ('0'*64) }
    Assert-RtcSha256 $good (Get-FileHash -Algorithm SHA256 -LiteralPath $good).Hash
    $unexpected=Join-Path $root 'unexpected.zip';New-TestZip $unexpected 'unreviewed.exe'
    Expect-Failure { Test-RtcZip $unexpected @('gamebridge_rtc.dll') }
    Write-Host 'artifact guards: D-root, traversal, allowlist, SHA-256, actual-byte limits, run/attempt provenance PASS'
} finally {
    $checked=Resolve-RtcDPath $root
    if($checked.StartsWith([IO.Path]::GetFullPath("$PSScriptRoot/../../../out/rtc-artifact-tests-"),[StringComparison]::OrdinalIgnoreCase)) { Remove-Item -LiteralPath $checked -Recurse -Force }
}
