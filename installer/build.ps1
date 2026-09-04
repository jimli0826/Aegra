#Requires -Version 5.1
<#
.SYNOPSIS
  Compile Aegra Release outputs into installer\payload and build the WiX package.

  payload\ is the canonical layout. This script keeps a file list taken from that
  tree and fails if any listed file is missing after compile and copy.

.EXAMPLE
  .\build.ps1
  .\build.ps1 -Version 0.1.0
  .\build.ps1 -SkipCompile
  .\build.ps1 -MsiOnly
#>
param(
    [string]$Version = "",
    [switch]$SkipCompile,
    [switch]$MsiOnly
)

$ErrorActionPreference = "Stop"
$InstallerRoot = $PSScriptRoot
$RepoRoot = (Resolve-Path (Join-Path $InstallerRoot "..")).Path
$OutDir = Join-Path $InstallerRoot "out"
$Payload = Join-Path $InstallerRoot "payload"
$DokanOfficialMsi = Join-Path $RepoRoot "Dokan_x64.msi"
$VsRoot = "C:\Program Files\Microsoft Visual Studio\18\Insiders"
$VsDevCmd = Join-Path $VsRoot "Common7\Tools\VsDevCmd.bat"
$CmakeExe = Join-Path $VsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$QtRoot = "C:\Qt6\6.8.3\msvc2022_64"
$ReleaseBuildDir = Join-Path $RepoRoot "out\build\vs2026-release"
$ServiceDir = Join-Path $RepoRoot "out\build\vs2026-release\src\apps\service"
$CliDir = Join-Path $RepoRoot "out\build\vs2026-release\src\apps\cli"
$ShellDir = Join-Path $RepoRoot "out\build\vs2026-release\src\apps\shell_extension"
$GuiDir = Join-Path $ReleaseBuildDir "src\apps\desktop"

$env:Path = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
            [System.Environment]::GetEnvironmentVariable("Path", "User")

$script:ProjectBinaries = @(
    "AegraCLI.exe",
    "AegraImage.exe",
    "AegraPEResore.exe",
    "AegraBootCheck.exe",
    "AegraWorker.exe",
    "AegraService.exe",
    "aegra_shell_extension.dll",
    # Remove stale outputs produced before the executable rename.
    "AegraBootCheckHost.exe",
    "aegra_cli.exe",
    "aegra_desktop.exe",
    "aegra_pe_restore.exe",
    "aegra_personal_worker.exe",
    "aegra_service.exe"
)

function Get-PayloadFileList {
    return @(
        "AegraCLI.exe",
        "AegraImage.exe",
        "AegraPEResore.exe",
        "AegraBootCheck.exe",
        "AegraWorker.exe",
        "AegraService.exe",
        "aegra_shell_extension.dll",
        "concrt140.dll",
        "D3Dcompiler_47.dll",
        "dokan2.dll",
        "libsodium.dll",
        "msvcp140.dll",
        "msvcp140_1.dll",
        "msvcp140_2.dll",
        "msvcp140_atomic_wait.dll",
        "msvcp140_codecvt_ids.dll",
        "sqlite3.dll",
        "vcruntime140.dll",
        "vcruntime140_1.dll",
        "zstd.dll",
        "Qt6Core.dll",
        "Qt6Gui.dll",
        "Qt6Network.dll",
        "Qt6OpenGL.dll",
        "Qt6Qml.dll",
        "Qt6QmlMeta.dll",
        "Qt6QmlModels.dll",
        "Qt6QmlWorkerScript.dll",
        "Qt6Quick.dll",
        "Qt6QuickControls2.dll",
        "Qt6QuickControls2Basic.dll",
        "Qt6QuickControls2BasicStyleImpl.dll",
        "Qt6QuickControls2Impl.dll",
        "Qt6QuickLayouts.dll",
        "Qt6QuickTemplates2.dll",
        "imageformats\qico.dll",
        "platforms\qwindows.dll",
        "tls\qschannelbackend.dll",
        "qml\QML\plugins.qmltypes",
        "qml\QML\qmldir",
        "qml\QtQml\plugins.qmltypes",
        "qml\QtQml\qmldir",
        "qml\QtQml\qmlplugin.dll",
        "qml\QtQml\Models\modelsplugin.dll",
        "qml\QtQml\Models\plugins.qmltypes",
        "qml\QtQml\Models\qmldir",
        "qml\QtQml\WorkerScript\plugins.qmltypes",
        "qml\QtQml\WorkerScript\qmldir",
        "qml\QtQml\WorkerScript\workerscriptplugin.dll",
        "qml\QtQuick\plugins.qmltypes",
        "qml\QtQuick\qmldir",
        "qml\QtQuick\qtquick2plugin.dll",
        "qml\QtQuick\Controls\plugins.qmltypes",
        "qml\QtQuick\Controls\qmldir",
        "qml\QtQuick\Controls\qtquickcontrols2plugin.dll",
        "qml\QtQuick\Controls\Basic\AbstractButton.qml",
        "qml\QtQuick\Controls\Basic\Action.qml",
        "qml\QtQuick\Controls\Basic\ActionGroup.qml",
        "qml\QtQuick\Controls\Basic\ApplicationWindow.qml",
        "qml\QtQuick\Controls\Basic\BusyIndicator.qml",
        "qml\QtQuick\Controls\Basic\Button.qml",
        "qml\QtQuick\Controls\Basic\ButtonGroup.qml",
        "qml\QtQuick\Controls\Basic\Calendar.qml",
        "qml\QtQuick\Controls\Basic\CalendarModel.qml",
        "qml\QtQuick\Controls\Basic\CheckBox.qml",
        "qml\QtQuick\Controls\Basic\CheckDelegate.qml",
        "qml\QtQuick\Controls\Basic\ComboBox.qml",
        "qml\QtQuick\Controls\Basic\Container.qml",
        "qml\QtQuick\Controls\Basic\Control.qml",
        "qml\QtQuick\Controls\Basic\DayOfWeekRow.qml",
        "qml\QtQuick\Controls\Basic\DelayButton.qml",
        "qml\QtQuick\Controls\Basic\Dial.qml",
        "qml\QtQuick\Controls\Basic\Dialog.qml",
        "qml\QtQuick\Controls\Basic\DialogButtonBox.qml",
        "qml\QtQuick\Controls\Basic\Drawer.qml",
        "qml\QtQuick\Controls\Basic\Frame.qml",
        "qml\QtQuick\Controls\Basic\GroupBox.qml",
        "qml\QtQuick\Controls\Basic\HorizontalHeaderView.qml",
        "qml\QtQuick\Controls\Basic\ItemDelegate.qml",
        "qml\QtQuick\Controls\Basic\Label.qml",
        "qml\QtQuick\Controls\Basic\Menu.qml",
        "qml\QtQuick\Controls\Basic\MenuBar.qml",
        "qml\QtQuick\Controls\Basic\MenuBarItem.qml",
        "qml\QtQuick\Controls\Basic\MenuItem.qml",
        "qml\QtQuick\Controls\Basic\MenuSeparator.qml",
        "qml\QtQuick\Controls\Basic\MonthGrid.qml",
        "qml\QtQuick\Controls\Basic\Page.qml",
        "qml\QtQuick\Controls\Basic\PageIndicator.qml",
        "qml\QtQuick\Controls\Basic\Pane.qml",
        "qml\QtQuick\Controls\Basic\plugins.qmltypes",
        "qml\QtQuick\Controls\Basic\Popup.qml",
        "qml\QtQuick\Controls\Basic\ProgressBar.qml",
        "qml\QtQuick\Controls\Basic\qmldir",
        "qml\QtQuick\Controls\Basic\qtquickcontrols2basicstyleplugin.dll",
        "qml\QtQuick\Controls\Basic\RadioButton.qml",
        "qml\QtQuick\Controls\Basic\RadioDelegate.qml",
        "qml\QtQuick\Controls\Basic\RangeSlider.qml",
        "qml\QtQuick\Controls\Basic\RoundButton.qml",
        "qml\QtQuick\Controls\Basic\ScrollBar.qml",
        "qml\QtQuick\Controls\Basic\ScrollIndicator.qml",
        "qml\QtQuick\Controls\Basic\ScrollView.qml",
        "qml\QtQuick\Controls\Basic\SelectionRectangle.qml",
        "qml\QtQuick\Controls\Basic\Slider.qml",
        "qml\QtQuick\Controls\Basic\SpinBox.qml",
        "qml\QtQuick\Controls\Basic\SplitView.qml",
        "qml\QtQuick\Controls\Basic\StackView.qml",
        "qml\QtQuick\Controls\Basic\SwipeDelegate.qml",
        "qml\QtQuick\Controls\Basic\SwipeView.qml",
        "qml\QtQuick\Controls\Basic\Switch.qml",
        "qml\QtQuick\Controls\Basic\SwitchDelegate.qml",
        "qml\QtQuick\Controls\Basic\TabBar.qml",
        "qml\QtQuick\Controls\Basic\TabButton.qml",
        "qml\QtQuick\Controls\Basic\TextArea.qml",
        "qml\QtQuick\Controls\Basic\TextField.qml",
        "qml\QtQuick\Controls\Basic\ToolBar.qml",
        "qml\QtQuick\Controls\Basic\ToolButton.qml",
        "qml\QtQuick\Controls\Basic\ToolSeparator.qml",
        "qml\QtQuick\Controls\Basic\ToolTip.qml",
        "qml\QtQuick\Controls\Basic\TreeViewDelegate.qml",
        "qml\QtQuick\Controls\Basic\Tumbler.qml",
        "qml\QtQuick\Controls\Basic\VerticalHeaderView.qml",
        "qml\QtQuick\Controls\Basic\WeekNumberColumn.qml",
        "qml\QtQuick\Controls\Basic\impl\plugins.qmltypes",
        "qml\QtQuick\Controls\Basic\impl\qmldir",
        "qml\QtQuick\Controls\Basic\impl\qtquickcontrols2basicstyleimplplugin.dll",
        "qml\QtQuick\Controls\impl\plugins.qmltypes",
        "qml\QtQuick\Controls\impl\qmldir",
        "qml\QtQuick\Controls\impl\qtquickcontrols2implplugin.dll",
        "qml\QtQuick\Layouts\plugins.qmltypes",
        "qml\QtQuick\Layouts\qmldir",
        "qml\QtQuick\Layouts\qquicklayoutsplugin.dll",
        "qml\QtQuick\Templates\plugins.qmltypes",
        "qml\QtQuick\Templates\qmldir",
        "qml\QtQuick\Templates\qtquicktemplates2plugin.dll",
        "qml\QtQuick\Window\qmldir",
        "qml\QtQuick\Window\quickwindow.qmltypes",
        "qml\QtQuick\Window\quickwindowplugin.dll"
    )
}

function Assert-FilesExist([string]$root, [string[]]$relativePaths, [string]$label) {
    $missing = @()
    foreach ($rel in $relativePaths) {
        $path = Join-Path $root $rel
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            $missing += $rel
        }
    }
    if ($missing.Count -gt 0) {
        $shown = $missing -join "`n  "
        throw "$label missing $($missing.Count) file(s):`n  $shown"
    }
}

function Remove-ProjectBinaries([string[]]$roots) {
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        foreach ($name in $script:ProjectBinaries) {
            Get-ChildItem -Path $root -Recurse -Filter $name -File -ErrorAction SilentlyContinue |
                ForEach-Object {
                    Write-Host "  Removing $($_.FullName)"
                    Remove-Item -LiteralPath $_.FullName -Force
                }
        }
    }
}

function Invoke-VsDevCommand([string]$inner) {
    if (-not (Test-Path $VsDevCmd)) { throw "Missing VsDevCmd: $VsDevCmd" }
    $bat = Join-Path $env:TEMP "aegra-installer-build.cmd"
    @(
        "@echo off",
        "call `"$VsDevCmd`" -no_logo -arch=x64 -host_arch=x64",
        "if errorlevel 1 exit /b %errorlevel%",
        $inner,
        "exit /b %errorlevel%"
    ) | Set-Content -Path $bat -Encoding ASCII
    try {
        & cmd.exe /c $bat
        if ($LASTEXITCODE -ne 0) {
            throw "Command failed with exit $LASTEXITCODE"
        }
    } finally {
        Remove-Item $bat -Force -ErrorAction SilentlyContinue
    }
}

function Invoke-ProductCompile {
    $buildCmd = Join-Path $RepoRoot "scripts\build.cmd"
    if (-not (Test-Path $buildCmd)) { throw "Missing $buildCmd" }
    Write-Host "`nCompiling vs2026-release..." -ForegroundColor Yellow
    Push-Location $RepoRoot
    try {
        & cmd.exe /c "`"$buildCmd`" Release"
        if ($LASTEXITCODE -ne 0) {
            throw "scripts\build.cmd Release failed with exit $LASTEXITCODE"
        }
    } finally {
        Pop-Location
    }

    if (-not (Test-Path $CmakeExe)) { throw "Missing cmake: $CmakeExe" }
    if (-not (Test-Path (Join-Path $QtRoot "lib\cmake\Qt6\Qt6Config.cmake"))) {
        throw "Missing Qt 6.8.3 CMake package: $QtRoot"
    }

    Write-Host "`nConfiguring aegra_desktop in vs2026-release..." -ForegroundColor Yellow
    Push-Location $RepoRoot
    try {
        Invoke-VsDevCommand "`"$CmakeExe`" --preset vs2026-release --no-warn-unused-cli -DAEGRA_QT_ROOT=`"$QtRoot`""
    } finally {
        Pop-Location
    }

    if (-not (Test-Path (Join-Path $ReleaseBuildDir "CMakeCache.txt"))) {
        throw "Release CMake tree missing after configuration: $ReleaseBuildDir"
    }
    Write-Host "`nCompiling aegra_desktop..." -ForegroundColor Yellow
    Invoke-VsDevCommand "`"$CmakeExe`" --build `"$ReleaseBuildDir`" --target aegra_desktop --parallel"
}

function Copy-Required([string]$source, [string]$destinationDir) {
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Required compiled file missing: $source"
    }
    New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null
    Copy-Item -LiteralPath $source -Destination $destinationDir -Force
    Write-Host "  Copied $(Split-Path $source -Leaf)"
}

function Copy-IfExists([string]$source, [string]$destinationDir) {
    if (Test-Path -LiteralPath $source -PathType Leaf) {
        Copy-Item -LiteralPath $source -Destination $destinationDir -Force
        Write-Host "  Copied $(Split-Path $source -Leaf)"
    }
}

function Copy-ProjectOutputsToPayload {
    Write-Host "`nCopying compiled outputs into payload..." -ForegroundColor Yellow
    Copy-Required (Join-Path $ServiceDir "AegraService.exe") $Payload
    Copy-Required (Join-Path $CliDir "AegraCLI.exe") $Payload
    Copy-Required (Join-Path $ServiceDir "AegraWorker.exe") $Payload
    Copy-Required (Join-Path $ServiceDir "AegraBootCheck.exe") $Payload
    Copy-Required (Join-Path $ServiceDir "AegraPEResore.exe") $Payload
    Copy-Required (Join-Path $ShellDir "aegra_shell_extension.dll") $Payload
    Copy-Required (Join-Path $GuiDir "AegraImage.exe") $Payload

    $companion = @(
        "dokan2.dll",
        "libsodium.dll",
        "zstd.dll",
        "sqlite3.dll",
        "concrt140.dll",
        "msvcp140.dll",
        "msvcp140_1.dll",
        "msvcp140_2.dll",
        "msvcp140_atomic_wait.dll",
        "msvcp140_codecvt_ids.dll",
        "vcruntime140.dll",
        "vcruntime140_1.dll"
    )
    foreach ($name in $companion) {
        Copy-IfExists (Join-Path $ServiceDir $name) $Payload
    }
}

function Get-AegraVersionFromHeader {
    $hdr = Join-Path $RepoRoot "include\aegra_version.h"
    if (-not (Test-Path $hdr)) { throw "Missing version header: $hdr" }
    $t = Get-Content $hdr -Raw
    $m = [regex]::Match($t, 'AEGRI_VERSION_MAJOR\s+(\d+)')
    $n = [regex]::Match($t, 'AEGRI_VERSION_MINOR\s+(\d+)')
    $p = [regex]::Match($t, 'AEGRI_VERSION_PATCH\s+(\d+)')
    $b = [regex]::Match($t, 'AEGRI_VERSION_BUILD\s+(\d+)')
    if (-not ($m.Success -and $n.Success -and $p.Success -and $b.Success)) {
        throw "Could not parse version macros from $hdr"
    }
    return "$($m.Groups[1].Value).$($n.Groups[1].Value).$($p.Groups[1].Value).$($b.Groups[1].Value)"
}

function Update-AegraBuildNumber {
    $hdr = Join-Path $RepoRoot "include\aegra_version.h"
    if (-not (Test-Path $hdr)) { throw "Missing version header: $hdr" }
    $t = Get-Content $hdr -Raw
    $m = [regex]::Match($t, '(#define\s+AEGRI_VERSION_BUILD\s+)(\d+)')
    if (-not $m.Success) {
        throw "Could not find AEGRI_VERSION_BUILD in $hdr"
    }
    $next = [int]$m.Groups[2].Value + 1
    $updated = $t.Substring(0, $m.Index) + $m.Groups[1].Value + "$next" +
        $t.Substring($m.Index + $m.Length)
    $utf8 = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($hdr, $updated, $utf8)
    Write-Host "AEGRI_VERSION_BUILD $($m.Groups[2].Value) -> $next"
}

function Require-Command([string]$name) {
    if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
        throw "Required command not found: $name."
    }
}

function Initialize-Wix {
    Require-Command "wix"
    try { & wix eula accept wix7 2>$null } catch {}
    $exts = & wix extension list 2>$null
    if ($exts -notmatch "BootstrapperApplications") {
        Write-Host "Adding WiX BootstrapperApplications extension..."
        & wix extension add WixToolset.BootstrapperApplications.wixext | Out-Null
    }
    if ($exts -notmatch "Util") {
        Write-Host "Adding WiX Util extension..."
        & wix extension add WixToolset.Util.wixext | Out-Null
    }
}

function Invoke-WixPackage {
    Initialize-Wix
    if (Test-Path $OutDir) {
        Get-ChildItem -Path $OutDir -Recurse -ErrorAction SilentlyContinue | Remove-Item -Force -Recurse
    }
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

    $dokanMsiOut = Join-Path $OutDir "Dokan_x64.msi"
    Copy-Item $DokanOfficialMsi $dokanMsiOut -Force
    Write-Host "Copied official Dokan MSI to out\Dokan_x64.msi"

    $packageWxs = Join-Path $InstallerRoot "Package.wxs"
    $bundleWxs = Join-Path $InstallerRoot "Bundle.wxs"
    $msiOut = Join-Path $OutDir "AegraImage.msi"
    $exeOut = Join-Path $OutDir "AegraImageSetup.exe"

    Write-Host "`nBuilding product MSI..." -ForegroundColor Yellow
    & wix build -o $msiOut -arch x64 -d "ProductVersion=$Version" `
        -bindpath "Payload=$Payload" `
        -ext WixToolset.Util.wixext $packageWxs
    if ($LASTEXITCODE -ne 0) { throw "wix build product MSI failed with exit $LASTEXITCODE" }

    if (-not $MsiOnly) {
        Write-Host "Building Setup EXE..." -ForegroundColor Yellow
        Push-Location $InstallerRoot
        try {
            & wix build -o $exeOut -arch x64 -d "ProductVersion=$Version" `
                -ext WixToolset.BootstrapperApplications.wixext `
                -ext WixToolset.Util.wixext $bundleWxs
            if ($LASTEXITCODE -ne 0) { throw "wix build Bundle failed with exit $LASTEXITCODE" }
        } finally {
            Pop-Location
        }
    }

    Write-Host "`n=== Build succeeded ===" -ForegroundColor Green
    Get-ChildItem $OutDir -File | Format-Table Name, Length, LastWriteTime -AutoSize
    Update-AegraBuildNumber
}

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = Get-AegraVersionFromHeader
}
if ($Version -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') {
    throw "Version must look like major.minor.patch[.build] (got: $Version)"
}
if (($Version.ToCharArray() | Where-Object { $_ -eq '.' }).Count -eq 2) {
    $Version = "$Version.0"
}

if (-not (Test-Path $Payload)) {
    throw "Missing payload baseline directory: $Payload"
}
if (-not (Test-Path $DokanOfficialMsi)) {
    throw "Missing official Dokan MSI: $DokanOfficialMsi"
}

$payloadFiles = Get-PayloadFileList
Write-Host "=== Aegra Image Installer ===" -ForegroundColor Cyan
Write-Host "Version: $Version"
Write-Host "Payload: $Payload ($($payloadFiles.Count) required files)"
Write-Host "Dokan:   $DokanOfficialMsi"

if (-not $SkipCompile) {
    Write-Host "`nDeleting previous project exe/dll outputs..." -ForegroundColor Yellow
    Remove-ProjectBinaries @(
        (Join-Path $ReleaseBuildDir "src\apps"),
        $Payload
    )
    Invoke-ProductCompile
    Copy-ProjectOutputsToPayload
}

Write-Host "`nVerifying payload file list..." -ForegroundColor Yellow
Assert-FilesExist $Payload $payloadFiles "Payload"
Write-Host "  $($payloadFiles.Count) files present."

Invoke-WixPackage
