param([string]$BuildScript="$PSScriptRoot/../../../scripts/build-libwebrtc-ci.ps1")
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. "$PSScriptRoot/../../../scripts/rtc-ci-storage.ps1"
function Expect-Failure([scriptblock]$Action) {
    $failed=$false
    try { & $Action | Out-Null } catch { $failed=$true }
    if (!$failed) { throw 'Expected unsafe CI storage rejection' }
}
# Replay the actual first hosted run's measured bytes: no cache deletion is
# necessary, and the D: build workspace must be selected before cleanup.
$runVolumes=@(
    [pscustomobject]@{Name='C';Free=[long]34209615872},
    [pscustomobject]@{Name='D';Free=[long]157842391040},
    [pscustomobject]@{Name='Temp';Free=[long]34209615872}
)
$volume=Select-RtcCiBuildVolume $runVolumes (60GB)
if ($null -eq $volume -or $volume.Name -cne 'D') { throw 'Adequate hosted D: volume must skip cleanup' }
$volume=Select-RtcCiBuildVolume @([pscustomobject]@{Name='C';Free=200GB},[pscustomobject]@{Name='D';Free=60GB}) (60GB)
if ($volume.Name -cne 'D') { throw 'D: preference or exact minimum boundary failed' }
$volume=Select-RtcCiBuildVolume @([pscustomobject]@{Name='C';Free=61GB},[pscustomobject]@{Name='D';Free=59GB}) (60GB)
if ($volume.Name -cne 'C') { throw 'Eligible C: fallback should avoid unnecessary cleanup' }
if ($null -ne (Select-RtcCiBuildVolume @([pscustomobject]@{Name='C';Free=59GB},[pscustomobject]@{Name='Temp';Free=200GB}) (60GB))) { throw 'Insufficient or aliased storage must not be selected' }

$directory=[IO.FileAttributes]::Directory
# C:\vcpkg is an explicitly allowed, fully resolved directory with only eight
# characters. The former minimum-length heuristic incorrectly rejected it.
foreach ($target in Get-RtcCiCleanupTargets) {
    Assert-RtcCiCleanupTarget $target $target $directory
}
foreach($target in @('C:\','D:\','C:\Windows','C:\Program Files','C:\Program Files\Microsoft Visual Studio','C:\Program Files (x86)\Windows Kits','C:\vcpkg\..','C:\vcpkg-other','C:\vcpkg:stream','\\server\share','C:relative')) {
    Expect-Failure { Assert-RtcCiCleanupTarget $target $target $directory }
}
Expect-Failure { Assert-RtcCiCleanupTarget 'C:\vcpkg' 'C:\Windows' $directory }
Expect-Failure { Assert-RtcCiCleanupTarget 'C:\vcpkg' 'C:\vcpkg' ($directory -bor [IO.FileAttributes]::ReparsePoint) }
Expect-Failure { Assert-RtcCiCleanupTarget 'C:\vcpkg' 'C:\vcpkg' ([IO.FileAttributes]::Normal) }
# Exercise the actual script's storage block, isolated from installers/network.
# Any attempted cleanup/path access with sufficient D: space is a test failure.
# Filesystem doubles below never issue a real deletion or inspect a C: target.
$buildSource=Get-Content -Raw -LiteralPath $BuildScript
$start=$buildSource.IndexOf('$before=@(Disk-Snapshot)',[StringComparison]::Ordinal)
$end=$buildSource.IndexOf('$buildRoot=',[StringComparison]::Ordinal)
if($start -lt 0 -or $end -le $start){throw 'CI storage block boundaries not found'}
$storageBlock=[scriptblock]::Create($buildSource.Substring($start,$end-$start))
& {
    $pins=[pscustomobject]@{minimum_free_gib=60}
    function Disk-Snapshot { $runVolumes }
    function Test-Path { throw 'Unexpected cleanup path lookup despite adequate D: capacity' }
    function Get-Item { throw 'Unexpected cleanup inspection despite adequate D: capacity' }
    function Remove-Item { throw 'Unexpected deletion despite adequate D: capacity' }
    . $storageBlock
    if($volume.Name -cne 'D' -or !$cleanupSkipped -or $reclaimed.Count -ne 0){throw 'Actual build block did not skip all cleanup'}
}
& {
    $pins=[pscustomobject]@{minimum_free_gib=60}
    $state=[pscustomobject]@{Free=59GB;Removed=@()}
    function Disk-Snapshot { [pscustomobject]@{Name='C';Free=$state.Free;Used=0};[pscustomobject]@{Name='D';Free=59GB;Used=0} }
    function Test-Path { param($LiteralPath) return $LiteralPath -ceq 'C:\vcpkg' }
    function Get-Item { param($LiteralPath,[switch]$Force) return [pscustomobject]@{FullName=$LiteralPath;Attributes=[IO.FileAttributes]::Directory} }
    function Remove-Item {
        param($LiteralPath,[switch]$Recurse,[switch]$Force)
        if($LiteralPath -cne 'C:\vcpkg' -or !$Recurse -or !$Force){throw 'Unexpected mock cleanup request'}
        $state.Removed += $LiteralPath
        $state.Free=61GB
    }
    . $storageBlock
    if($volume.Name -cne 'C' -or $cleanupSkipped -or $state.Removed.Count -ne 1 -or $reclaimed.Count -ne 1){throw 'Bounded cleanup did not stop once minimum capacity was reached'}
}
Write-Host 'CI storage policy: recorded D: capacity, no-cleanup selection, minimum boundary, exact allowlist and reparse guards PASS'
