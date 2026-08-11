# Explorer Shell / NTFS 产品上限与稳定码

| 属性 | 内容 |
| --- | --- |
| 状态 | ES0 冻结权威（ADR-0023） |
| 日期 | 2026-08-11 |
| 关联 | [开发计划](EXPLORER_SHELL_EXTENSION_DEVELOPMENT_PLAN.md)、[架构](../architecture/EXPLORER_ARCHIVE_BROWSING.md) |

产品配置只能收紧，不得松于 current V7/format 硬上限。

## 1. 产品上限

| ID | 项 | 上限 |
| --- | ---: | ---: |
| S01 | 单次目录页 | 256 items |
| S02 | 单次 stream/file read 请求 | 1 MiB |
| S03 | NTFS MFT record | 64 KiB |
| S04 | NTFS index record | 1 MiB |
| S05 | attribute list extension depth | 32 |
| S06 | NTFS path/navigation depth | 256 |
| S07 | NTFS MFT record cache | 1024 records |
| S08 | NTFS index page cache | 256 records |
| S09 | materialized single file（复制/缓存配额） | 16 GiB（默认可配置） |
| S09a | 直接打开（物化 + ShellExecute）单文件 | **1 GiB**（超过则拒绝并提示先复制） |
| S10 | Shell Cache total | 32 GiB（默认可配置） |
| S11 | standalone repository-parent walk | 16 ancestors |
| S12 | PIDL display name UTF-16 code units | 255 |
| S13 | PIDL version | 1 only（不匹配拒绝） |

## 2. 稳定 message code

### Shell

```text
shell.archive_not_found
shell.archive_changed
shell.unsupported_version
shell.unsupported_content_kind
shell.password_required
shell.password_invalid
shell.password_cancelled
shell.parent_missing
shell.split_incomplete
shell.archive_corrupt
shell.volume_not_found
shell.filesystem_not_ntfs
shell.item_not_found
shell.read_failed
shell.cache_limit
shell.cache_write_failed
shell.file_too_large_for_direct_open
```

### NTFS

```text
ntfs.invalid_boot_sector
ntfs.invalid_geometry
ntfs.corrupt_mft_record
ntfs.fixup_failed
ntfs.attribute_out_of_bounds
ntfs.runlist_corrupt
ntfs.index_corrupt
ntfs.compressed_unsupported
ntfs.efs_unsupported
ntfs.entry_not_found
ntfs.read_failed
```

业务分支只依赖稳定码与 `ErrorCode`，不解析英文 message 文本。错误与日志禁止包含密码、密钥、SecretRef 或文件内容。

## 3. ErrorCode 映射（建议）

| 场景 | ErrorCode |
| --- | --- |
| 路径/参数非法 | `kInvalidArgument` |
| 版本/content kind 不支持 | `kUnsupportedVersion` |
| 取消 | `kCancelled` |
| 读失败 | `kIoFailure` |
| 损坏结构 | `kCorruptData` |
| 项/卷不存在 | `kNotFound` |
| 密码错误/需要 | `kUnauthorized` |
| 缓存配额 | `kInsufficientSpace` |
| 内部不变式 | `kInternal` |
