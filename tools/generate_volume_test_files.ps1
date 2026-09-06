param(
    [Parameter(Mandatory = $true)]
    [string]$VolumePath,

    [Parameter(Mandatory = $true)]
    [double]$TotalSizeGB,

    [UInt64]$MinFileSizeBytes = 1048576,      # 1 MiB
    [UInt64]$MaxFileSizeBytes = 67108864,     # 64 MiB

    [int]$MaxDepth = 4,
    [int]$MaxDirectories = 32,
    [double]$ZeroFileRatio = 0.35,

    [int]$RandomSeed = 0,
    [switch]$AllZero
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# 1 GB = 1024^3 bytes
$TotalSizeBytes = [UInt64]([Math]::Round($TotalSizeGB * 1GB))

function Assert-ParamRange {
    param(
        [string]$Name,
        [double]$Value,
        [double]$Min,
        [double]$Max
    )
    if ($Value -lt $Min -or $Value -gt $Max) {
        throw "$Name must be between $Min and $Max. Actual: $Value"
    }
}

function New-ZeroBytesFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][UInt64]$Size
    )

    $bufferSize = 1024 * 1024
    $buffer = New-Object byte[] $bufferSize

    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try {
        $remaining = $Size
        while ($remaining -gt 0) {
            $writeCount = [int][Math]::Min([UInt64]$bufferSize, $remaining)
            $stream.Write($buffer, 0, $writeCount)
            $remaining -= [UInt64]$writeCount
        }
        $stream.Flush()
    }
    finally {
        $stream.Dispose()
    }
}

function New-RandomBytesFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][UInt64]$Size,
        [Parameter(Mandatory = $true)][System.Random]$Random
    )

    $bufferSize = 1024 * 1024
    $buffer = New-Object byte[] $bufferSize

    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try {
        $remaining = $Size
        while ($remaining -gt 0) {
            $writeCount = [int][Math]::Min([UInt64]$bufferSize, $remaining)
            $Random.NextBytes($buffer)
            $stream.Write($buffer, 0, $writeCount)
            $remaining -= [UInt64]$writeCount
        }
        $stream.Flush()
    }
    finally {
        $stream.Dispose()
    }
}

function Get-RandomFileSize {
    param(
        [Parameter(Mandatory = $true)][UInt64]$Min,
        [Parameter(Mandatory = $true)][UInt64]$Max,
        [Parameter(Mandatory = $true)][UInt64]$Remaining,
        [Parameter(Mandatory = $true)][System.Random]$Random
    )

    if ($Remaining -le $Min) {
        return $Remaining
    }

    $maxCandidate = [UInt64][Math]::Min($Max, $Remaining)
    if ($maxCandidate -le $Min) {
        return $maxCandidate
    }

    $range = [double]($maxCandidate - $Min)
    $pick = [UInt64]([Math]::Floor($Random.NextDouble() * ($range + 1.0)))
    return ($Min + $pick)
}

Assert-ParamRange -Name 'ZeroFileRatio' -Value $ZeroFileRatio -Min 0 -Max 1
if ($TotalSizeGB -le 0) {
    throw 'TotalSizeGB must be greater than 0.'
}
if ($TotalSizeBytes -eq 0) {
    throw 'TotalSizeGB is too small; converted TotalSizeBytes is 0.'
}
if ($MinFileSizeBytes -eq 0) {
    throw 'MinFileSizeBytes must be greater than 0.'
}
if ($MaxFileSizeBytes -lt $MinFileSizeBytes) {
    throw 'MaxFileSizeBytes must be greater than or equal to MinFileSizeBytes.'
}
if ($MaxDepth -lt 1) {
    throw 'MaxDepth must be >= 1.'
}
if ($MaxDirectories -lt 1) {
    throw 'MaxDirectories must be >= 1.'
}

$targetRootInput = $VolumePath.Trim()
if (-not (Test-Path -LiteralPath $targetRootInput)) {
    throw "VolumePath does not exist: $targetRootInput"
}

$targetRootResolved = (Resolve-Path -LiteralPath $targetRootInput).Path
$timestamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$scenarioRoot = Join-Path $targetRootResolved "AegraImageGen_$timestamp"

$seed = $RandomSeed
if ($seed -eq 0) {
    $seed = [Environment]::TickCount
}
$rand = [System.Random]::new($seed)

Write-Host "Target root      : $targetRootResolved"
Write-Host "Scenario folder  : $scenarioRoot"
Write-Host "Total size       : $TotalSizeGB GB ($TotalSizeBytes bytes)"
Write-Host "Min/Max file size: $MinFileSizeBytes / $MaxFileSizeBytes"
Write-Host "Max depth        : $MaxDepth"
Write-Host "Max directories  : $MaxDirectories"
Write-Host "Zero ratio       : $ZeroFileRatio"
Write-Host "All zero         : $AllZero"
Write-Host "Random seed      : $seed"

New-Item -ItemType Directory -Path $scenarioRoot -Force | Out-Null

# Build a guaranteed depth chain to ensure files can be created across different directory levels.
$directories = New-Object System.Collections.Generic.List[object]
$directories.Add([PSCustomObject]@{ Path = $scenarioRoot; Depth = 0 }) | Out-Null

$current = $scenarioRoot
for ($d = 1; $d -le $MaxDepth; $d++) {
    $current = Join-Path $current ("L{0}" -f $d)
    New-Item -ItemType Directory -Path $current -Force | Out-Null
    $directories.Add([PSCustomObject]@{ Path = $current; Depth = $d }) | Out-Null
}

$remainingDirs = [Math]::Max(0, $MaxDirectories - $directories.Count)
for ($i = 0; $i -lt $remainingDirs; $i++) {
    $parentIndex = $rand.Next(0, $directories.Count)
    $parent = $directories[$parentIndex]

    if ($parent.Depth -ge $MaxDepth) {
        continue
    }

    $depth = $parent.Depth + 1
    $name = "D{0:D3}_{1}" -f $depth, $i
    $path = Join-Path $parent.Path $name

    if (-not (Test-Path -LiteralPath $path)) {
        New-Item -ItemType Directory -Path $path -Force | Out-Null
        $directories.Add([PSCustomObject]@{ Path = $path; Depth = $depth }) | Out-Null
    }
}

$directoriesByDepth = @{}
foreach ($dir in $directories) {
    if (-not $directoriesByDepth.ContainsKey($dir.Depth)) {
        $directoriesByDepth[$dir.Depth] = New-Object System.Collections.Generic.List[string]
    }
    $directoriesByDepth[$dir.Depth].Add($dir.Path) | Out-Null
}

$totalWritten = [UInt64]0
$zeroBytes = [UInt64]0
$randomBytes = [UInt64]0
$fileCount = 0
$zeroFiles = 0
$randomFiles = 0

# First pass: place at least one file in each depth level (if size budget allows).
for ($depth = 0; $depth -le $MaxDepth; $depth++) {
    if ($totalWritten -ge $TotalSizeBytes) {
        break
    }

    if (-not $directoriesByDepth.ContainsKey($depth)) {
        continue
    }

    $remaining = $TotalSizeBytes - $totalWritten
    $size = Get-RandomFileSize -Min $MinFileSizeBytes -Max $MaxFileSizeBytes -Remaining $remaining -Random $rand

    $depthDirs = $directoriesByDepth[$depth]
    $dirPath = $depthDirs[$rand.Next(0, $depthDirs.Count)]

    $makeZero = $AllZero.IsPresent -or ($rand.NextDouble() -lt $ZeroFileRatio)
    $prefix = if ($makeZero) { 'Z' } else { 'R' }
    $fileName = "{0}_{1:D5}_{2}.bin" -f $prefix, $fileCount, $size
    $filePath = Join-Path $dirPath $fileName

    if ($makeZero) {
        New-ZeroBytesFile -Path $filePath -Size $size
        $zeroBytes += $size
        $zeroFiles++
    }
    else {
        New-RandomBytesFile -Path $filePath -Size $size -Random $rand
        $randomBytes += $size
        $randomFiles++
    }

    $totalWritten += $size
    $fileCount++
}

# Second pass: fill remaining budget.
while ($totalWritten -lt $TotalSizeBytes) {
    $remaining = $TotalSizeBytes - $totalWritten
    $size = Get-RandomFileSize -Min $MinFileSizeBytes -Max $MaxFileSizeBytes -Remaining $remaining -Random $rand

    $dirObj = $directories[$rand.Next(0, $directories.Count)]
    $makeZero = $AllZero.IsPresent -or ($rand.NextDouble() -lt $ZeroFileRatio)
    $prefix = if ($makeZero) { 'Z' } else { 'R' }
    $fileName = "{0}_{1:D5}_{2}.bin" -f $prefix, $fileCount, $size
    $filePath = Join-Path $dirObj.Path $fileName

    if ($makeZero) {
        New-ZeroBytesFile -Path $filePath -Size $size
        $zeroBytes += $size
        $zeroFiles++
    }
    else {
        New-RandomBytesFile -Path $filePath -Size $size -Random $rand
        $randomBytes += $size
        $randomFiles++
    }

    $totalWritten += $size
    $fileCount++

    if (($fileCount % 50) -eq 0 -or $totalWritten -eq $TotalSizeBytes) {
        Write-Host ("Progress: files={0}, written={1}/{2} bytes" -f $fileCount, $totalWritten, $TotalSizeBytes)
    }
}

Write-Host ''
Write-Host 'Generation complete.'
Write-Host ("Scenario path : {0}" -f $scenarioRoot)
Write-Host ("Directories   : {0}" -f $directories.Count)
Write-Host ("Files         : {0}" -f $fileCount)
Write-Host ("Zero files    : {0} ({1} bytes)" -f $zeroFiles, $zeroBytes)
Write-Host ("Random files  : {0} ({1} bytes)" -f $randomFiles, $randomBytes)
Write-Host ("Total written : {0} bytes" -f $totalWritten)
