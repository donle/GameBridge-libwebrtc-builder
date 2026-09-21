param([Parameter(Mandatory)][string]$SourceRoot,[Parameter(Mandatory)][string]$Output)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$pins=Get-Content -Raw -LiteralPath "$PSScriptRoot/../pins.json" | ConvertFrom-Json
if ((git -C $SourceRoot rev-parse HEAD).Trim() -cne $pins.webrtc_revision) { throw 'Receiver test requires the exact pinned upstream revision' }
$source=Get-Content -Raw -LiteralPath (Join-Path $SourceRoot 'video/rtp_video_stream_receiver2.cc')
$start=$source.IndexOf('void RtpVideoStreamReceiver2::FrameConsumed(',[StringComparison]::Ordinal)
if($start -lt 0) {
    # The unpatched baseline has no acknowledgement path. Compile that actual
    # no-op behavior so RED demonstrates packet loss, not a missing-symbol error.
    $method='void RtpVideoStreamReceiver2::FrameConsumed(int64_t, uint64_t) {}'
} else {
    $brace=$source.IndexOf('{',$start)
    $end=$brace+1
    $depth=1
    while($end -lt $source.Length -and $depth -gt 0) {
        if($source[$end] -eq '{'){++$depth}
        if($source[$end] -eq '}'){--$depth}
        ++$end
    }
    if($depth){throw 'Unbalanced pinned receiver method'}
    $method=$source.Substring($start,$end-$start)
}
[IO.File]::WriteAllText([IO.Path]::GetFullPath($Output),$method+"`n",[Text.UTF8Encoding]::new($false))
