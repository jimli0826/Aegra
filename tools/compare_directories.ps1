<#
.SYNOPSIS
    Recursively compare two directories and report all file differences.
.DESCRIPTION
    Checks: only-in-source, only-in-target, size mismatch, SHA256 hash mismatch.
    Use -PassThru to return a structured result object.
#>

param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir,

    [Parameter(Mandatory = $true)]
    [string]$TargetDir,

    [switch]$PassThru
)

$ErrorActionPreference = 'Continue'
$HashAlgorithm = 'SHA256'

function Get-NormalizedDirectoryPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $pathRoot = [System.IO.Path]::GetPathRoot($fullPath)
    if ($fullPath.TrimEnd('\', '/') -eq $pathRoot.TrimEnd('\', '/')) {
        return $pathRoot
    }
    return $fullPath.TrimEnd('\', '/')
}

function Get-RelativeFilePath {
    param(
        [Parameter(Mandatory = $true)][string]$BaseDirectory,
        [Parameter(Mandatory = $true)][string]$FilePath
    )
    $prefix = $BaseDirectory.TrimEnd('\', '/') + '\'
    if (-not $FilePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "File '$FilePath' is outside directory '$BaseDirectory'."
    }
    return $FilePath.Substring($prefix.Length)
}

$SourceDir = Get-NormalizedDirectoryPath -Path $SourceDir
$TargetDir = Get-NormalizedDirectoryPath -Path $TargetDir

if (-not (Test-Path -LiteralPath $SourceDir -PathType Container)) {
    Write-Host "ERROR: Source directory not found: $SourceDir"
    exit 1
}
if (-not (Test-Path -LiteralPath $TargetDir -PathType Container)) {
    Write-Host "ERROR: Target directory not found: $TargetDir"
    exit 1
}

$onlyInSource    = New-Object System.Collections.Generic.List[string]
$onlyInTarget    = New-Object System.Collections.Generic.List[string]
$sizeMismatch    = New-Object System.Collections.Generic.List[PSCustomObject]
$contentMismatch = New-Object System.Collections.Generic.List[PSCustomObject]
$identical       = 0
$errorList       = New-Object System.Collections.Generic.List[string]

function Get-FileHashSafe {
    param([string]$FilePath, [string]$Algorithm)
    try {
        return (Get-FileHash -LiteralPath $FilePath -Algorithm $Algorithm -ErrorAction Stop).Hash
    }
    catch { return $null }
}

function Format-SizeBytes {
    param([UInt64]$Bytes)
    if ($Bytes -ge 1TB) { return "{0:F2} TB" -f ($Bytes / 1TB) }
    if ($Bytes -ge 1GB) { return "{0:F2} GB" -f ($Bytes / 1GB) }
    if ($Bytes -ge 1MB) { return "{0:F2} MB" -f ($Bytes / 1MB) }
    if ($Bytes -ge 1KB) { return "{0:F2} KB" -f ($Bytes / 1KB) }
    return "$Bytes B"
}

Write-Host ('=' * 70)
Write-Host 'Directory Compare Report'
Write-Host ('=' * 70)
Write-Host "Source : $SourceDir"
Write-Host "Target : $TargetDir"
Write-Host "Hash   : $HashAlgorithm"
Write-Host ''

Write-Host '[1/4] Enumerating source...'
$sourceFiles = @{}
Get-ChildItem -LiteralPath $SourceDir -File -Recurse -ErrorAction SilentlyContinue -ErrorVariable +e1 |
    ForEach-Object {
        $rel = Get-RelativeFilePath -BaseDirectory $SourceDir -FilePath $_.FullName
        $sourceFiles[$rel] = $_
    }
if ($e1) { foreach ($e in $e1) { $errorList.Add("Src enum: $($e.Exception.Message)") } }
Write-Host "  Files: $($sourceFiles.Count)"

Write-Host '[2/4] Enumerating target...'
$targetFiles = @{}
Get-ChildItem -LiteralPath $TargetDir -File -Recurse -ErrorAction SilentlyContinue -ErrorVariable +e2 |
    ForEach-Object {
        $rel = Get-RelativeFilePath -BaseDirectory $TargetDir -FilePath $_.FullName
        $targetFiles[$rel] = $_
    }
if ($e2) { foreach ($e in $e2) { $errorList.Add("Tgt enum: $($e.Exception.Message)") } }
Write-Host "  Files: $($targetFiles.Count)"
Write-Host ''

Write-Host '[3/4] Comparing file lists...'
foreach ($k in $sourceFiles.Keys) {
    if (-not $targetFiles.ContainsKey($k)) { $onlyInSource.Add($k) }
}
foreach ($k in $targetFiles.Keys) {
    if (-not $sourceFiles.ContainsKey($k)) { $onlyInTarget.Add($k) }
}

$common = $sourceFiles.Keys | Where-Object { $targetFiles.ContainsKey($_) }
$hashComparisons = 0
$processed = 0

Write-Host '[4/4] Hashing common files...'
foreach ($rel in $common) {
    $processed++
    if (($processed % 100) -eq 0) {
        Write-Host "  $processed / $($common.Count)"
    }

    $sf = $sourceFiles[$rel]
    $tf = $targetFiles[$rel]

    if ($sf.Length -ne $tf.Length) {
        $sizeMismatch.Add([PSCustomObject]@{ RelativePath = $rel; SourceSize = $sf.Length; TargetSize = $tf.Length })
        continue
    }

    $sh = Get-FileHashSafe -FilePath $sf.FullName -Algorithm $HashAlgorithm
    $th = Get-FileHashSafe -FilePath $tf.FullName -Algorithm $HashAlgorithm

    if ($null -eq $sh) { $errorList.Add("Hash fail src: $rel"); continue }
    if ($null -eq $th) { $errorList.Add("Hash fail tgt: $rel"); continue }

    $hashComparisons++
    if ($sh -ne $th) {
        $contentMismatch.Add([PSCustomObject]@{ RelativePath = $rel; SourceHash = $sh; TargetHash = $th; FileSize = $sf.Length })
    }
    else { $identical++ }
}

Write-Host ''
Write-Host ('=' * 70)
Write-Host 'Comparison Results'
Write-Host ('=' * 70)
Write-Host "Source files     : $($sourceFiles.Count)"
Write-Host "Target files     : $($targetFiles.Count)"
Write-Host "Identical        : $identical"
Write-Host "Only in source   : $($onlyInSource.Count)"
Write-Host "Only in target   : $($onlyInTarget.Count)"
Write-Host "Size mismatch    : $($sizeMismatch.Count)"
Write-Host "Content mismatch : $($contentMismatch.Count)"
Write-Host "Errors           : $($errorList.Count)"
Write-Host ''

if ($onlyInSource.Count -gt 0) {
    Write-Host ('-' * 70)
    Write-Host "[ONLY IN SOURCE] ($($onlyInSource.Count) files)"
    Write-Host ('-' * 70)
    $onlyInSource | Sort-Object | ForEach-Object { Write-Host "  MISS: $_" }
    Write-Host ''
}

if ($onlyInTarget.Count -gt 0) {
    Write-Host ('-' * 70)
    Write-Host "[ONLY IN TARGET] ($($onlyInTarget.Count) files)"
    Write-Host ('-' * 70)
    $onlyInTarget | Sort-Object | ForEach-Object { Write-Host "  EXTRA: $_" }
    Write-Host ''
}

if ($sizeMismatch.Count -gt 0) {
    Write-Host ('-' * 70)
    Write-Host "[SIZE MISMATCH] ($($sizeMismatch.Count) files)"
    Write-Host ('-' * 70)
    $sizeMismatch | Sort-Object RelativePath | ForEach-Object {
        Write-Host "  SIZE: $($_.RelativePath)"
        Write-Host "        src=$(Format-SizeBytes $_.SourceSize)  tgt=$(Format-SizeBytes $_.TargetSize)"
    }
    Write-Host ''
}

if ($contentMismatch.Count -gt 0) {
    Write-Host ('-' * 70)
    Write-Host "[CONTENT MISMATCH] ($($contentMismatch.Count) files)"
    Write-Host ('-' * 70)
    $contentMismatch | Sort-Object RelativePath | ForEach-Object {
        Write-Host "  HASH: $($_.RelativePath) ($(Format-SizeBytes $_.FileSize))"
        Write-Host "        src=$($_.SourceHash)"
        Write-Host "        tgt=$($_.TargetHash)"
    }
    Write-Host ''
}

if ($errorList.Count -gt 0) {
    Write-Host ('-' * 70)
    Write-Host "[ERRORS] ($($errorList.Count))"
    Write-Host ('-' * 70)
    $errorList | ForEach-Object { Write-Host "  ERROR: $_" }
    Write-Host ''
}

$totalProblems = $onlyInSource.Count + $onlyInTarget.Count + $sizeMismatch.Count + $contentMismatch.Count

Write-Host ('=' * 70)
if ($totalProblems -eq 0 -and $errorList.Count -eq 0) {
    Write-Host 'RESULT: IDENTICAL - all files match.'
    $exitCode = 0
}
else {
    Write-Host "RESULT: DIFFER - $totalProblems difference(s), $($errorList.Count) error(s)."
    $exitCode = 1
}
Write-Host ('=' * 70)

if ($PassThru) {
    [PSCustomObject]@{
        SourceFileCount    = $sourceFiles.Count
        TargetFileCount    = $targetFiles.Count
        Identical          = $identical
        OnlyInSource       = $onlyInSource.ToArray()
        OnlyInTarget       = $onlyInTarget.ToArray()
        SizeMismatch       = $sizeMismatch.ToArray()
        ContentMismatch    = $contentMismatch.ToArray()
        HashComparisons    = $hashComparisons
        Errors             = $errorList.ToArray()
        IsIdentical        = ($totalProblems -eq 0 -and $errorList.Count -eq 0)
    }
}

exit $exitCode
