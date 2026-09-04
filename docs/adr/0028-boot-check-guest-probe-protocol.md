# ADR-0028：BootCheck Guest Probe Protocol V1

- 状态：Superseded（2026-09-03：启动判据改为差分盘 overlay 增长，COM1 Guest Probe 全链路移除，
  见 [BOOT_CHECK_DEVELOPMENT_PLAN.md](../development/BOOT_CHECK_DEVELOPMENT_PLAN.md) 同日记录；
  Manifest V7 的 `probe_protocol_version` 字段保留为历史记录）
- 日期：2026-08-29
- 决策者：Aegra maintainers
- 关联模块：contracts、virtualization、apps/service、apps/boot_check

## 背景

VirtualBox 报告 VM `running` 只表示虚拟硬件开始执行，不能证明备份内 Windows、系统卷、SCM 和 Aegra
Service 已启动。BootCheck V1 需要一个无网络、无 Guest Additions 的最小成功信号，并优先降低首版实现与
部署复杂度。

## 决策

1. V1 不创建或挂载 Challenge ISO，Host 不向 Guest 发送 Challenge。
2. READY 经本 Job 专属的本机 Windows Named Pipe 传输，pipe 不承载普通 Service IPC。VirtualBox：
   Host Adapter 在 VM 启动前创建 pipe server（拒绝远程客户端），VirtualBox 以 client 模式把 VM 的
   COM1（I/O `0x3F8`、IRQ 4、16550A）连接到该 pipe。Hyper-V：方向相反，`Set-VMComPort` 由 Hyper-V
   以 pipe server 身份创建同名 pipe，Host Adapter 以 client 连接（对 pipe 未就绪/busy 做取消感知
   重试）。两种传输承载完全相同的 READY 字节与匹配语义。
3. Guest Aegra Service 达到 BootCheck 成功阶段后，向 COM1 发送且只发送固定 ASCII 字节串：
   `AEGRA_BOOTCHECK_READY_V1\r\n`，共 26 bytes。
4. Host 在 boot deadline 内对 COM1 字节流做 26-byte 滑动窗口精确匹配（KMP）：只有完整 READY 字节序列
   在流中逐字节出现才接受成功。串口读取可以分段；UEFI 固件（EDK2）会在 guest Service 运行前向 COM1
   输出自身引导信息，因此匹配必须容忍前导噪声，不得假设信号是流的最先字节（2026-08-29 在 VirtualBox
   7.2.14 UEFI VM 实测确认）。扫描有 4 MiB 上限，超限、pipe 断开、VM 提前退出或超时均不能形成成功。
   成功后 Host 立即停止读取并关闭 VM；Guest 按合同不得发送尾随数据。
5. 固定串的 `V1` 后缀是协议版本。未来改变 payload 时必须提升版本并更新 Manifest Boot Profile 的 probe
   protocol version；产品未发布，不保留旧开发协议的兼容解析。
6. V1 信号只证明这台隔离 VM 中有程序写出了约定字节，不绑定 job id，不防止旧消息、错误 VM 或重放，也不是
   远程安全证明。隔离的 per-job `VBOX_USER_HOME`、VM、Named Pipe、无网络和 Host deadline 是首版边界。

## 备选方案

- **VM running / 截图：** 无法证明 Windows 和 Service 到达目标阶段。
- **Guest Additions property：** 增加客体依赖与宿主集成面，不适用于最小隔离配置。
- **虚拟网络 health endpoint：** 扩大攻击面并引入防火墙、地址和网络初始化不确定性。
- **Challenge ISO + nonce-bound response：** 可绑定任务并防止旧消息串线，但增加 ISO 生成、挂载、Guest 探测、
  双消息状态机和清理面；首版明确暂不采用。
- **Host/Guest 双向 COM1 challenge：** 不需要 ISO，也能提供任务绑定；首版仍选择单向固定信号以先验证完整链路。
- **JSON/binary DTO over serial：** 对单一成功事件增加 framing 和解析面；固定 26-byte 标识足够表达 V1 结果。

## 影响

- `contracts` 拥有 protocol version 与固定 READY 常量；`virtualization` 提供固定字节串的精确匹配与
  流式滑动窗口 `BootProbeSignalScanner`（O(1) 内存、计数已扫描字节）。
- Format Boot Profile 的 probe protocol version 引用同一 Contracts 常量，防止版本漂移。
- `IBootCheckVmSession::wait_for_boot_probe` 隐藏 provider endpoint；VirtualBox Adapter 拥有本机 pipe
  server，Hyper-V Adapter 拥有对应的 pipe client；两者共用可取消 connect/read 和同一 READY 扫描器。
- Windows BootCheck Adapter 仅在 live SMBIOS `SystemProductName` 为 `VirtualBox` 或 `Virtual Machine`
  （Hyper-V）时尝试 COM1；使用 115200/8N1、无 flow control 和 1 秒 write timeout。其它环境不打开
  COM1，COM1 不可用不阻止普通 Service 启动。
- 日志只记录 protocol、阶段、接收字节数和匹配结果，不把收到的任意串口内容原样写入日志。
- 由于没有 Challenge，Guest 无法由 Host 输入判断当前启动是否属于 BootCheck；Service 的发送路径必须快速、
  有界，COM1 不存在或不可写时不得阻塞普通 Service 启动。

## 验证

- Debug/Release 生产构建和源码规模检查通过。
- 人工覆盖完整信号、分段读取、截断、错误字节、pipe 断开和 deadline。
- 用隔离 VirtualBox home 确认 7.2.14 接受 COM1 `0x3F8/IRQ4`、client Named Pipe 和 16550A 配置，随后注销
  临时 VM 并删除临时目录。
- 非 VirtualBox 主机确认 SMBIOS gate 不访问 COM1；真实 Windows Recovery Point 的 Guest→Host READY
  端到端验收仍需在后续独立 BootCheck Host 接入后执行。
- 在隔离 VM 中确认只有完整 `AEGRA_BOOTCHECK_READY_V1\r\n` 形成成功，VM running 与 guest poweroff 均不
  形成成功。
