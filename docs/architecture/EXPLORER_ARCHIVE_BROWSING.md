# Explorer `.bkf` 进程内只读浏览

| 属性 | 内容 |
| --- | --- |
| 状态 | 现行设计（ADR-0023） |
| 日期 | 2026-08-11 |
| 进程 | `explorer.exe` 加载的 `aegra_shell_extension.dll`（x64） |
| 非目标 | Mount Host、Dokan、VHDX、盘符、写回、旧格式兼容 |

## 1. 用户可见行为

双击 current V7 `.bkf` 后，文件作为只读虚拟文件夹打开：

| content_kind | 树结构 | 内容读取 |
| --- | --- | --- |
| `volume_set` | Disk → Volume → NTFS 根目录树 | Volume Random Reader + NTFS Parser |
| `file_set` | tip File Index 目录树 | File Archive Chain `read_stream` |

共享：PIDL、详情列、导航、只读属性、`IDataObject` 复制、`IStream`、默认应用打开（物化临时文件）。

## 2. 组件与依赖

```text
aegra_shell_extension (Composition Root, SHARED DLL)
  |-- third_party/msf (MSF header-only: ShellFolderImpl, EnumIDList, PIDL, RGS)
  |-- Base / Contracts / Ports / Format
  |-- PersonalRepository (Catalog graph, chain resolve)
  |-- AdapterStorageLocal (optional repository object IO)
  |-- AdapterPersonalArchive (chain + volume random reader)
  |-- AdapterNtfs (IRandomAccessReader -> NTFS DTO)
  +-- Crypto / Compression (via PersonalArchive)

Browse model: ArchiveShellModel (volume_set Disk/Volume/NTFS; file_set tip Index)
COM host: HostShellFolder / HostEnumIDList / HostItem (msf_host pattern)

aegra_adapter_ntfs (STATIC)
  +-- Base, Ports only
```

禁止：Qt、Service、Worker、Application、Dokan、Mount Host。

## 3. 读取链

```text
Explorer COM (MSF HostShellFolder)
        |
        v
ArchiveShellModel
        |
        +-- volume_set
        |     PersonalArchiveChainReader
        |     PersonalArchiveVolumeRandomReader (per volume)
        |     NtfsVolumeReader
        |     list_directory / describe_entry / read_file
        |
        +-- file_set
              PersonalFileArchiveChainReader
              tip Index list_children / describe_entry
              read_stream (base-first parent resolve)
```

`PersonalArchiveVolumeRandomReader` 把一个 Manifest Volume 的 Chunk 视图暴露为 `IRandomAccessReader`（offset 0 = 该卷 Boot Sector）。它不知道 NTFS。

## 4. PIDL v1

固定头：magic、version、kind、payload size、attributes、logical size、时间；附带有界 UTF-16 显示名。

| Kind | Payload |
| --- | --- |
| Disk | `disk_number` |
| Volume | `volume_index` |
| NtfsDirectory / NtfsFile | `volume_index + MFT record + sequence` |
| FileSetDirectory / FileSetFile | `entry_id + stream_index` |
| StatusItem | 稳定 message code 标识 |

PIDL 不含指针、C++ 对象、Archive offset、密码或绝对 NTFS 路径。`CoTaskMemAlloc` 分配；version 不匹配拒绝。

## 5. Session 与生命周期

- Root `IShellFolder` 在 `CreateShellFolderView` 创建 Explorer View 之前打开并认证 Session，与
  AIVImage 的 ShellFolder 流程一致；密码错误在同一个模态框内继续校验。
- 用户取消密码框时返回空 `IShellView`，本次导航终止并停留在原 Explorer 位置；不得创建状态项、
  错误框或把 Cancel 记录为粘性 Session 失败。
- 子 Folder / Enumerator / Stream 共享 `shared_ptr<ArchiveShellSession>`。
- 最后引用释放或 Archive identity（路径/size/last-write）变化时取消并释放 Reader。
- 无业务全局单例；COM 仅保留 module lock 与 `HINSTANCE`。

## 6. 链与 Repository

1. 规范化普通文件路径；拒绝 device namespace、目录、reparse escape、非 `.bkf` 主文件。
2. 打开 tip Archive，读认证 Header/Manifest。
3. Full standalone → 单层。
4. Incremental：向上找 `aegra.repository`；Catalog 定位 `archive_main_key`；`resolve_chain`；base-first open。
5. 找不到 Repository 或链不完整 → `shell.parent_missing`（status item 或打开失败）。

## 7. 安全与资源

- 全部外部长度/offset/PIDL 先验证；加乘防溢出。
- NTFS/Index/stream cache 固定容量。
- Session 密码缓存使用 Windows DPAPI 当前用户范围保护，只保存密文；Reader `open` 前临时解密，
  使用后立即擦除 DPAPI 输出和 UTF-8/UTF-16 明文缓冲；日志不含认证材料与文件内容。
- 临时 Cache：当前用户 ACL、配额、TTL、原子 rename 发布。
- C++ 异常在 COM/DLL 导出边界转为 HRESULT。

## 8. 与 Mount Host 的关系

| 场景 | 权威路径 |
| --- | --- |
| Explorer 双击浏览/复制/默认打开 | Shell Extension in-process |
| 需要盘符、整盘镜像、Dokan 会话 | Mount Host（Service 编排） |

二者不共享 Session；Shell Extension 不启动 Mount Host。

## 9. 稳定错误码（Shell/NTFS 边界）

见 [EXPLORER_SHELL_PRODUCT_LIMITS_AND_CODES.md](../development/EXPLORER_SHELL_PRODUCT_LIMITS_AND_CODES.md)。通用 format/file 错误继续复用既有码。

## 10. 验证

生产 Target 构建、`CheckSourceLimits`、依赖边界审查、人工 Explorer 矩阵（注册、两种 content kind、加密、分卷、损坏输入、生命周期）。不新增测试 Target。
