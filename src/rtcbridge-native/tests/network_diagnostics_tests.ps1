$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. "$PSScriptRoot/../../../scripts/validate-rtc-native-gate.ps1"
$evidence=@{network_evidence_schema=2;producer_evidence_schema=1;fatal_other_errors=1;fatal_producer_errors=1;fatal_callback_other_errors=0;frames_submitted=2;submission_failures=1}
foreach($source in @('video','audio','data')){
  foreach($result in @('attempts','ok','backpressure','state','invalid','closed','other')){$evidence["producer_${source}_$result"]=0}
}
$evidence.producer_video_attempts=3;$evidence.producer_video_ok=2;$evidence.producer_video_state=1
$reasons=@('missing_selected_pair','missing_pair_record','missing_candidate_record','missing_candidate_type','pair_not_succeeded','relay','timeout','other')
foreach($peer in @('sender','receiver')){
  $evidence["native_${peer}_proof_samples"]=6;$evidence["native_${peer}_proof_succeeded_samples"]=3
  $evidence["native_${peer}_proof_retained_routine_probes"]=2;$evidence["native_${peer}_proof_unproven_samples"]=1
  $evidence["native_${peer}_proof_initial"]=1;$evidence["native_${peer}_proof_regressions"]=1
  $evidence["native_${peer}_proof_recoveries"]=1;$evidence["native_${peer}_proof_terminal"]=1;$evidence["native_${peer}_proof_transitions"]=4
  foreach($reason in $reasons){$evidence["native_${peer}_regression_$reason"]=0;$evidence["native_${peer}_terminal_$reason"]=0}
  $evidence["native_${peer}_regression_missing_candidate_record"]=1;$evidence["native_${peer}_terminal_other"]=1
}
$safe=ConvertTo-NativeRtcDiagnostic ([pscustomobject]$evidence) 5 'consumer'
foreach($field in $evidence.Keys){if($null-eq $safe.PSObject.Properties[$field]){throw "Numeric network evidence was dropped: $field"}}
if(!(Get-Command Test-NativeRtcEvidence -ErrorAction SilentlyContinue)){throw 'Missing numeric evidence partition validator'}
Test-NativeRtcEvidence $safe
$withoutInitial=$evidence.Clone();$withoutInitial.native_sender_proof_initial=0;$withoutInitial.native_sender_proof_transitions=3
$rejected=$false;try{Test-NativeRtcEvidence ([pscustomobject]$withoutInitial)}catch{$rejected=$true}
if(!$rejected){throw 'Retained routine probes accepted without initial proof'}
foreach($field in @('native_sender_proof_retained_routine_probes','native_receiver_proof_samples')){
  $missing=$evidence.Clone();$missing.Remove($field)
  $rejected=$false;try{Test-NativeRtcEvidence ([pscustomobject]$missing)}catch{$rejected=$true}
  if(!$rejected){throw 'Schema 2 silently accepted missing routine-probe evidence'}
}
foreach($field in @('producer_video_attempts','producer_video_state','fatal_producer_errors','native_sender_proof_transitions','native_receiver_regression_missing_candidate_record','native_sender_terminal_other','native_sender_proof_samples','native_sender_proof_succeeded_samples','native_sender_proof_retained_routine_probes','native_receiver_proof_unproven_samples')){
  $broken=$evidence.Clone();$broken[$field]++
  $rejected=$false;try{Test-NativeRtcEvidence ([pscustomobject]$broken)}catch{$rejected=$true}
  if(!$rejected){throw "Inconsistent numeric partition accepted: $field"}
}
foreach($invalid in @('PRIVATE_SENTINEL',-1,0.5,[double]::NaN,9007199254740992)){
 foreach($counter in @('producer_video_state','native_sender_proof_retained_routine_probes')){
  $broken=$evidence.Clone();$broken[$counter]=$invalid
  $rejected=$false;try{Test-NativeRtcEvidence ([pscustomobject]$broken)}catch{$rejected=$true}
  if(!$rejected){throw 'Unsafe/noninteger/unbounded evidence accepted'}
  $projected=ConvertTo-NativeRtcDiagnostic ([pscustomobject]$broken) 5 'consumer'
  if($null-ne $projected.$counter){throw 'Rejected counter escaped the frozen integer evidence schema'}
  if(($projected|ConvertTo-Json -Depth 10).Contains('PRIVATE_SENTINEL')){throw 'Opaque evidence leaked'}
 }
}
$legacy=$evidence.Clone();$legacy.network_evidence_schema=1
foreach($peer in @('sender','receiver')){foreach($suffix in @('samples','succeeded_samples','retained_routine_probes','unproven_samples')){$legacy.Remove("native_${peer}_proof_$suffix")}}
Test-NativeRtcEvidence ([pscustomobject]$legacy)
Write-Host 'Network evidence schemas 1/2: numeric projection, retained-probe samples, producer/fatal/proof/reason partitions, integer bounds and privacy PASS'
