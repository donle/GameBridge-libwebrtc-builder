param([Parameter(Mandatory)][string]$SourceRoot,[Parameter(Mandatory)][string]$Output,
    [string]$BridgeSource)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(!$BridgeSource){$BridgeSource=Join-Path $PSScriptRoot '../src/bridge.cpp'}
function Get-CanonicalSourceHash([string]$Path){
    # Git for Windows may check text out as CRLF. Pin content, not that
    # checkout-only newline conversion; all other changes remain rejected.
    $bytes=[Text.Encoding]::UTF8.GetBytes([IO.File]::ReadAllText($Path).Replace("`r`n","`n"))
    $hash=[Security.Cryptography.SHA256]::Create()
    try {([BitConverter]::ToString($hash.ComputeHash($bytes))).Replace('-','').ToLowerInvariant()}
    finally {$hash.Dispose()}
}
$pins=Get-Content -Raw -LiteralPath "$PSScriptRoot/../pins.json" | ConvertFrom-Json
if((git -C $SourceRoot rev-parse HEAD).Trim() -cne $pins.webrtc_revision){throw 'Sender test requires exact pinned upstream'}
$factory=Join-Path $SourceRoot 'video/config/encoder_stream_factory.cc'
if((Get-CanonicalSourceHash $factory) -cne '00e440e7229f1be8b1d1b5fc14985dcff75d29b1dc0182af5a98a4b74d8e854c'){throw 'Pinned encoder stream factory differs'}
foreach($entry in @(
    @('modules/pacing/pacing_controller.cc','c08ac163e1b89cc304c3d7fce7d64de6c712e3158be645cff16b98a99b86f285'),
    @('video/encoder_bitrate_adjuster.cc','53fa8fd0853a88af55f56d21653457a947d141d078f14997e3b31e0b48297cd2'),
    @('modules/congestion_controller/goog_cc/goog_cc_network_control.cc','3bbffda70a7a9b0bb3923a34ca32d6387c4c56b3d5dd0462412f4fd4e27f5d61'))){
    if((Get-CanonicalSourceHash (Join-Path $SourceRoot $entry[0])) -cne $entry[1]){throw 'Pinned rate-control source differs'}
}
$source=Get-Content -Raw -LiteralPath $factory
$default=[regex]::Match($source,'(?s)int GetMaxDefaultVideoBitrateKbps\(.*?\n\}')
$limits=[regex]::Match($source,'(?s)  const bool encoding_max_bitrate_configured =.*?(?=  VideoStream layer;)')
if(!$default.Success -or !$limits.Success){throw 'Pinned singlecast limit code not found'}
$bridge=Get-Content -Raw -LiteralPath $BridgeSource
$transport=[regex]::Match($bridge,'(?s)    w::BitrateSettings bitrate;.*?(?=    if \(!pc_->SetBitrate)')
if(!$transport.Success){throw 'Actual transport bitrate configuration not found'}
$init=[regex]::Match($bridge,'(?s)      w::RtpTransceiverInit init;.*?(?=      auto transceiver_result = pc_->AddTransceiver)')
if($init.Success){
    if($bridge -notmatch 'pc_->AddTransceiver\(media, init\)'){throw 'Configured encodings are not passed to AddTransceiver'}
    $initialization=$init.Value
}else{
    # Legacy AddTransceiver(media) has no initial encodings. This makes the
    # regression execute the old defaults rather than merely fail to compile.
    if($bridge -notmatch 'pc_->AddTransceiver\(media\)'){throw 'Unrecognized transceiver initialization'}
    $initialization='w::RtpTransceiverInit init;'
}
$generated=@"
// Exact pinned upstream default/limit algorithm; surrounding value types only
// are isolated. The bridge configuration statements are copied verbatim.
$($default.Value)
Limits PinnedSinglecast(const Config& encoder_config, int width, int height) {
  bool is_screencast = false;
  std::optional<DataRate> experimental_min_bitrate;
$($limits.Value)
  return {min_bitrate_bps, max_bitrate_bps, max_framerate};
}
w::BitrateSettings ActualTransportSettings() {
$($transport.Value)
  return bitrate;
}
w::RtpTransceiverInit ActualMediaInit(unsigned kind) {
  (void)kind;
$initialization
  return init;
}
"@
[IO.File]::WriteAllText([IO.Path]::GetFullPath($Output),$generated,[Text.UTF8Encoding]::new($false))
