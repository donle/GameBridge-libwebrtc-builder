function Resolve-RtcDPath {
    param([Parameter(Mandatory)][string]$Path)
    if ($Path -notmatch '^[dD]:[\\/]') { throw 'RTC artifact path must be an absolute D: path' }
    $full=[IO.Path]::GetFullPath($Path)
    if ([IO.Path]::GetPathRoot($full) -ine 'D:\' -or $full.Substring(2).Contains(':')) { throw 'Invalid RTC artifact path' }
    $cursor=$full
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item=Get-Item -LiteralPath $cursor -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'RTC artifact path traverses a reparse point' }
        }
        $parent=[IO.Directory]::GetParent($cursor)
        if ($null -eq $parent) { break }
        $cursor=$parent.FullName
    }
    $full.TrimEnd('\')
}
function Assert-RtcSha256 {
    param([string]$Path,[string]$Expected)
    if ($Expected -notmatch '^[a-fA-F0-9]{64}$' -or (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash -ine $Expected) { throw 'RTC artifact SHA-256 mismatch' }
}
function Assert-RtcProvenanceRun {
    param([object[]]$Attestations,[string]$Repository,[long]$RunId,[long]$RunAttempt)
    if ($RunId -lt 1 -or $RunAttempt -lt 1) { throw 'Invalid RTC provenance run identity' }
    $expected="https://github.com/$Repository/actions/runs/$RunId/attempts/$RunAttempt"
    foreach ($attestation in $Attestations) {
        try {
            $verified=$attestation.verificationResult
            # Use only the statement/certificate parsed by successful gh
            # verification, never an unverified bundle or API download URL.
            # The certificate extension binds GitHub's OIDC run identity too.
            if ($verified.statement.predicateType -ceq 'https://slsa.dev/provenance/v1' -and
                $verified.statement.predicate.runDetails.metadata.invocationId -ceq $expected -and
                $verified.signature.certificate.runInvocationURI -ceq $expected) { return }
        } catch { # Missing or malformed fields do not establish identity.
        }
    }
    throw 'No verified RTC provenance for the requested GitHub run and attempt'
}
function Copy-RtcBoundedStream {
    param(
        [IO.Stream]$InputStream,[IO.Stream]$OutputStream,
        [long]$MaximumBytes,[long]$ExpectedBytes=-1,
        [ref]$TotalBytes,[long]$MaximumTotalBytes
    )
    if ($MaximumBytes -lt 0 -or $MaximumTotalBytes -lt 0 -or $ExpectedBytes -lt -1 -or
        $TotalBytes.Value -lt 0 -or $TotalBytes.Value -gt $MaximumTotalBytes) { throw 'Invalid RTC stream bounds' }
    $buffer=New-Object byte[] 65536
    [long]$written=0
    while ($true) {
        # Read at most one byte beyond the remaining budget to detect overflow;
        # never write that byte or trust a ZIP/HTTP advertised length.
        [long]$remaining=[Math]::Min($MaximumBytes-$written,$MaximumTotalBytes-$TotalBytes.Value)
        if ($ExpectedBytes -ge 0) { $remaining=[Math]::Min($remaining,$ExpectedBytes-$written) }
        $read=$InputStream.Read($buffer,0,[int][Math]::Min($buffer.Length,$remaining+1))
        if (!$read) { break }
        if ($read -gt $remaining) { throw 'RTC stream exceeds actual byte or declared length bounds' }
        $OutputStream.Write($buffer,0,$read)
        $written += $read
        $TotalBytes.Value += $read
    }
    if ($ExpectedBytes -ge 0 -and $written -ne $ExpectedBytes) { throw 'RTC stream declared length mismatch' }
}
function Save-RtcBoundedProcessOutput {
    param([string]$FilePath,[string]$Arguments,[string]$Output,[long]$MaximumBytes=314572800)
    # Binary stdout is consumed directly, including on Windows PowerShell 5.
    # Do not redirect it to an unbounded file and check the size afterwards.
    $process=[Diagnostics.Process]::new()
    $process.StartInfo=[Diagnostics.ProcessStartInfo]::new()
    $process.StartInfo.FileName=$FilePath
    $process.StartInfo.Arguments=$Arguments
    $process.StartInfo.UseShellExecute=$false
    $process.StartInfo.CreateNoWindow=$true
    $process.StartInfo.RedirectStandardOutput=$true
    $started=$false
    $destination=[IO.File]::Open($Output,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    try {
        $started=$process.Start()
        if (!$started) { throw 'RTC download process did not start' }
        [long]$total=0
        Copy-RtcBoundedStream $process.StandardOutput.BaseStream $destination $MaximumBytes -1 ([ref]$total) $MaximumBytes
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw 'RTC artifact download failed' }
    } finally {
        if ($started -and !$process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $destination.Dispose()
        $process.Dispose()
    }
}
function Test-RtcZip {
    param([string]$Path,[string[]]$AllowedFiles,[long]$MaximumExpandedBytes=268435456,[long]$MaximumEntryBytes=268435456)
    Add-Type -AssemblyName System.IO.Compression
    $file=[IO.File]::OpenRead($Path)
    try {
        $zip=[IO.Compression.ZipArchive]::new($file,[IO.Compression.ZipArchiveMode]::Read,$true)
        try {
            $names=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
            [long]$total=0
            foreach($entry in $zip.Entries) {
                if ($AllowedFiles -cnotcontains $entry.FullName -or !$names.Add($entry.FullName) -or $entry.FullName -match '[\\/:]' -or (($entry.ExternalAttributes -shr 16) -band 0xf000) -eq 0xa000) { throw 'Unexpected, duplicate or unsafe RTC archive entry' }
                if ($entry.Length -lt 1 -or $entry.Length -gt $MaximumEntryBytes -or $entry.Length -gt $MaximumExpandedBytes-$total) { throw 'RTC archive expanded size exceeds bounds' }
                $input=$entry.Open()
                try {
                    Copy-RtcBoundedStream $input ([IO.Stream]::Null) $MaximumEntryBytes $entry.Length ([ref]$total) $MaximumExpandedBytes
                } finally { $input.Dispose() }
            }
            if (!$names.Count) { throw 'Empty RTC artifact archive' }
        } finally { $zip.Dispose() }
    } finally { $file.Dispose() }
}
function Copy-RtcZipEntry {
    param([string]$Archive,[string]$EntryName,[string]$Output,[long]$MaximumEntryBytes=268435456,[ref]$TotalBytes,[long]$MaximumExpandedBytes=268435456)
    # Only called after the complete archive entry allowlist has been checked.
    $file=[IO.File]::OpenRead($Archive)
    try {
        $zip=[IO.Compression.ZipArchive]::new($file,[IO.Compression.ZipArchiveMode]::Read,$true)
        try {
            $entry=$zip.GetEntry($EntryName)
            if ($null -eq $entry -or $entry.Length -lt 1 -or $entry.Length -gt $MaximumEntryBytes) { throw 'Required RTC artifact entry missing or oversize' }
            if ($null -eq $TotalBytes) { [long]$localTotal=0; $TotalBytes=[ref]$localTotal }
            $input=$entry.Open()
            try {
                $destination=[IO.File]::Open($Output,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
                try {
                    Copy-RtcBoundedStream $input $destination $MaximumEntryBytes $entry.Length $TotalBytes $MaximumExpandedBytes
                } finally { $destination.Dispose() }
            } finally { $input.Dispose() }
        } finally { $zip.Dispose() }
    } finally { $file.Dispose() }
}
