# Aegra Image Installer (WiX)

Brand **Aegra** · Product **Aegra Image** (`docs/branding/BRAND.md`).

Produces:

| Output | Description |
|--------|-------------|
| `out\AegraImage.msi` | Product package (embedded in Setup; hidden from Apps list when installed via bundle) |
| `out\AegraImageSetup.exe` | Single-file setup bootstrapper (**recommended**; UI language follows OS) |

The bundle also embeds the official `Dokan_x64.msi` from the repository root (Dokan Library 2.3.1.1000 x64).

After setup, **Apps & Features** shows:

| Entry | Notes |
|-------|--------|
| **AegraImage** | Main product (bundle). Uninstall removes product only. |
| **Dokan Library 2.3.1.1000 (x64)** | Official Dokan MSI, chained as permanent. **Not** removed with the product; uninstall manually if desired. |

## Prerequisites

1. Desktop CMake tree already configured (Qt Creator Release, or equivalent):
   `build\Desktop_Qt_6_8_3_MSVC2022_64bit-Release`
2. Install WiX Toolset CLI v7:
   ```bat
   winget install WiXToolset.WiXCLI
   wix eula accept wix7
   ```
3. Official Dokan installer at repo root: `Dokan_x64.msi`.
4. Canonical payload at `installer\payload\` (Qt runtime + product layout). `build.ps1` does **not** regenerate Qt files.

## Build installer

`build.ps1` compiles the product, copies new binaries into `payload\`, checks the payload file list, then builds WiX. MSI/Setup version defaults to `include/aegra_version.h` (`Major.Minor.Patch.Build`). The same header fills Windows File Properties on every product EXE/DLL. After a successful package, the script increments `AEGRI_VERSION_BUILD` in that header for the next build.

```powershell
cd installer
.\build.ps1
.\build.ps1 -Version 0.1.0
.\build.ps1 -SkipCompile
.\build.ps1 -MsiOnly
```

Compile steps (unless `-SkipCompile`):

1. Delete previous project `exe` / `dll` under `out\build\vs2026-release`, the Desktop CMake tree, and `payload\`.
2. `scripts\build.cmd Release` (Service / Worker / PE / Shell).
3. `cmake --build` target `aegra_desktop` in the Desktop CMake tree.
4. Copy those outputs into `payload\`.
5. Fail if any file from the payload list is missing.

| Input | Default |
|-------|---------|
| Service / worker / PE / CRT | `out\build\vs2026-release\src\apps\service` |
| CLI | `out\build\vs2026-release\src\apps\cli` |
| Shell extension | `out\build\vs2026-release\src\apps\shell_extension` |
| Desktop | `build\Desktop_Qt_6_8_3_MSVC2022_64bit-Release\src\apps\desktop` |
| Payload baseline | `installer\payload\` |
| Dokan kernel MSI | `Dokan_x64.msi` (repo root) |

## What gets installed

Default install directory: `C:\Program Files\AegraImage\`

All binaries live in that folder (no `ui\` subdirectory):

| File | Role |
|------|------|
| `AegraImage.exe` | Desktop GUI (+ Qt runtime: `qml\`, `platforms\`, `imageformats\`, `tls\`) |
| `AegraCLI.exe` | Local Service control CLI |
| `AegraService.exe` | Windows service **AegraService** (`--service`) |
| `AegraWorker.exe` | Backup / restore / mount worker |
| `AegraPEResore.exe` | WinPE offline restore executor (injected into boot.wim) |
| `aegra_shell_extension.dll` | Explorer `.bkf` namespace (HKLM COM + synchronous Shell refresh after registry authoring) |
| `dokan2.dll`, `libsodium.dll`, `zstd.dll`, `sqlite3.dll` | Runtime |
| VC++ CRT (`msvcp140*.dll`, `vcruntime140*.dll`) | WinPE payload + host |

Runtime data lives under **`%ProgramData%\Aegra`** (Service default), not Program Files.

### Windows service

| Field | Value |
|-------|--------|
| Service name | `AegraService` |
| Display name | Aegra Management Service |
| Start | Automatic |
| Account | LocalSystem |
| Arguments | `--service` |
| Recovery | Restart the service on first, second, and subsequent failures |
| Reset fail count after | 0 days |
| Restart service after | 0 minutes |

### Mount (Dokan)

User-mode `dokan2.dll` ships in the product MSI. Kernel driver comes from the official `Dokan_x64.msi` (permanent).

## Install / uninstall

```bat
:: Install (Admin)
out\AegraImageSetup.exe

:: Silent MSI
msiexec /i out\AegraImage.msi /qn

:: Uninstall
msiexec /x out\AegraImage.msi /qn
```

Uninstall stops and removes the service and shell extension. **`%ProgramData%\Aegra` is kept by default.** Official Dokan remains installed.

Setup.exe Modify → checkbox **Also delete application data (ProgramData\Aegra)**. Silent wipe:

```bat
AegraImageSetup.exe /uninstall /quiet DELETEDATA=1
```

Installation requires administrator (service + shell registration).
