# Read-only policy helpers. Merely loading this file never changes the host.
function Select-RtcCiBuildVolume {
    param([object[]]$Volumes,[long]$MinimumFreeBytes)
    if ($MinimumFreeBytes -lt 1) { throw 'Invalid CI free-space requirement' }
    # D is the hosted data/workspace disk. Use it before C even when C is larger.
    foreach ($name in @('D','C')) {
        $volumeMatches=@($Volumes | Where-Object { $_.Name -ceq $name })
        if ($volumeMatches.Count -gt 1) { throw 'Ambiguous CI filesystem volume' }
        if ($volumeMatches.Count -eq 1 -and [long]$volumeMatches[0].Free -ge $MinimumFreeBytes) { return $volumeMatches[0] }
    }
    return $null
}
function Get-RtcCiCleanupTargets {
    # Only irrelevant toolchain caches on the ephemeral hosted VM. Never add
    # VS, Windows Kits, Git, Node, gh, a drive root or a workspace here.
    @('C:\hostedtoolcache','C:\Android','C:\Program Files\Android','C:\Program Files\Java','C:\Program Files\Unity','C:\Program Files\Unity Hub','C:\SeleniumWebDrivers','C:\vcpkg')
}
function Assert-RtcCiCleanupTarget {
    param([string]$Path,[string]$ResolvedPath,[IO.FileAttributes]$Attributes)
    # Exact fixed identity is the security boundary, not an arbitrary minimum
    # path length (the explicitly allowed C:\vcpkg has eight characters).
    if ((Get-RtcCiCleanupTargets) -cnotcontains $Path -or $ResolvedPath -ine $Path -or
        [IO.Path]::GetFullPath($ResolvedPath) -ine $Path -or
        $ResolvedPath -like '*Visual Studio*' -or $ResolvedPath -like '*Windows Kits*') {
        throw "Unsafe ephemeral cleanup target: requested '$Path', resolved '$ResolvedPath'"
    }
    if (($Attributes -band [IO.FileAttributes]::Directory) -eq 0 -or
        ($Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Refusing ephemeral cleanup of a non-directory or reparse point: $ResolvedPath"
    }
}
