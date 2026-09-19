param([string]$BuildScript="$PSScriptRoot/../../../scripts/build-libwebrtc-ci.ps1")
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$helper="$PSScriptRoot/../../../scripts/rtc-ci-git.ps1"
if(Test-Path -LiteralPath $helper){. $helper}

# Run the real build script's Git selection assignment, not an imitation.
$source=Get-Content -Raw -LiteralPath $BuildScript
$assignment=[regex]::Match($source,'(?m)^\$gitExe=.*$').Value
if(!$assignment){throw 'Build Git selection assignment not found'}
$selectGit=[scriptblock]::Create($assignment)
$existing=@(Get-Command git.exe -CommandType Application -All -ErrorAction Stop)[0].Source
$install=[IO.DirectoryInfo]([IO.Path]::GetDirectoryName($existing))
while($null -ne $install -and $install.Name -ine 'Git'){$install=$install.Parent}
if($null -eq $install){throw 'Test requires the existing Git for Windows installation'}
$expected=Join-Path $install.FullName 'cmd/git.exe'
$bin=Join-Path $install.FullName 'bin'
$cmd=Join-Path $install.FullName 'cmd'
if(!(Test-Path -LiteralPath (Join-Path $bin 'git.exe'))){throw 'Git bin launcher required for regression'}
$savedPath=$env:Path
try {
    $env:Path="$bin;$cmd;$savedPath"
    if(@(Get-Command git.exe -CommandType Application -All).Count -lt 2){throw 'Multiple real application matches required for regression'}
    . $selectGit
    if($gitExe -isnot [string] -or $gitExe -ine $expected){throw 'Bin-first/multiple-match selection must return one canonical Git cmd executable'}
    $env:Path="$cmd;$bin;$savedPath"
    . $selectGit
    if($gitExe -isnot [string] -or $gitExe -ine $expected){throw 'Cmd-first selection must return the same canonical executable'}
} finally {$env:Path=$savedPath}
$applications=@(Get-Command git.exe -CommandType Application -All)
$notApplication=[pscustomobject]@{CommandType=[Management.Automation.CommandTypes]::Function;Source=$expected}
$missingApplication=[pscustomobject]@{CommandType=[Management.Automation.CommandTypes]::Application;Source=(Join-Path $install.FullName 'missing/git.exe')}
$selected=Resolve-RtcCiGitExecutable (@($notApplication,$missingApplication)+$applications)
if($selected -isnot [string] -or $selected -ine $expected){throw 'Selection must skip invalid matches and choose the first valid application'}
$rejected=$false
try{Resolve-RtcCiGitExecutable @($notApplication,$missingApplication) | Out-Null}catch{$rejected=$true}
if(!$rejected){throw 'All-invalid Git application candidates must fail closed'}
Write-Host 'CI Git selection: real bin-first, cmd-first and multiple application matches PASS'
