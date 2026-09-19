param([Parameter(Mandatory)][string]$SourceRoot,[Parameter(Mandatory)][string]$Output)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$pins=Get-Content -Raw -LiteralPath "$PSScriptRoot/../pins.json" | ConvertFrom-Json
if ((git -C $SourceRoot rev-parse HEAD).Trim() -cne $pins.webrtc_revision) { throw 'Cold-start test requires the exact pinned upstream revision' }
$source=Get-Content -Raw -LiteralPath (Join-Path $SourceRoot 'pc/encoded_video_frame_injector.cc')
$methods=@()
foreach($name in @('InjectFrame','RegisterEncoder')) {
    $start=$source.IndexOf("void EncodedVideoFrameInjector::$name(",[StringComparison]::Ordinal)
    if($start -lt 0){throw 'Pinned injector method not found'}
    $brace=$source.IndexOf('{',$start)
    $end=$brace+1
    $depth=1
    while($end -lt $source.Length -and $depth -gt 0) {
        if($source[$end] -eq '{'){++$depth}
        if($source[$end] -eq '}'){--$depth}
        ++$end
    }
    if($depth){throw 'Unbalanced pinned injector method'}
    $methods += $source.Substring($start,$end-$start)
}
[IO.File]::WriteAllText([IO.Path]::GetFullPath($Output),($methods -join "`n"),[Text.UTF8Encoding]::new($false))
