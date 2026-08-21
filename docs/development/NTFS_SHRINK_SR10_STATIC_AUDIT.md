# NTFS Shrink Restore — SR10 Static / Architecture Audit

| Attribute | Value |
| --- | --- |
| Date | 2026-08-21 |
| Scope | Development plan §19.2 dependency and static audit for NTFS smaller-target restore (SR9 surfaces) |
| Mode | Read-only code/CMake inspection; this report file only |
| Capability | Debug/Release declare `restore.ntfs_shrink.v1` by the 2026-08-21 product decision |
| Manual matrix | **M01–M26 are not claimed passed** |

This audit does **not** claim SR10 DoD or release-gate completion. Capability visibility is no longer evidence of release qualification.

**Build companion (2026-08-21):** `scripts\build.cmd Debug` and `scripts\build.cmd Release` both exited 0 with `-- Aegra source file size limits passed`.

**Release runtime probe (2026-08-21):** an isolated `--once` Release Service returned
`restore.ntfs_shrink.v1` in GetServiceInfo (`NtfsShrinkCapability=True`, 20 capabilities).

---

## Checklist results

### 1. NtfsCore / NtfsResize purity — **PASS**

**Includes (grep):** no hits in `src/ntfs_core` or `src/ntfs_resize` for:

`Windows.h` / `winnt` / `Qt` / `personal_archive` / `apps/service` / `apps/worker` / `CreateFile` / `HWND` / `QString`.

**CMake link deps:**

| Target | File | Links |
| --- | --- | --- |
| `aegra_ntfs_core` | `D:\Work\OpenSource\Aegra\src\ntfs_core\CMakeLists.txt` | `Aegra::Base`, `Aegra::Ports` only |
| `aegra_ntfs_resize` | `D:\Work\OpenSource\Aegra\src\ntfs_resize\CMakeLists.txt` | `Aegra::Base`, `Aegra::Ports`, `Aegra::NtfsCore` only |

Does **not** link Windows adapters, Qt, Archive, Service, Worker, or Shell.

Matches `docs/architecture/MODULAR_ARCHITECTURE.md` (`ntfs_core` / `ntfs_resize` rules).

---

### 2. Shell Extension isolation — **PASS**

**CMake:** `D:\Work\OpenSource\Aegra\src\apps\shell_extension\CMakeLists.txt` links `Aegra::AdapterNtfs` (and Archive/storage peers) but **does not** link `Aegra::NtfsResize` or `Aegra::NtfsCore` directly.

**Sources:** no `ntfs_resize` / `NtfsResize` / `ntfs_core` / `NtfsCore` includes or references under `src/apps/shell_extension`.

**Note:** `Aegra::AdapterNtfs` PUBLIC-links `Aegra::NtfsCore` for read-only Explorer use. Architecture explicitly allows `adapters/ntfs → ntfs_core` and forbids Shell→NtfsResize; this audit treats that as compliant with plan §19.2 (“Shell Extension 未链接 NtfsResize”).

---

### 3. Adapter isolation (spot-check) — **PASS** (with residual)

Spot-check of `#include "aegra/adapters/..."` among:

| Module | Cross-peer concrete adapter includes |
| --- | --- |
| `src/adapters/ntfs` | Self only |
| `src/adapters/windows_disk` | Self only |
| `src/adapters/storage_local` | Self only |
| `src/adapters/personal_archive` | Also includes `aegra/adapters/compression_zstd/*` and `aegra/adapters/crypto_sodium/*`; CMake PRIVATE-links those adapters |

**Shrink-introduced path:** no new cross-includes among ntfs / windows_disk / storage_local for shrink.

**Residual (pre-existing):** `personal_archive` depends on concrete codec adapters (`compression_zstd`, `crypto_sodium`). Architecture text says adapters must not depend on another adapter’s implementation. This coupling predates SR9 shrink work; not treated as a shrink-gate FAIL here, but remains an open architecture debt if CI ever hard-enforces adapter→adapter bans.

---

### 4. Win32 file I/O (`std::fstream` family) — **PASS** (remediated 2026-08-21)

Grep for `std::ifstream` / `std::ofstream` / `std::fstream` / `std::filebuf` / `#include <fstream>`:

| Path | Result |
| --- | --- |
| `src/ntfs_core` | Clean |
| `src/ntfs_resize` | Clean |
| `src/adapters/windows_disk` | Clean |
| `src/adapters/storage_local` | Clean |
| `src/adapters/windows_filesystem` | Clean |
| `src/apps/service` | Clean |
| `src/apps/worker` shrink + `windows_personal_backup.cpp` | Clean |
| `src/adapters/personal_archive` | Clean — replaced with `Win32InputFile` (RAII `CreateFileW`/`ReadFile`) |

**Remediation:** added `win32_input_file.{h,cpp}`; migrated preamble/reader/sidecar/file-index/session paths; worker backup uses a local `ScopedInputFile`. Debug rebuild of affected production Targets passed after the change.

---

### 5. Ownership / globals (SR9 shrink surfaces) — **PASS**

Scoped grep on:

- `src/ntfs_resize/**`
- `src/apps/worker/src/personal_archive_restore_shrink.cpp`
- `src/apps/service/src/worker_job_service_shrink_analyze.cpp`

for `new` / `delete` / `std::jthread` / `.detach(`:

- No owning `new`/`delete`, `std::jthread`, or `detach(` usage.
- Matches for `new` were comment/prose only (“new boundary”, “new runs”, …).
- Shrink paths use `std::unique_ptr` / RAII adapters; no suspicious global mutable owners spotted in this scoped scan.

(Not an exhaustive whole-repo ownership audit.)

---

### 6. Source size (1500 physical lines) — **PASS**

Physical line counts (PowerShell `Get-Content -ReadCount 0`).Count on `ntfs_resize` `*.cpp`, worker shrink, and service shrink analyze:

| File | Physical lines |
| --- | --- |
| `src/ntfs_resize/src/shrink_plan_payload.cpp` | 585 (largest in ntfs_resize) |
| `src/apps/worker/src/personal_archive_restore_shrink.cpp` | 536 |
| `src/ntfs_resize/src/ntfs_precommit_auditor.cpp` | 345 |
| `src/apps/service/src/worker_job_service_shrink_analyze.cpp` | 322 |
| All other listed shrink `.cpp` | ≤ 304 |

**None exceed 1500 physical lines.**

**Spot-check (style, not 1500 Fail):** `run_shrink_volume_restore` in `personal_archive_restore_shrink.cpp` (~lines 429–534, ~105 physical lines) exceeds the usual 80-logical-line guideline. Recommend a later non-behavior split; not a §19.2 1500-line violation.

---

### 7. Capability gate — **PASS（Host-gated / Debug and Release enabled）**

**Runtime capability:**

`D:\Work\OpenSource\Aegra\src\apps\service\src\service_main.cpp` — `runtime_capabilities()` adds
`restore.ntfs_shrink.v1` in Debug and Release. The Host still maps Analyze to this capability and rejects any
custom runtime that omits it.

**Analyze maps to the gated capability:**

`D:\Work\OpenSource\Aegra\src\apps\service\src\service_host.cpp`:

- `required_capability(ServiceRequestKind::kAnalyzeNtfsShrink)` → `"restore.ntfs_shrink.v1"`
- `handle_service_request` calls `capability_enabled` before dispatch; undeclared capability → `capability_unavailable`

**Product comments consistent:** Desktop reads the capability string only when Service advertises it
(`service_client.cpp`). Debug/Release use the same exact Analyze/Start path. M01–M26 still govern release qualification.

**This audit does not claim release eligibility.**

---

### 8. Legacy / tests — **PASS**

- No `add_test` / `enable_testing` / CTest / `aegra_*_test` registrations found in repo `CMakeLists.txt` for shrink (or generally in this scan).
- No new test executables or fixtures added for shrink under `src/ntfs_resize`.
- Shrink path greps for legacy/compat/fallback/migrate aliases: no format-fallback aliases; only CMake `ALIAS` targets and “bitmap compatibility” audit naming.

Aligns with repository testing policy and “no legacy format fallback” product rule.

---

### 9. Logging secrets — **PASS**

Shrink/analyze paths:

- `worker_job_service_shrink_analyze.cpp` assigns `layer.password = archive_password` for archive open only; no logging of password material observed.
- `personal_archive_restore_shrink.cpp` uses `WorkerTaskLog` / `ScopedStage` for stages, paths, sizes, booleans — not passwords. `worker_task_log.h` documents: “File-only; never writes secrets or credential material.”
- Service JSON log formatting redacts keys containing `password` / `secret` / `token` / `credential` / … (`service_log_formatter.cpp` `is_sensitive_key`).

No evidence of logging MFT payload contents or user file bytes in these shrink stage notes.

---

## Summary table

| # | Item | Result |
| ---: | --- | --- |
| 1 | NtfsCore / NtfsResize purity | **PASS** |
| 2 | Shell Extension isolation | **PASS** |
| 3 | Adapter isolation (spot-check) | **PASS** (residual: personal_archive→crypto/zstd) |
| 4 | Win32 file I/O (no iostream file streams) | **PASS** (personal_archive + worker backup remediated 2026-08-21) |
| 5 | Ownership / globals (scoped) | **PASS** |
| 6 | Source size ≤ 1500 lines | **PASS** |
| 7 | Capability gate | **PASS** (Debug/Release enabled; Analyze mapped) |
| 8 | Legacy / tests | **PASS** |
| 9 | Logging secrets | **PASS** |

---

## Open issues / residual risks

1. **Adapter→adapter codec coupling:** `personal_archive` PRIVATE-depends on `compression_zstd` and `crypto_sodium` public headers. Pre-existing; not introduced by SR9.
2. **SR10 remaining work:** **M01–M26** human matrix (`NTFS_SHRINK_SR10_VERIFICATION_MATRIX.md`) still blocks release qualification. Debug/Release builds and source-limit passed again after the 2026-08-21 review remediation.

## 2026-08-21 review remediation

The focused post-implementation review findings were corrected before the final build:

- Scratch logical address space now equals source logical size; allocation quota remains separate,
  is page-rounded with checked arithmetic, and is checked against physical free space before writes.
- Overlay first partial-page writes hydrate the complete base page; create-new ownership prevents
  backing/index/temp clobber.
- Locked flush/readback/Boot commit and unlocked CHKDSK/postcheck are separate calls; the raw target
  handle is destroyed before CHKDSK starts.
- Boot geometry and ShrinkPlan bind final `$MFT` and `$MFTMirr` LCNs; commit patches all three BPB
  fields and final structural audit reads MFT/Bitmap/Mirror only from the real Target.
- Metadata mutations bind record sequence, exact attribute location/capacity, preimage digest and
  pre-encoded replacement runlist; execution does not re-plan after destructive writes.
- `$LogFile` preflight validates both restart pages, USA, geometry, version, LSN and latest clean
  state; runlist terminators, sparse zero-fill, Attribute List references and exact device geometry
  are fail-closed.
- Boot invalidation checks cancellation before the first write and then completes both poison writes
  without an interruptible gap; every subsequent Worker failure maps to target-incomplete unless a
  commit/postcheck state has a stronger stable outcome.

---

## Explicit gate statements

1. **Debug and Release capabilities are ON.** Both advertise `restore.ntfs_shrink.v1`; the Host continues to reject
   `kAnalyzeNtfsShrink` for any custom runtime that omits the capability.
2. **M01–M26 are not claimed passed.** This document is a static/architecture audit only. Destructive and regression matrix rows remain incomplete per `NTFS_SHRINK_SR10_VERIFICATION_MATRIX.md`.
3. **Do not mark or distribute the Release build as qualified** until development-plan §21 gates (including M01–M26
   records and full SR10 DoD) are satisfied.

---

## Evidence index (absolute paths)

- `D:\Work\OpenSource\Aegra\src\ntfs_core\CMakeLists.txt`
- `D:\Work\OpenSource\Aegra\src\ntfs_resize\CMakeLists.txt`
- `D:\Work\OpenSource\Aegra\src\apps\shell_extension\CMakeLists.txt`
- `D:\Work\OpenSource\Aegra\src\apps\service\src\service_main.cpp` (`runtime_capabilities`)
- `D:\Work\OpenSource\Aegra\src\apps\service\src\service_host.cpp` (`required_capability` / `capability_enabled`)
- `D:\Work\OpenSource\Aegra\src\apps\service\src\worker_job_service_shrink_analyze.cpp`
- `D:\Work\OpenSource\Aegra\src\apps\worker\src\personal_archive_restore_shrink.cpp`
- `D:\Work\OpenSource\Aegra\src\apps\worker\src\windows_personal_backup.cpp` (fstream hits)
- `D:\Work\OpenSource\Aegra\src\adapters\personal_archive\` (fstream + codec adapter includes)
- `D:\Work\OpenSource\Aegra\docs\architecture\MODULAR_ARCHITECTURE.md`
- `D:\Work\OpenSource\Aegra\docs\development\NTFS_SMALLER_TARGET_VOLUME_RESTORE_DEVELOPMENT_PLAN.md` §19.2
- `D:\Work\OpenSource\Aegra\docs\development\NTFS_SHRINK_SR10_VERIFICATION_MATRIX.md`
