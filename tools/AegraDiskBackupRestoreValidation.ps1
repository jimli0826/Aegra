<#
.SYNOPSIS
    Generates source data, runs an Aegra disk backup and restore, and verifies file contents.
.DESCRIPTION
    This is a destructive validation utility for isolated, non-system test disks. It uses an
    existing volume_set schedule, waits for the backup, finds the newly published recovery point,
    restores the source disk from that point to an explicitly confirmed target disk, compares all
    source and target volume files, mounts the same image, and compares source and mounted files.
    The complete workflow can be repeated for a configured number of iterations.
#>

[CmdletBinding()]
param()

# =============================================================================
# USER CONFIGURATION - EDIT THESE VALUES BEFORE RUNNING THE SCRIPT
# =============================================================================
$AegraCliPath = 'C:\Program Files\AegraImage\AegraCLI.exe'
$ScheduleId = 'sch-e8cc5023f8168d219202370a2a99b3c4'
$SourceVolumePath = 'E:\'
$TargetVolumePath = 'G:\'
$TargetDiskSourceId = 'disk.2'
$ConfirmTargetDiskSourceId = 'disk.2'
$SourceDiskNumber = [UInt32]0
$TotalSizeGB = 1.0
$SourceFreeSpaceReserveGB = 1.0 # Free space kept after generating this iteration's files.
$BackupType = 'incremental' # full or incremental
$JobTimeoutMinutes = 180
$CatalogPublishTimeoutSeconds = 60
$TargetMountTimeoutSeconds = 60
$ImageMountTimeoutSeconds= 60
$PreserveDiskSignature = $false
$AutoExpandLastPartition = $false
$ConfirmDestructiveRestore = $true # Change to $true only after checking every disk value above.
$MountedVolumePath = 'H:\'
$IterationCount = [UInt32]10000 # Number of complete generate/backup/restore/compare cycles.
$IterationIntervalSeconds = [UInt32]60 # Wait time between iterations; zero disables the wait.
$StopOnFirstFailure = $true # $true stops safely after the first failed iteration.

# Usually no changes are needed below this line.
$GeneratorScriptPath = Join-Path $PSScriptRoot 'generate_volume_test_files.ps1'
$CompareScriptPath = Join-Path $PSScriptRoot 'compare_directories.ps1'
$ResultDirectory = Join-Path $PSScriptRoot 'results'

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:RunStopwatch = $null
$script:StageStopwatches = $null
$script:Result = $null
$script:ActiveMountSessionId = $null

function Initialize-IterationResult {
    param([Parameter(Mandatory = $true)][UInt32]$IterationNumber)
    $script:RunStopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $script:StageStopwatches = [ordered]@{}
    $script:ActiveMountSessionId = $null
    $script:Result = [ordered]@{
        iteration = $IterationNumber
        run_id = [Guid]::NewGuid().ToString('D')
        started_utc = [DateTime]::UtcNow.ToString('o')
        completed_utc = $null
        duration_seconds = $null
        interval_after_seconds = 0
        outcome = 'running'
        failed_stage = $null
        error = $null
        source_space = $null
        generated_scenario = $null
        backup_job_id = $null
        backup_state = $null
        recovery_point_id = $null
        repository_connection_id = $null
        restore_job_id = $null
        restore_state = $null
        restored_comparison = $null
        mount_session_id = $null
        mount_state = $null
        mounted_comparison = $null
        unmount_outcome = 'not_started'
        unmount_error = $null
        stages = [ordered]@{}
    }
}

function Write-StatusLine {
    param([Parameter(Mandatory = $true)][string]$Text)
    Write-Host $Text
}

function Start-Stage {
    param([Parameter(Mandatory = $true)][string]$Name)
    $script:Result.failed_stage = $Name
    $script:StageStopwatches[$Name] = [System.Diagnostics.Stopwatch]::StartNew()
    $script:Result.stages[$Name] = [ordered]@{
        outcome = 'running'
        duration_seconds = $null
    }
    Write-StatusLine "START $Name"
}

function Stop-StageTimer {
    param([Parameter(Mandatory = $true)][string]$Name)
    if (-not $script:StageStopwatches.Contains($Name)) {
        return
    }
    $stopwatch = $script:StageStopwatches[$Name]
    $stopwatch.Stop()
    $script:Result.stages[$Name].duration_seconds =
        [Math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
    [void]$script:StageStopwatches.Remove($Name)
}

function Complete-Stage {
    param([Parameter(Mandatory = $true)][string]$Name)
    $stage = $script:Result.stages[$Name]
    Stop-StageTimer -Name $Name
    $stage.outcome = 'succeeded'
    Write-StatusLine "PASS  $Name"
}

function Write-ResultFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Data
    )
    $json = $Data | ConvertTo-Json -Depth 20
    $temporaryPath = "$Path.tmp"
    [System.IO.File]::WriteAllText($temporaryPath, $json, [System.Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporaryPath -Destination $Path -Force
}

function Invoke-AegraCliProcess {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $AegraCliPath
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.StandardOutputEncoding = [System.Text.UTF8Encoding]::new($false)
    $startInfo.StandardErrorEncoding = [System.Text.UTF8Encoding]::new($false)
    if ($null -eq $startInfo.ArgumentList) {
        throw 'This script requires PowerShell 7 or a newer .NET runtime to invoke AegraCLI safely.'
    }
    foreach ($argument in @('--json') + $Arguments) {
        [void]$startInfo.ArgumentList.Add($argument)
    }

    $process = [System.Diagnostics.Process]::new()
    try {
        $process.StartInfo = $startInfo
        if (-not $process.Start()) {
            throw 'AegraCLI process could not be started.'
        }
        $standardOutputTask = $process.StandardOutput.ReadToEndAsync()
        $standardErrorTask = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $standardOutput = $standardOutputTask.GetAwaiter().GetResult()
        $standardError = $standardErrorTask.GetAwaiter().GetResult()
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            StandardOutput = $standardOutput
            StandardError = $standardError
        }
    }
    finally {
        $process.Dispose()
    }
}

function Get-NativeJsonResponses {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    $result = Invoke-AegraCliProcess -Arguments $Arguments
    $output = @($result.StandardOutput -split '\r?\n' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $errors = @($result.StandardError -split '\r?\n' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    foreach ($line in $output) {
        Write-StatusLine "CLI   $line"
    }
    foreach ($line in $errors) {
        Write-StatusLine "CLIERR $line"
    }
    if ($result.ExitCode -ne 0) {
        throw "AegraCLI exited with code $($result.ExitCode): $($Arguments -join ' ')"
    }
    $responses = [System.Collections.Generic.List[object]]::new()
    foreach ($line in $output) {
        $text = ([string]$line).TrimStart([char]0xFEFF)
        if ($text.TrimStart().StartsWith('{')) {
            $responses.Add(($text | ConvertFrom-Json))
        }
    }
    if ($responses.Count -eq 0) {
        throw "AegraCLI returned no JSON response: $($Arguments -join ' ')"
    }
    return $responses.ToArray()
}

function Get-OnlyResponse {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    $responses = @(Get-NativeJsonResponses -Arguments $Arguments)
    if ($responses.Count -ne 1) {
        throw "Expected one AegraCLI response, got $($responses.Count)."
    }
    return $responses[0]
}

function Wait-Job {
    param([Parameter(Mandatory = $true)][string]$JobId)
    $waitMilliseconds = [Math]::Min([UInt64][UInt32]::MaxValue,
        [UInt64]$JobTimeoutMinutes * [UInt64]60 * [UInt64]1000)
    $response = Get-OnlyResponse -Arguments @(
        '--wait-timeout-ms', [string]$waitMilliseconds,
        'job', 'wait', '--id', $JobId
    )
    $items = @($response.payload.items)
    if ($items.Count -ne 1 -or $items[0].job_id -ne $JobId) {
        throw "Job wait returned an unexpected job payload for $JobId."
    }
    return $items[0]
}

function Get-Schedule {
    $responses = @(Get-NativeJsonResponses -Arguments @('schedule', 'list'))
    $matches = @($responses | ForEach-Object { $_.payload.items } |
        Where-Object { $_.schedule_id -eq $ScheduleId })
    if ($matches.Count -ne 1) {
        throw "Expected exactly one schedule with id '$ScheduleId'; found $($matches.Count)."
    }
    return $matches[0]
}

function Assert-SafeTargetDisk {
    param([Parameter(Mandatory = $true)][object]$Schedule)
    if ($TargetDiskSourceId -ne $ConfirmTargetDiskSourceId) {
        throw 'ConfirmTargetDiskSourceId must exactly match TargetDiskSourceId.'
    }
    $responses = @(Get-NativeJsonResponses -Arguments @('inventory', 'list'))
    $inventory = @($responses | ForEach-Object { $_.payload.items })
    $targets = @($inventory |
        Where-Object { $_.source_id -eq $TargetDiskSourceId })
    if ($targets.Count -ne 1) {
        throw "Target inventory id '$TargetDiskSourceId' was not found exactly once."
    }
    if ([bool]$targets[0].is_system) {
        throw "Refusing to restore the system disk '$TargetDiskSourceId'."
    }
    if ([int]$targets[0].availability -ne 1) {
        throw "Target disk '$TargetDiskSourceId' is unavailable."
    }
    $scheduledSourcesOnTarget = @($inventory | Where-Object {
        @($Schedule.source_ids) -contains $_.source_id -and
        [UInt32]$_.disk_number -eq [UInt32]$targets[0].disk_number
    })
    if ($scheduledSourcesOnTarget.Count -gt 0) {
        throw 'The target disk contains one or more sources from the backup schedule.'
    }
}

function Invoke-Generator {
    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $GeneratorScriptPath,
        '-VolumePath', $SourceVolumePath, '-TotalSizeGB', [string]$TotalSizeGB
    )
    $output = @(& powershell.exe @arguments 2>&1)
    $exitCode = $LASTEXITCODE
    foreach ($line in $output) {
        Write-StatusLine "GEN   $line"
    }
    if ($exitCode -ne 0) {
        throw "File generator exited with code $exitCode."
    }
    $match = $output | Select-String -Pattern '^Scenario path\s*:\s*(.+)$' | Select-Object -Last 1
    if ($null -eq $match) {
        throw 'Could not read the generated scenario path from generator output.'
    }
    return $match.Matches[0].Groups[1].Value.Trim()
}

function Get-SourceSpaceStatus {
    $generationGB = [double]$TotalSizeGB
    $reserveGB = [double]$SourceFreeSpaceReserveGB
    if ([double]::IsNaN($generationGB) -or [double]::IsInfinity($generationGB) -or
        $generationGB -le 0) {
        throw 'TotalSizeGB must be a finite number greater than zero.'
    }
    if ([double]::IsNaN($reserveGB) -or [double]::IsInfinity($reserveGB) -or
        $reserveGB -lt 0) {
        throw 'SourceFreeSpaceReserveGB must be a finite non-negative number.'
    }
    $generationBytesValue = [Math]::Ceiling($generationGB * 1GB)
    $reserveBytesValue = [Math]::Ceiling($reserveGB * 1GB)
    if ($generationBytesValue -gt [UInt64]::MaxValue -or
        $reserveBytesValue -gt [UInt64]::MaxValue - $generationBytesValue) {
        throw 'The configured source-space requirement is too large.'
    }
    $sourceRoot = [System.IO.Path]::GetPathRoot(
        [System.IO.Path]::GetFullPath($SourceVolumePath))
    $drive = [System.IO.DriveInfo]::new($sourceRoot)
    if (-not $drive.IsReady) {
        throw "Source volume is not ready: $sourceRoot"
    }
    $generationBytes = [UInt64]$generationBytesValue
    $reserveBytes = [UInt64]$reserveBytesValue
    return [ordered]@{
        available_bytes = [UInt64]$drive.AvailableFreeSpace
        required_bytes = $generationBytes + $reserveBytes
        generation_bytes = $generationBytes
        reserve_bytes = $reserveBytes
    }
}

function Get-NewRecoveryPoint {
    param(
        [Parameter(Mandatory = $true)][object]$Schedule,
        [Parameter(Mandatory = $true)][UInt64]$NotBeforeUtcMs
    )
    $deadline = [DateTime]::UtcNow.AddSeconds($CatalogPublishTimeoutSeconds)
    do {
        $responses = @(Get-NativeJsonResponses -Arguments @(
            'recovery-point', 'list', '--connection', $Schedule.repository_connection_id
        ))
        $points = @($responses | ForEach-Object { $_.payload.catalog.items } |
            Where-Object {
                $_.backup_set_uuid -eq $Schedule.backup_set_uuid -and
                [UInt64]$_.created_utc_ms -ge $NotBeforeUtcMs
            } | Sort-Object -Property created_utc_ms -Descending)
        if ($points.Count -gt 0) {
            return $points[0]
        }
        Start-Sleep -Seconds 2
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'The completed backup was not published in the recovery point catalog before timeout.'
}

function Wait-TargetVolume {
    $deadline = [DateTime]::UtcNow.AddSeconds($TargetMountTimeoutSeconds)
    do {
        if (Test-Path -LiteralPath $TargetVolumePath -PathType Container) {
            return
        }
        Start-Sleep -Seconds 2
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Target volume did not become available at '$TargetVolumePath'."
}

function Get-MountDriveLetter {
    $match = [Regex]::Match($MountedVolumePath, '^(?<letter>[D-Zd-z]):\\?$')
    if (-not $match.Success) {
        throw "MountedVolumePath must be a drive root from D:\ through Z:\."
    }
    return $match.Groups['letter'].Value.ToUpperInvariant()
}

function Wait-ImageMount {
    param([Parameter(Mandatory = $true)][string]$SessionId)
    $deadline = [DateTime]::UtcNow.AddSeconds($ImageMountTimeoutSeconds)
    $expectedMountPoint = $MountedVolumePath.TrimEnd('\', '/')
    do {
        $responses = @(Get-NativeJsonResponses -Arguments @('mount', 'list'))
        $matches = @($responses | ForEach-Object { $_.payload.items } |
            Where-Object { $_.session_id -eq $SessionId })
        if ($matches.Count -gt 1) {
            throw "Mount session '$SessionId' was returned more than once."
        }
        if ($matches.Count -eq 1) {
            $session = $matches[0]
            $script:Result.mount_state = [int]$session.state
            if ([int]$session.state -eq 4) {
                throw "Mount session failed: $($session.message_code)"
            }
            if ([int]$session.state -eq 2) {
                $mountPoints = @(([string]$session.mount_point -split '\s+') |
                    ForEach-Object { $_.TrimEnd('\', '/') })
                if ($mountPoints -notcontains $expectedMountPoint) {
                    throw "Mount session did not use requested drive '$expectedMountPoint'."
                }
                if (Test-Path -LiteralPath $MountedVolumePath -PathType Container) {
                    return $session
                }
            }
        }
        Start-Sleep -Seconds 2
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Mount session '$SessionId' did not become available before timeout."
}

function Invoke-ImageUnmount {
    param([Parameter(Mandatory = $true)][string]$SessionId)
    [void](Get-OnlyResponse -Arguments @('mount', 'unmount', '--id', $SessionId))
}

function Invoke-DirectoryComparison {
    param(
        [Parameter(Mandatory = $true)][string]$SourceDirectory,
        [Parameter(Mandatory = $true)][string]$TargetDirectory
    )
    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $CompareScriptPath,
        '-SourceDir', $SourceDirectory, '-TargetDir', $TargetDirectory
    )
    $output = @(& powershell.exe @arguments 2>&1)
    $exitCode = $LASTEXITCODE
    foreach ($line in $output) {
        Write-StatusLine "CMP   $line"
    }
    $summary = [ordered]@{
        is_identical = ($exitCode -eq 0)
        exit_code = $exitCode
        source_directory = $SourceDirectory
        target_directory = $TargetDirectory
    }
    foreach ($name in @('Source files', 'Target files', 'Identical', 'Only in source',
            'Only in target', 'Size mismatch', 'Content mismatch', 'Errors')) {
        $match = $output | Select-String -Pattern ("^{0}\s*:\s*([0-9]+)$" -f [Regex]::Escape($name)) |
            Select-Object -Last 1
        if ($null -ne $match) {
            $key = $name.ToLowerInvariant().Replace(' ', '_')
            $summary[$key] = [UInt64]$match.Matches[0].Groups[1].Value
        }
    }
    return $summary
}

function Invoke-ValidationIteration {
param([Parameter(Mandatory = $true)][UInt32]$IterationNumber)
Initialize-IterationResult -IterationNumber $IterationNumber
Write-StatusLine "ITERATION $IterationNumber OF $IterationCount"
try {
    Start-Stage 'preflight'
    if (-not $ConfirmDestructiveRestore) {
        throw 'ConfirmDestructiveRestore is required.'
    }
    foreach ($path in @($AegraCliPath, $GeneratorScriptPath, $CompareScriptPath)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Required file does not exist: $path"
        }
    }
    if (-not (Test-Path -LiteralPath $SourceVolumePath -PathType Container)) {
        throw "Source volume path does not exist: $SourceVolumePath"
    }
    if (-not (Test-Path -LiteralPath $TargetVolumePath -PathType Container)) {
        throw "Target volume path does not exist: $TargetVolumePath"
    }
    if ([System.IO.Path]::GetFullPath($SourceVolumePath) -eq
        [System.IO.Path]::GetFullPath($TargetVolumePath)) {
        throw 'SourceVolumePath and TargetVolumePath must be different.'
    }
    $mountDriveLetter = Get-MountDriveLetter
    if (Test-Path -LiteralPath $MountedVolumePath) {
        throw "MountedVolumePath is already in use: $MountedVolumePath"
    }
    $configuredPaths = @($SourceVolumePath, $TargetVolumePath) |
        ForEach-Object { [System.IO.Path]::GetFullPath($_) }
    if ($configuredPaths -contains [System.IO.Path]::GetFullPath($MountedVolumePath)) {
        throw 'MountedVolumePath must be different from source and target volume paths.'
    }
    $schedule = Get-Schedule
    if ([int]$schedule.content_kind -ne 1) {
        throw 'The selected schedule is not a volume_set schedule.'
    }
    if ([bool]$schedule.encryption_enabled) {
        throw 'Automated CLI disk restore supports only unencrypted schedules.'
    }
    Assert-SafeTargetDisk -Schedule $schedule
    $script:Result.repository_connection_id = $schedule.repository_connection_id
    Complete-Stage 'preflight'

    Start-Stage 'source_space'
    $script:Result.source_space = Get-SourceSpaceStatus
    if ($script:Result.source_space.available_bytes -lt
        $script:Result.source_space.required_bytes) {
        throw "Source volume free space is insufficient: available " +
            "$($script:Result.source_space.available_bytes) bytes, required " +
            "$($script:Result.source_space.required_bytes) bytes."
    }
    Complete-Stage 'source_space'

    Start-Stage 'generate_files'
    $scenarioPath = Invoke-Generator
    $script:Result.generated_scenario = $scenarioPath
    Complete-Stage 'generate_files'

    Start-Stage 'backup'
    $backupNotBeforeUtcMs = [UInt64]([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())
    $backupResponse = Get-OnlyResponse -Arguments @(
        'schedule', 'run', '--id', $ScheduleId, '--type', $BackupType
    )
    $backupJobId = [string]$backupResponse.payload.resource_id
    if ([string]::IsNullOrWhiteSpace($backupJobId)) {
        throw 'StartBackup did not return a job id.'
    }
    $script:Result.backup_job_id = $backupJobId
    $backupJob = Wait-Job -JobId $backupJobId
    $script:Result.backup_state = [int]$backupJob.state
    if ([int]$backupJob.state -ne 4) {
        throw "Backup job ended in state $($backupJob.state): $($backupJob.message_code)"
    }
    Complete-Stage 'backup'

    Start-Stage 'catalog'
    $recoveryPoint = Get-NewRecoveryPoint -Schedule $schedule -NotBeforeUtcMs $backupNotBeforeUtcMs
    $script:Result.recovery_point_id = [string]$recoveryPoint.file_uuid
    Complete-Stage 'catalog'

    Start-Stage 'restore'
    $restoreArguments = @(
        'restore', 'run',
        '--connection', [string]$schedule.repository_connection_id,
        '--recovery-point', [string]$recoveryPoint.file_uuid,
        '--target', $TargetDiskSourceId,
        '--source-disk-number', [string]$SourceDiskNumber,
        '--confirm-target', $ConfirmTargetDiskSourceId
    )
    if (-not $PreserveDiskSignature) {
        $restoreArguments += '--regenerate-disk-signature'
    }
    if (-not $AutoExpandLastPartition) {
        $restoreArguments += '--no-auto-expand'
    }
    $restoreResponse = Get-OnlyResponse -Arguments $restoreArguments
    $restoreJobId = [string]$restoreResponse.payload.resource_id
    if ([string]::IsNullOrWhiteSpace($restoreJobId)) {
        throw 'StartRestore did not return a job id.'
    }
    $script:Result.restore_job_id = $restoreJobId
    $restoreJob = Wait-Job -JobId $restoreJobId
    $script:Result.restore_state = [int]$restoreJob.state
    if ([int]$restoreJob.state -ne 4) {
        throw "Restore job ended in state $($restoreJob.state): $($restoreJob.message_code)"
    }
    Complete-Stage 'restore'

    Start-Stage 'compare_restore'
    Wait-TargetVolume
    $script:Result.restored_comparison = Invoke-DirectoryComparison `
        -SourceDirectory $SourceVolumePath -TargetDirectory $TargetVolumePath
    if (-not $script:Result.restored_comparison.is_identical) {
        throw "Restored volume comparison reported differences or errors " +
            "(exit code $($script:Result.restored_comparison.exit_code))."
    }
    Complete-Stage 'compare_restore'

    Start-Stage 'mount_image'
    $mountResponse = Get-OnlyResponse -Arguments @(
        '--timeout-ms', [string]([UInt32]$ImageMountTimeoutSeconds * [UInt32]1000),
        'mount', 'start',
        '--connection', [string]$schedule.repository_connection_id,
        '--recovery-point', [string]$recoveryPoint.file_uuid,
        '--source-disk-number', [string]$SourceDiskNumber,
        '--drive-letter', $mountDriveLetter
    )
    $mountSessionId = [string]$mountResponse.payload.resource_id
    if ([string]::IsNullOrWhiteSpace($mountSessionId)) {
        throw 'MountRecoveryPoint did not return a session id.'
    }
    $script:Result.mount_session_id = $mountSessionId
    $script:ActiveMountSessionId = $mountSessionId
    [void](Wait-ImageMount -SessionId $mountSessionId)
    Complete-Stage 'mount_image'

    Start-Stage 'compare_mounted_image'
    $script:Result.mounted_comparison = Invoke-DirectoryComparison `
        -SourceDirectory $SourceVolumePath -TargetDirectory $MountedVolumePath
    if (-not $script:Result.mounted_comparison.is_identical) {
        throw "Mounted image comparison reported differences or errors " +
            "(exit code $($script:Result.mounted_comparison.exit_code))."
    }
    Complete-Stage 'compare_mounted_image'

    Start-Stage 'unmount_image'
    $script:Result.unmount_outcome = 'running'
    Invoke-ImageUnmount -SessionId $mountSessionId
    $script:ActiveMountSessionId = $null
    $script:Result.unmount_outcome = 'succeeded'
    Complete-Stage 'unmount_image'

    $script:Result.failed_stage = $null
    $script:Result.outcome = 'succeeded'
}
catch {
    $script:Result.outcome = 'failed'
    $script:Result.error = $_.Exception.Message
    if ($null -ne $script:Result.failed_stage -and
        $script:Result.stages.Contains($script:Result.failed_stage)) {
        $failed = $script:Result.stages[$script:Result.failed_stage]
        Stop-StageTimer -Name $script:Result.failed_stage
        $failed.outcome = 'failed'
    }
    Write-StatusLine "FAIL  $($script:Result.failed_stage): $($_.Exception.Message)"
}
finally {
    if (-not [string]::IsNullOrWhiteSpace($script:ActiveMountSessionId)) {
        try {
            Write-StatusLine "CLEANUP unmount $($script:ActiveMountSessionId)"
            Invoke-ImageUnmount -SessionId $script:ActiveMountSessionId
            $script:Result.unmount_outcome = 'succeeded'
            $script:ActiveMountSessionId = $null
        }
        catch {
            $script:Result.unmount_outcome = 'failed'
            $script:Result.unmount_error = $_.Exception.Message
            Write-StatusLine "CLEANUP unmount failed: $($_.Exception.Message)"
        }
    }
    $script:RunStopwatch.Stop()
    $script:Result.completed_utc = [DateTime]::UtcNow.ToString('o')
    $script:Result.duration_seconds = [Math]::Round(
        $script:RunStopwatch.Elapsed.TotalSeconds, 3)
}
return $script:Result
}

New-Item -ItemType Directory -Path $ResultDirectory -Force | Out-Null
$batchId = [Guid]::NewGuid().ToString('D')
$resultPath = Join-Path $ResultDirectory (
    'aegra_disk_validation_batch_{0}_{1}.json' -f (Get-Date -Format 'yyyyMMdd_HHmmss'), $batchId
)
$batchStopwatch = [System.Diagnostics.Stopwatch]::StartNew()
$iterationResults = [System.Collections.Generic.List[object]]::new()
$batchResult = [ordered]@{
    schema_version = 6
    batch_id = $batchId
    started_utc = [DateTime]::UtcNow.ToString('o')
    completed_utc = $null
    duration_seconds = $null
    outcome = 'running'
    error = $null
    requested_iterations = $IterationCount
    completed_iterations = 0
    succeeded_iterations = 0
    failed_iterations = 0
    stop_on_first_failure = $StopOnFirstFailure
    iteration_interval_seconds = $IterationIntervalSeconds
    configuration = [ordered]@{
        schedule_id = $ScheduleId
        backup_type = $BackupType
        source_volume = $SourceVolumePath
        target_volume = $TargetVolumePath
        mounted_volume = $MountedVolumePath
        target_disk_source_id = $TargetDiskSourceId
        source_disk_number = $SourceDiskNumber
        generated_size_gb_per_iteration = $TotalSizeGB
        source_free_space_reserve_gb = $SourceFreeSpaceReserveGB
    }
    iterations = $iterationResults
}

if ($IterationCount -eq 0) {
    $batchResult.outcome = 'failed'
    $batchResult.error = 'IterationCount must be greater than zero.'
}
else {
    for ($iteration = [UInt32]1; $iteration -le $IterationCount; $iteration++) {
        $iterationResult = Invoke-ValidationIteration -IterationNumber $iteration
        [void]$iterationResults.Add($iterationResult)
        $batchResult.completed_iterations = $iterationResults.Count
        if ($iterationResult.outcome -eq 'succeeded') {
            $batchResult.succeeded_iterations++
        }
        else {
            $batchResult.failed_iterations++
            if ($null -eq $batchResult.error) {
                $batchResult.error = "Iteration $iteration failed: $($iterationResult.error)"
            }
        }
        Write-ResultFile -Path $resultPath -Data $batchResult
        $sourceSpaceFailure = $iterationResult.failed_stage -eq 'source_space'
        if ($iterationResult.outcome -ne 'succeeded' -and
            ($StopOnFirstFailure -or $sourceSpaceFailure)) {
            break
        }
        if ($iteration -lt $IterationCount -and $IterationIntervalSeconds -gt 0) {
            Write-StatusLine "WAIT  $IterationIntervalSeconds seconds before next iteration"
            $intervalStopwatch = [System.Diagnostics.Stopwatch]::StartNew()
            Start-Sleep -Seconds $IterationIntervalSeconds
            $intervalStopwatch.Stop()
            $iterationResult.interval_after_seconds = [Math]::Round(
                $intervalStopwatch.Elapsed.TotalSeconds, 3)
            Write-ResultFile -Path $resultPath -Data $batchResult
        }
    }
    $allSucceeded = $batchResult.failed_iterations -eq 0 -and
        $batchResult.completed_iterations -eq $IterationCount
    $batchResult.outcome = if ($allSucceeded) { 'succeeded' } else { 'failed' }
}

$batchStopwatch.Stop()
$batchResult.completed_utc = [DateTime]::UtcNow.ToString('o')
$batchResult.duration_seconds = [Math]::Round($batchStopwatch.Elapsed.TotalSeconds, 3)
Write-ResultFile -Path $resultPath -Data $batchResult
Write-Host "Result file: $resultPath"

if ($batchResult.outcome -ne 'succeeded') {
    exit 1
}
exit 0
