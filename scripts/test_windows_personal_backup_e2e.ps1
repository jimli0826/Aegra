#Requires -Version 5.1
#Requires -RunAsAdministrator

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [string]$WorkerPath,

    [string]$ArtifactsRoot = (Join-Path $env:TEMP 'AegraE2E'),

    [ValidateRange(1, 180)]
    [int]$TimeoutMinutes = 30,

    [Security.SecureString]$ArchivePassword,

    [switch]$SkipBuild,

    [switch]$KeepArtifacts
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$credentialInteropSource = @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security;
using System.Text;

public static class AegraCredentialNative
{
    private const uint CredTypeGeneric = 1;
    private const uint CredPersistSession = 1;
    private const int MaximumBlobBytes = 2560;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct Credential
    {
        public uint Flags;
        public uint Type;
        public string TargetName;
        public string Comment;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastWritten;
        public uint CredentialBlobSize;
        public IntPtr CredentialBlob;
        public uint Persist;
        public uint AttributeCount;
        public IntPtr Attributes;
        public string TargetAlias;
        public string UserName;
    }

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CredWriteW(ref Credential credential, uint flags);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CredDeleteW(string target, uint type, uint flags);

    public static void Write(string target, SecureString secret)
    {
        if (String.IsNullOrWhiteSpace(target) || secret == null || secret.Length == 0)
        {
            throw new ArgumentException("Credential target and password are required.");
        }

        IntPtr unicode = IntPtr.Zero;
        char[] characters = null;
        byte[] blob = null;
        GCHandle pinned = default(GCHandle);
        try
        {
            unicode = Marshal.SecureStringToCoTaskMemUnicode(secret);
            characters = new char[secret.Length];
            Marshal.Copy(unicode, characters, 0, characters.Length);
            blob = Encoding.UTF8.GetBytes(characters);
            if (blob.Length == 0 || blob.Length > MaximumBlobBytes)
            {
                throw new ArgumentException("UTF-8 credential blob has an invalid size.");
            }

            pinned = GCHandle.Alloc(blob, GCHandleType.Pinned);
            Credential credential = new Credential
            {
                Type = CredTypeGeneric,
                TargetName = target,
                Comment = "Temporary Aegra administrator E2E credential",
                CredentialBlobSize = (uint)blob.Length,
                CredentialBlob = pinned.AddrOfPinnedObject(),
                Persist = CredPersistSession,
                UserName = "AegraE2E"
            };
            if (!CredWriteW(ref credential, 0))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "CredWriteW failed");
            }
        }
        finally
        {
            if (pinned.IsAllocated) pinned.Free();
            if (blob != null) Array.Clear(blob, 0, blob.Length);
            if (characters != null) Array.Clear(characters, 0, characters.Length);
            if (unicode != IntPtr.Zero) Marshal.ZeroFreeCoTaskMemUnicode(unicode);
        }
    }

    public static void Delete(string target)
    {
        if (!CredDeleteW(target, CredTypeGeneric, 0))
        {
            int error = Marshal.GetLastWin32Error();
            if (error != 1168) throw new Win32Exception(error, "CredDeleteW failed");
        }
    }
}
'@

function Assert-Condition {
    param(
        [Parameter(Mandatory)]
        [bool]$Condition,

        [Parameter(Mandatory)]
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Write-Step {
    param([Parameter(Mandatory)][string]$Message)
    Write-Host "[Aegra E2E] $Message"
}

function Initialize-CredentialInterop {
    if ('AegraCredentialNative' -as [type]) {
        return
    }
    Add-Type -Language CSharp -TypeDefinition $credentialInteropSource
}

function Invoke-DiskPartScript {
    param(
        [Parameter(Mandatory)][string[]]$Lines,
        [Parameter(Mandatory)][string]$ScriptPath,
        [switch]$IgnoreFailure
    )

    [IO.File]::WriteAllLines($ScriptPath, $Lines, [Text.Encoding]::Unicode)
    $output = (& diskpart.exe /s $ScriptPath 2>&1 | Out-String).Trim()
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0 -and -not $IgnoreFailure) {
        throw "DiskPart failed with exit code $exitCode. Output: $output"
    }
    return $output
}

function Get-FreeDriveLetter {
    $used = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($drive in [IO.DriveInfo]::GetDrives()) {
        [void]$used.Add($drive.Name.Substring(0, 1))
    }
    foreach ($drive in Get-PSDrive -PSProvider FileSystem) {
        [void]$used.Add($drive.Name)
    }
    foreach ($code in 90..82) {
        $candidate = ([char]$code).ToString()
        if (-not $used.Contains($candidate)) {
            return $candidate
        }
    }
    throw 'No free drive letter is available between R: and Z:.'
}

function New-TestVolume {
    param(
        [Parameter(Mandatory)][string]$VhdPath,
        [Parameter(Mandatory)][string]$DriveLetter,
        [Parameter(Mandatory)][int]$SizeMiB,
        [Parameter(Mandatory)][string]$DiskPartPath
    )

    Assert-Condition (-not (Test-Path -LiteralPath $VhdPath)) `
        "Refusing to overwrite an existing VHDX: $VhdPath"

    $commands = @(
        "create vdisk file=`"$VhdPath`" maximum=$SizeMiB type=expandable",
        "select vdisk file=`"$VhdPath`"",
        'attach vdisk',
        'convert gpt',
        'create partition primary',
        'format fs=ntfs quick label="AegraE2E" unit=4096',
        "assign letter=$DriveLetter",
        'exit'
    )
    [void](Invoke-DiskPartScript -Lines $commands -ScriptPath $DiskPartPath)

    $root = "${DriveLetter}:\"
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while (-not (Test-Path -LiteralPath $root) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 250
    }
    Assert-Condition (Test-Path -LiteralPath $root) "Test VHD did not mount as $root."
}

function Dismount-TestVolume {
    param(
        [Parameter(Mandatory)][string]$VhdPath,
        [Parameter(Mandatory)][string]$DriveLetter,
        [Parameter(Mandatory)][string]$DiskPartPath
    )

    if (-not (Test-Path -LiteralPath $VhdPath)) {
        return
    }
    $commands = @(
        "select vdisk file=`"$VhdPath`"",
        'detach vdisk',
        'exit'
    )
    [void](Invoke-DiskPartScript -Lines $commands -ScriptPath $DiskPartPath)
    $volumeRoot = "${DriveLetter}:\"
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ((Test-Path -LiteralPath $volumeRoot) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 250
    }
    Assert-Condition (-not (Test-Path -LiteralPath $volumeRoot)) `
        "Test VHD remained mounted at $volumeRoot."
}

function Write-TestData {
    param([Parameter(Mandatory)][string]$VolumeRoot)

    $dataRoot = Join-Path $VolumeRoot 'AegraE2EData'
    $unicodeRoot = Join-Path $dataRoot '目录-Δοκιμή'
    New-Item -ItemType Directory -Force $unicodeRoot | Out-Null
    [IO.File]::WriteAllText(
        (Join-Path $dataRoot 'readme.txt'),
        "Aegra administrator E2E`r`nUnicode: 备份完整性`r`n",
        [Text.UTF8Encoding]::new($false))

    $randomPath = Join-Path $dataRoot 'random-8m.bin'
    $random = [Security.Cryptography.RandomNumberGenerator]::Create()
    $stream = [IO.File]::Open($randomPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
    try {
        $buffer = [byte[]]::new(1024 * 1024)
        for ($index = 0; $index -lt 8; ++$index) {
            $random.GetBytes($buffer)
            $stream.Write($buffer, 0, $buffer.Length)
        }
        [Array]::Clear($buffer, 0, $buffer.Length)
    }
    finally {
        $stream.Dispose()
        $random.Dispose()
    }

    $zeroPath = Join-Path $dataRoot 'zero-32m.bin'
    $zeroStream = [IO.File]::Open($zeroPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
    try {
        $zeroStream.SetLength(32MB)
    }
    finally {
        $zeroStream.Dispose()
    }

    1..64 | ForEach-Object {
        $path = Join-Path $unicodeRoot ("item-{0:D3}.txt" -f $_)
        [IO.File]::WriteAllText($path, "record=$_", [Text.UTF8Encoding]::new($false))
    }

    $hashes = Get-ChildItem -LiteralPath $dataRoot -File -Recurse |
        Sort-Object FullName |
        Get-FileHash -Algorithm SHA256 |
        Select-Object Path, Hash
    $hashes | ConvertTo-Json -Depth 3 |
        Out-File -LiteralPath (Join-Path $dataRoot 'source_hashes.json') -Encoding utf8
}

function Get-CanonicalVolumePath {
    param([Parameter(Mandatory)][string]$DriveLetter)

    $mountPoint = "${DriveLetter}:\"
    $output = (& mountvol.exe $mountPoint /L 2>&1 | Out-String)
    $match = [regex]::Match(
        $output,
        '\\\\\?\\Volume\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\}\\')
    Assert-Condition $match.Success "mountvol did not return a canonical Volume GUID path: $output"
    return $match.Value
}

function Get-ShadowCopyIds {
    return @(Get-CimInstance -ClassName Win32_ShadowCopy | ForEach-Object { $_.ID })
}

function Remove-TestShadowCopies {
    param(
        [Parameter(Mandatory)][string[]]$BaselineIds,
        [string]$VolumePath
    )

    if ([string]::IsNullOrWhiteSpace($VolumePath)) {
        return
    }
    $owned = @(Get-CimInstance -ClassName Win32_ShadowCopy | Where-Object {
        $_.ID -notin $BaselineIds -and
        [string]::Equals($_.VolumeName, $VolumePath, [StringComparison]::OrdinalIgnoreCase)
    })
    foreach ($shadow in $owned) {
        $result = Invoke-CimMethod -InputObject $shadow -MethodName Delete
        if ($result.ReturnValue -ne 0) {
            Write-Warning "Failed to delete test Shadow Copy $($shadow.ID): $($result.ReturnValue)"
        }
    }
}

function Invoke-Worker {
    param(
        [Parameter(Mandatory)][string]$Executable,
        [Parameter(Mandatory)][string]$RequestJson,
        [Parameter(Mandatory)][int]$TimeoutMilliseconds
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.StandardInputEncoding = [Text.UTF8Encoding]::new($false)
    $startInfo.StandardOutputEncoding = [Text.Encoding]::UTF8
    $startInfo.StandardErrorEncoding = [Text.Encoding]::UTF8

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        Assert-Condition $process.Start() 'Failed to start aegra_personal_worker.'
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $process.StandardInput.Write($RequestJson)
        $process.StandardInput.Close()
        if (-not $process.WaitForExit($TimeoutMilliseconds)) {
            $process.Kill()
            throw "Worker exceeded the $TimeoutMinutes minute timeout."
        }
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Stdout = $stdoutTask.Result
            Stderr = $stderrTask.Result
        }
    }
    finally {
        $process.Dispose()
    }
}

function Get-FileMagic {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][long]$Offset
    )

    $stream = [IO.File]::OpenRead($Path)
    try {
        [void]$stream.Seek($Offset, [IO.SeekOrigin]::Begin)
        $buffer = [byte[]]::new(8)
        Assert-Condition ($stream.Read($buffer, 0, $buffer.Length) -eq $buffer.Length) `
            "File is too short to read magic: $Path"
        return [Text.Encoding]::ASCII.GetString($buffer).TrimEnd([char]0)
    }
    finally {
        $stream.Dispose()
    }
}

function Assert-WorkerResult {
    param(
        [Parameter(Mandatory)]$Invocation,
        [Parameter(Mandatory)][string]$JobId,
        [Parameter(Mandatory)][string]$TraceId,
        [Parameter(Mandatory)][UInt64]$ExpectedLogicalBytes,
        [Parameter(Mandatory)][string]$CredentialTarget
    )

    Assert-Condition ($Invocation.ExitCode -eq 0) `
        "Worker failed with exit code $($Invocation.ExitCode). stderr=$($Invocation.Stderr)"
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($Invocation.Stdout)) `
        'Worker returned an empty response.'
    Assert-Condition (-not $Invocation.Stdout.Contains($CredentialTarget)) `
        'Worker response disclosed the Credential target.'

    $response = $Invocation.Stdout | ConvertFrom-Json
    Assert-Condition ([int]$response.schema_version -eq 1) 'Unexpected WorkerResponse schema.'
    Assert-Condition ([int]$response.kind -eq 1) 'Worker did not return a TaskResult response.'
    Assert-Condition ($response.job_id -eq $JobId -and $response.trace_id -eq $TraceId) `
        'WorkerResponse correlation does not match the Job.'
    Assert-Condition ($null -ne $response.task_result) 'Worker response has no task_result.'
    Assert-Condition ([int]$response.task_result.outcome -eq 1) `
        "Backup did not complete cleanly: $($response.task_result.message_code)"
    Assert-Condition ([int]$response.task_result.error_code -eq 0) `
        'Successful backup returned a nonzero error code.'
    Assert-Condition ($response.task_result.message_code -eq 'backup.completed') `
        "Unexpected completion code: $($response.task_result.message_code)"
    Assert-Condition ([UInt64]$response.task_result.logical_bytes -eq $ExpectedLogicalBytes) `
        'Worker logical byte count does not match the source volume.'
    Assert-Condition ([UInt64]$response.task_result.chunk_count -gt 0) `
        'Worker reported no archive chunks.'
    return $response
}

function Assert-ArchiveArtifacts {
    param([Parameter(Mandatory)][string]$ArchivePath)

    $sidecarPath = "$ArchivePath.bhx"
    Assert-Condition (Test-Path -LiteralPath $ArchivePath -PathType Leaf) 'Archive was not published.'
    Assert-Condition (Test-Path -LiteralPath $sidecarPath -PathType Leaf) 'Sidecar was not published.'
    $archive = Get-Item -LiteralPath $ArchivePath
    $sidecar = Get-Item -LiteralPath $sidecarPath
    Assert-Condition ($archive.Length -gt 768) 'Archive is unexpectedly small.'
    Assert-Condition ($sidecar.Length -gt 96) 'Sidecar is unexpectedly small.'
    Assert-Condition ((Get-FileMagic -Path $ArchivePath -Offset 0) -eq 'MYBACKUP') `
        'Archive Header magic is invalid.'
    Assert-Condition ((Get-FileMagic -Path $ArchivePath -Offset ($archive.Length - 512)) -eq 'MYBKEND') `
        'Archive Footer magic is invalid.'
    Assert-Condition ((Get-FileMagic -Path $sidecarPath -Offset 0) -eq 'MYBKHIDX') `
        'Sidecar Header magic is invalid.'
    Assert-Condition (-not (Test-Path -LiteralPath "$ArchivePath.001")) `
        'Worker unexpectedly produced a split archive.'
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$artifactsRootFull = [IO.Path]::GetFullPath($ArtifactsRoot)
$runDirectory = Join-Path $artifactsRootFull ("windows-personal-backup-{0}" -f [Guid]::NewGuid())
$runDirectory = [IO.Path]::GetFullPath($runDirectory)
$rootPrefix = $artifactsRootFull.TrimEnd('\') + '\'
Assert-Condition $runDirectory.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase) `
    'Generated run directory escaped the artifacts root.'

$preset = if ($Configuration -eq 'Debug') { 'vs2026-debug' } else { 'vs2026-release' }
if ([string]::IsNullOrWhiteSpace($WorkerPath)) {
    $localWorker = Join-Path $PSScriptRoot 'aegra_personal_worker.exe'
    if ($SkipBuild -and (Test-Path -LiteralPath $localWorker -PathType Leaf)) {
        $WorkerPath = $localWorker
    }
    else {
        $WorkerPath = Join-Path $repoRoot "out\build\$preset\src\apps\worker\aegra_personal_worker.exe"
    }
    if (-not $SkipBuild) {
        Write-Step "Building and testing $Configuration with Visual Studio 2026 Insiders"
        $buildScript = Join-Path $repoRoot 'scripts\build.cmd'
        Assert-Condition (Test-Path -LiteralPath $buildScript -PathType Leaf) `
            'Repository build script was not found. Use -SkipBuild with a local Worker or pass -WorkerPath.'
        & $buildScript $Configuration
        Assert-Condition ($LASTEXITCODE -eq 0) 'Aegra build or baseline tests failed.'
    }
}
$WorkerPath = [IO.Path]::GetFullPath($WorkerPath)
Assert-Condition (Test-Path -LiteralPath $WorkerPath -PathType Leaf) `
    "Worker executable does not exist: $WorkerPath"

New-Item -ItemType Directory -Force $runDirectory | Out-Null
$vhdPathSpecified = -not [string]::IsNullOrWhiteSpace($VhdPath)
if ($vhdPathSpecified) {
    $vhdPath = [IO.Path]::GetFullPath($VhdPath)
    Assert-Condition ([string]::Equals(
            [IO.Path]::GetExtension($vhdPath),
            '.vhdx',
            [StringComparison]::OrdinalIgnoreCase)) `
        "VHD path must use the .vhdx extension: $vhdPath"
    Assert-Condition (-not (Test-Path -LiteralPath $vhdPath)) `
        "Refusing to overwrite an existing VHDX: $vhdPath"
    $vhdParent = Split-Path -Parent $vhdPath
    New-Item -ItemType Directory -Force $vhdParent | Out-Null
}
else {
    $vhdPath = Join-Path $runDirectory 'source.vhdx'
}
$archivePath = Join-Path $runDirectory 'single-volume.bkf'
$jobPath = Join-Path $runDirectory 'job.json'
$responsePath = Join-Path $runDirectory 'response.json'
$stderrPath = Join-Path $runDirectory 'worker.stderr.txt'
$diskPartPath = Join-Path $runDirectory 'diskpart.txt'
$driveLetter = Get-FreeDriveLetter
$credentialTarget = "Aegra.E2E.Personal.$([Guid]::NewGuid())"
$credentialCreated = $false
$vhdCreated = $false
$succeeded = $false
$cleanupAllowsRemoval = $true
$testVolumePath = $null
$baselineShadowIds = Get-ShadowCopyIds

if ($null -eq $ArchivePassword) {
    $ArchivePassword = Read-Host 'Enter a temporary archive password' -AsSecureString
}

try {
    Write-Step "Creating isolated $VhdSizeMiB MiB NTFS VHDX at $vhdPath"
    New-TestVolume -VhdPath $vhdPath -DriveLetter $driveLetter -SizeMiB $VhdSizeMiB `
        -DiskPartPath $diskPartPath
    $vhdCreated = $true

    $volumeRoot = "${driveLetter}:\"
    Write-Step "Writing source data to $volumeRoot"
    Write-TestData -VolumeRoot $volumeRoot
    $volumePath = Get-CanonicalVolumePath -DriveLetter $driveLetter
    $testVolumePath = $volumePath
    $volume = Get-Volume -DriveLetter $driveLetter
    $expectedLogicalBytes = [UInt64]$volume.Size

    Initialize-CredentialInterop
    [AegraCredentialNative]::Write($credentialTarget, $ArchivePassword)
    $credentialCreated = $true

    $jobId = "e2e-$([Guid]::NewGuid())"
    $traceId = "trace-$([Guid]::NewGuid())"
    $job = [ordered]@{
        schema_version = 1
        job_id = $jobId
        tenant_id = 'personal-e2e'
        operation = 1
        source_refs = @($volumePath)
        target_ref = $archivePath
        credential_refs = @("wincred://$credentialTarget")
        trace_id = $traceId
        deadline_utc_ms = 0
    }
    $jobJson = $job | ConvertTo-Json -Depth 5 -Compress
    [IO.File]::WriteAllText($jobPath, $jobJson, [Text.UTF8Encoding]::new($false))

    Write-Step "Running the real Worker against $volumePath"
    $invocation = Invoke-Worker -Executable $WorkerPath -RequestJson $jobJson `
        -TimeoutMilliseconds ($TimeoutMinutes * 60 * 1000)
    [IO.File]::WriteAllText($responsePath, $invocation.Stdout, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($stderrPath, $invocation.Stderr, [Text.UTF8Encoding]::new($false))
    $response = Assert-WorkerResult -Invocation $invocation -JobId $jobId -TraceId $traceId `
        -ExpectedLogicalBytes $expectedLogicalBytes -CredentialTarget $credentialTarget
    Assert-ArchiveArtifacts -ArchivePath $archivePath

    $partials = @(Get-ChildItem -LiteralPath $runDirectory -File -Recurse -Filter '*.partial')
    Assert-Condition ($partials.Count -eq 0) 'Backup left partial files behind.'
    $residualShadows = @(Get-CimInstance -ClassName Win32_ShadowCopy | Where-Object {
        $_.ID -notin $baselineShadowIds -and
        [string]::Equals($_.VolumeName, $testVolumePath, [StringComparison]::OrdinalIgnoreCase)
    })
    Assert-Condition ($residualShadows.Count -eq 0) `
        "Backup left test-volume VSS snapshots behind: $(($residualShadows.ID) -join ', ')"

    $summary = [ordered]@{
        status = 'passed'
        worker = $WorkerPath
        source_volume = $volumePath
        logical_bytes = [UInt64]$response.task_result.logical_bytes
        stored_bytes = [UInt64]$response.task_result.stored_bytes
        chunk_count = [UInt64]$response.task_result.chunk_count
        archive_bytes = (Get-Item -LiteralPath $archivePath).Length
        sidecar_bytes = (Get-Item -LiteralPath "$archivePath.bhx").Length
        source_vhdx = $vhdPath
        artifacts = $runDirectory
    }
    Write-Host ($summary | ConvertTo-Json -Depth 3)
    $succeeded = $true
}
finally {
    if ($credentialCreated) {
        try {
            [AegraCredentialNative]::Delete($credentialTarget)
        }
        catch {
            Write-Warning "Failed to delete temporary Credential target: $($_.Exception.Message)"
        }
    }
    try {
        Remove-TestShadowCopies -BaselineIds $baselineShadowIds -VolumePath $testVolumePath
    }
    catch {
        $cleanupAllowsRemoval = $false
        Write-Warning "Failed to clean test Shadow Copies: $($_.Exception.Message)"
    }
    if ($vhdCreated -or (Test-Path -LiteralPath $vhdPath)) {
        try {
            Dismount-TestVolume -VhdPath $vhdPath -DriveLetter $driveLetter `
                -DiskPartPath $diskPartPath
        }
        catch {
            $cleanupAllowsRemoval = $false
            Write-Warning "Failed to detach the test VHDX: $($_.Exception.Message)"
        }
    }
    if ($succeeded -and -not $KeepArtifacts -and $cleanupAllowsRemoval) {
        if (Test-Path -LiteralPath $vhdPath) {
            Remove-Item -LiteralPath $vhdPath -Force
        }
        Remove-Item -LiteralPath $runDirectory -Recurse -Force
    }
    elseif (Test-Path -LiteralPath $runDirectory) {
        Write-Host "Artifacts retained at: $runDirectory"
        if ($vhdPathSpecified -and (Test-Path -LiteralPath $vhdPath)) {
            Write-Host "VHDX retained at: $vhdPath"
        }
    }
}
