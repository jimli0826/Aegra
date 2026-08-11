# MSF (Microsoft Shell Foundation templates)

Vendored **header-only** ATL/COM Shell Extension templates from the open-source
[msf](https://github.com/vbaderks/msf) project (local source used for this tree:
`D:\Work\OpenSource\msf-main`).

## What is included

- Contents of upstream `include/msf/` only (`msf.h`, `shell_folder_impl.h`, PIDL helpers, RGS scripts, etc.).
- Not included: `msf_host` sample host, Visual Studio solution, or any backup-engine code.

## Usage in Aegra

- CMake target: `Aegra::ThirdPartyMsf` (SYSTEM include of this directory).
- Shell Extension (`aegra_shell_extension`) includes `<msf.h>` and implements the host folder
  against Aegra `ArchiveShellModel`.

## Policy

- Do **not** hand-edit files under this directory except when refreshing the vendor drop.
- Source-size limits in Aegra do not apply to `third_party` (see CPP engineering standard).
- Upstream headers reference “See README.TXT for the details of the software licence”;
  keep the upstream license file when refreshing if one is present in the MSF checkout.

## Refresh

```text
robocopy <msf-main>\include\msf  <aegra>\third_party\msf  /E
```

Then rebuild `aegra_shell_extension`.
