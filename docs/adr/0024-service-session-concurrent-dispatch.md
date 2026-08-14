# ADR-0024：Service session 并发分派与请求级 deadline

- 状态：Accepted
- 日期：2026-08-14
- 决策者：Aegra 项目
- 关联模块：apps/service、apps/desktop、ports/message_channel

## 背景

Service 控制 Pipe 只有一个 session。原 Host 在同一循环中顺序执行 receive、业务调用和 send；Windows 网络
Provider 对断网 UNC 的同步调用可能长时间不返回，因此一个 Repository 请求会阻塞后续所有请求，Desktop
最终把单请求超时误判为 session 断线并禁用整个 UI。

## 决策

1. 每个 session 固定一个 reader 和一个 writer；二者可并发操作 `IMessageChannel`。
2. reader 只负责收帧、协议校验和入有界队列，不执行 Repository、文件系统或控制面业务。
3. 请求分到四条独立串行 lane：快速控制面、Repository/Recovery Point 读取、文件浏览、命令。每条 lane
   有界，避免无界线程和内存增长，同时隔离会阻塞的系统调用。
4. 每条请求拥有独立 cancellation source、30 秒 deadline、`request_id` correlation 和 exactly-once 响应门。
   deadline 到达立即排队该请求的失败响应并请求协作取消；业务调用迟到时丢弃其结果。
5. 响应允许乱序，经单 writer 写入 Pipe。业务失败、队列满或 deadline 不关闭 session；只有 Pipe/framing/
   peer close、Service stop 或写失败结束 session。
6. Desktop 的本地 deadline 通过原请求 handler 注入带相同 `request_id`/kind 的失败响应，只结束对应 UI 域，
   不触发 reconnect。
7. Service 不做调用者身份认证。Pipe 仍拒绝远程客户端并保留最小本机 ACL；browse token 只绑定 Service 生成的
   session id。

## 备选方案

- 每请求创建或 detach 一个线程：无法有界回收，不采用。
- 共享无分类线程池：多个断网调用可能耗尽全部 worker，不能保证快速控制面响应，不采用。
- 超时即断开 Pipe：会清空所有 pending 请求并造成 reconnect storm，不采用。

## 影响

- 单个 Repository 超时不再使整个 Desktop 进入 Disconnected。
- 同一 lane 内仍保持顺序，避免同类 mutation 并发；不同 lane 的响应可能乱序。
- 不支持取消的 Windows 调用可能在后台 lane 中迟到，但 deadline 响应与其它 lane 不受影响。

## 验证

- Debug/Release 构建及 architecture/static checks。
- 人工断开网络 Repository 后刷新，同时验证导航、Connections、Jobs、Schedules 和 Add 仍可用。
- 检查日志中超时 request 只有一个 correlation response，session 无 disconnect/reconnect。
