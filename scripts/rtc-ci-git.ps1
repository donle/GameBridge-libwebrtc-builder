function Resolve-RtcCiGitExecutable {
    param([object[]]$Applications)
    # Preserve Get-Command -All/PATH order, but return one scalar application.
    # The pinned bootstrap walks to the nearest directory named Git and binds
    # that installation's cmd/git.exe even if PATH originally matched bin/.
    foreach($application in $Applications) {
        if($application.CommandType -ne [Management.Automation.CommandTypes]::Application){continue}
        $candidate=[string]$application.Source
        if($candidate -notmatch '^[A-Za-z]:[\\/]' -or [IO.Path]::GetFileName($candidate) -ine 'git.exe'){continue}
        if(!(Test-Path -LiteralPath $candidate -PathType Leaf)){continue}
        $directory=[IO.DirectoryInfo]([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($candidate)))
        while($null -ne $directory -and $directory.Name -ine 'Git'){$directory=$directory.Parent}
        if($null -eq $directory){continue}
        $canonical=Join-Path $directory.FullName 'cmd/git.exe'
        if(Test-Path -LiteralPath $canonical -PathType Leaf) {
            return (Get-Item -LiteralPath $canonical -Force).FullName
        }
    }
    throw 'No valid Git for Windows application with the canonical Git/cmd/git.exe layout was found'
}
