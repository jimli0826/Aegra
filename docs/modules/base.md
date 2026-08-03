# `base` 模块开发文档

## 目标

提供所有模块可以安全依赖的最小 C++20 基础类型。`base` 必须稳定、轻量、无平台和业务知识。

## 职责

- `ErrorCode`、`Error`、`Result<T>`。
- 取消令牌、UUID、时间戳、字节视图和安全整数运算。
- 敏感缓冲区、范围检查和最小日志抽象声明。

## 禁止职责

- Windows Handle、VSS、磁盘枚举和注册表。
- 压缩、加密算法实现或第三方 SDK。
- Job、Manifest、Recovery Point 等业务 DTO。
- 全局 Logger、Service Locator 或可变 Singleton。

## 依赖

只允许依赖 C++ 标准库。Target 名为 `aegra_base`，其它模块通过 `Aegra::Base` 链接。

## 接口规则

- `Result<T>` 表示预期失败；错误码是协议稳定值。
- `kOutcomeUnknown` 表示外部 mutation 可能已经完成，调用方必须通过读取权威状态对账，不能当作未执行重试。
- `value()`/`error()` 前置条件违反属于编程错误。
- 时间统一为 UTC；持久化边界使用明确整数或 ISO 8601。
- UUID 类型负责解析、格式化和字节顺序，业务模块不重复实现。
- 整数偏移、长度相加和乘法必须使用 checked helper。

## 测试

- `Result<T>` 的值、错误、移动和 `void` 特化。
- 错误码名称稳定性。
- UUID 合法/非法输入与 roundtrip。
- 整数溢出、边界和零长度。
- 取消状态在线程间可见。

## 完成标准

- 公共头不包含平台或第三方头。
- 没有全局可变状态和拥有资源的裸指针。
- 单 Target 可独立构建，clang-tidy 与全部单元测试通过。
