param(
    [string]$BuildScript="$PSScriptRoot/../../../scripts/build-libwebrtc-ci.ps1",
    [string]$DepotRoot=''
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. "$PSScriptRoot/../../../scripts/rtc-ci-git.ps1"
$repository=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$source=Get-Content -Raw -LiteralPath $BuildScript
$call=[regex]::Match($source,'(?m)^& "\$depot/vpython3.bat".*depot_git_tests.py.*$').Value
if(!$call){throw 'Actual depot cache preflight invocation not found'}
# The test script lives outside depot_tools. Unlike gclient.py, it cannot
# inherit the pinned depot/vpython.toml by walking its own parent directories.
if($call -notmatch 'vpython3\.bat" -vpython-spec "\$depot/vpython\.toml" -B '){
    throw 'External preflight must explicitly bind the pinned depot_tools vpython.toml'
}
$setup=[regex]::Match($source,'(?m)^Initialize-RtcCiGitConfig -BuildRoot \$buildRoot\s*$').Value
if(!$setup -or $source.IndexOf($setup) -gt $source.IndexOf('cmd /d /c "$depot\bootstrap\win_tools.bat"')){
    throw 'Isolated Git configuration must be initialized before official bootstrap'
}
$buildRoot=Join-Path $repository ('out/depot-environment-test-'+[guid]::NewGuid().ToString('N'))
$savedGlobal=$env:GIT_CONFIG_GLOBAL
$savedPath=$env:Path
try {
    New-Item -ItemType Directory -Path $buildRoot | Out-Null
    $sentinel=Join-Path $buildRoot 'prior-global.gitconfig'
    $sentinelText="[user]`n`tname = Leave unchanged`n"
    [IO.File]::WriteAllText($sentinel,$sentinelText,[Text.UTF8Encoding]::new($false))
    $env:GIT_CONFIG_GLOBAL=$sentinel
    & ([scriptblock]::Create($setup))
    if($env:GIT_CONFIG_GLOBAL -eq $sentinel -or !$env:GIT_CONFIG_GLOBAL.StartsWith($buildRoot+'\',[StringComparison]::OrdinalIgnoreCase)){
        throw 'Bootstrap Git configuration did not move to the isolated build directory'
    }
    if([IO.File]::ReadAllText($sentinel) -cne $sentinelText){throw 'Existing global config was modified'}
    $gitExe=Resolve-RtcCiGitExecutable @(Get-Command git.exe -CommandType Application -All -ErrorAction Stop)
    $allow=(& $gitExe config --global --get depot-tools.allowGlobalGitConfig | Out-String).Trim()
    if($LASTEXITCODE -ne 0 -or $allow -cne 'false'){throw 'Real Git did not read the isolated no-global-change setting'}
    & $gitExe config --list --global | Out-Null
    if($LASTEXITCODE -ne 0){throw 'Isolated global config must exist before bootstrap reads it'}
    if($DepotRoot){
        $depot=(Resolve-Path -LiteralPath $DepotRoot).Path
        $env:Path="$depot;$([IO.Path]::GetDirectoryName($gitExe));$savedPath"
        # Optional integration mode executes the exact production invocation
        # against an already prepared pinned depot checkout, not a mock runner.
        & ([scriptblock]::Create($call))
        if($LASTEXITCODE -ne 0){throw 'Actual pinned vpython/cache preflight failed'}
        $configBefore=(Get-FileHash -Algorithm SHA256 -LiteralPath $env:GIT_CONFIG_GLOBAL).Hash
        $bootstrapOutput=@(& "$depot/vpython3.bat" -vpython-spec "$depot/vpython.toml" -B -c 'import sys; sys.path.insert(0, sys.argv[1]); from bootstrap import bootstrap; bootstrap._win_git_bootstrap_config()' $depot 2>&1)
        if($LASTEXITCODE -ne 0 -or $bootstrapOutput.Count -ne 0){throw 'Pinned bootstrap config check must be silent and successful'}
        if((Get-FileHash -Algorithm SHA256 -LiteralPath $env:GIT_CONFIG_GLOBAL).Hash -cne $configBefore){throw 'Pinned bootstrap changed the isolated no-write config'}
    }
} finally {
    $env:GIT_CONFIG_GLOBAL=$savedGlobal
    $env:Path=$savedPath
    $testParent=[IO.Path]::GetFullPath((Join-Path $repository 'out'))+'\'
    $resolved=[IO.Path]::GetFullPath($buildRoot)
    if(!$resolved.StartsWith($testParent,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolved) -notmatch '^depot-environment-test-[0-9a-f]{32}$'){
        throw 'Unsafe test cleanup target'
    }
    if(Test-Path -LiteralPath $resolved){Remove-Item -LiteralPath $resolved -Recurse -Force}
}
Write-Host 'CI depot environment: explicit pinned Python spec and isolated Git configuration PASS'
