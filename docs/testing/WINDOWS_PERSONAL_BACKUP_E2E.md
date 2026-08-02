# Windows 个人版真实单卷备份 E2E 测试

## 目标

管理员集成脚本验证真实 Windows 执行链：

```text
Temporary NTFS VHDX
-> Volume Inventory
-> VSS Snapshot
-> Snapshot Block Source
-> aegra_personal_worker
-> encrypted .bkf + .bhx commit
-> VSS cleanup
```

脚本属于 opt-in 管理员测试，不进入普通 CTest。它不会备份或修改系统卷，也不会删除运行前已经存在的
Shadow Copy。

## 运行

从管理员 PowerShell 执行：

```powershell
Set-Location D:\Work\OpenSource\Aegra
.\scripts\test_windows_personal_backup_e2e.ps1
```

脚本默认：

- 使用 Visual Studio 2026 Insiders 执行 Release 构建和普通测试；
- 创建 2 GiB 动态 VHDX 和独立 NTFS 测试卷；
- 交互读取临时 Archive 密码，不把密码写入参数、Job、日志或环境变量；
- 创建 session-persistent Generic Credential，并在 `finally` 中删除；
- 运行真实 stdin Worker；
- 校验结构化成功响应、逻辑容量、Header/Footer/Sidecar magic、无 partial 和无新增 VSS 残留；
- 成功时删除测试目录，失败时保留诊断产物但仍卸载 VHDX。

复用已有构建并保留成功产物：

```powershell
.\scripts\test_windows_personal_backup_e2e.ps1 -SkipBuild -KeepArtifacts
```

脚本也可以单独复制到测试机。把脚本、`aegra_personal_worker.exe`、`libsodium.dll` 和 `zstd.dll` 放在
同一个目录，然后在该目录的管理员 PowerShell 中运行：

```powershell
.\test_windows_personal_backup_e2e.ps1 -SkipBuild -KeepArtifacts
```

PowerShell 默认不会从当前目录搜索命令，前面的 `.\` 不能省略。单文件模式下 `-SkipBuild` 会自动使用
脚本同目录的 `aegra_personal_worker.exe`。

指定 Worker 或测试根目录：

```powershell
.\scripts\test_windows_personal_backup_e2e.ps1 `
    -WorkerPath D:\Build\aegra_personal_worker.exe `
    -ArtifactsRoot D:\AegraE2E `
    -KeepArtifacts
```

也可以单独指定测试 VHDX 的位置；Archive 和日志仍写入 `ArtifactsRoot` 下的本次运行目录：

```powershell
.\scripts\test_windows_personal_backup_e2e.ps1 `
    -SkipBuild `
    -KeepArtifacts `
    -VhdPath E:\AegraE2E\source.vhdx
```

`VhdPath` 必须以 `.vhdx` 结尾。脚本会自动创建父目录，但绝不覆盖已有文件。测试成功且未指定
`KeepArtifacts` 时，脚本在卸载测试卷后删除新建的 VHDX；指定 `KeepArtifacts` 或测试失败时保留 VHDX
以供检查。

## 前置条件

- Windows 管理员 PowerShell；
- VSS、Virtual Disk 和 WMI/CIM 服务可用；
- Visual Studio 2026 Insiders，除非通过 `-WorkerPath` 提供已构建 Worker；
- 测试根目录具有足够空间；动态 VHDX 上限默认为 2 GiB，但实际文件按使用增长。

## 成功标准

- Worker 退出码为 `0`；
- `WorkerResponse.kind == TaskResult`；
- `TaskResult.outcome == Succeeded`，不接受 Snapshot cleanup warning；
- `logical_bytes` 等于测试 Volume 的 IOCTL-backed 大小；
- `.bkf`、`.bkf.bhx` 完成发布且 magic 正确；
- 没有 `.partial`、意外分卷或新增残留 Shadow Copy。

当前脚本验证真实备份链路和持久化提交边界。逐 Chunk 解密、认证、解压的完整 Archive Verify 需要后续
增加 C++ Verify 可执行入口；恢复到第二个 VHDX 并比较文件需要 Windows Block Sink/Restore Worker。
