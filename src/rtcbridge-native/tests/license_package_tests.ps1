param([string]$BuildScript="$PSScriptRoot/../../../scripts/build-libwebrtc-ci.ps1")
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$source=Get-Content -Raw -LiteralPath $BuildScript
$match=[regex]::Match($source,'(?s)(?<guard>foreach\(\$name in @\(''LICENSE.md'',''THIRD_PARTY_LICENSES.md'',''PATENTS.txt''\)\).*?)\$archive=')
if(!$match.Success){throw 'Actual required-license packaging guard missing'}
$guard=[scriptblock]::Create($match.Groups['guard'].Value)
$repository=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$payload=Join-Path $repository ('out/license-package-test-'+[guid]::NewGuid().ToString('N'))
function Reject-Package {
    $rejected=$false
    try{& $guard}catch{$rejected=$true}
    if(!$rejected){throw 'Unsafe license package accepted'}
}
try {
    New-Item -ItemType Directory -Path $payload | Out-Null
    [IO.File]::WriteAllText((Join-Path $payload 'LICENSE.md'),'Complete fixture licenses')
    [IO.File]::WriteAllText((Join-Path $payload 'PATENTS.txt'),'Fixture patent notice')
    Reject-Package
    [IO.File]::WriteAllText((Join-Path $payload 'THIRD_PARTY_LICENSES.md'),'')
    Reject-Package
    [IO.File]::WriteAllText((Join-Path $payload 'THIRD_PARTY_LICENSES.md'),'Truncated fixture')
    Reject-Package
    Copy-Item -LiteralPath (Join-Path $payload 'LICENSE.md') -Destination (Join-Path $payload 'THIRD_PARTY_LICENSES.md') -Force
    & $guard
} finally {
    $parent=[IO.Path]::GetFullPath((Join-Path $repository 'out'))+'\'
    $resolved=[IO.Path]::GetFullPath($payload)
    if(!$resolved.StartsWith($parent,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolved) -notmatch '^license-package-test-[0-9a-f]{32}$'){throw 'Unsafe license test cleanup target'}
    if(Test-Path -LiteralPath $resolved){Remove-Item -LiteralPath $resolved -Recurse -Force}
}
Write-Host 'License package guard: required nonempty files and exact complete-license alias PASS'
