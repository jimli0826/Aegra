# Aegra

Aegra 是面向物理机与虚拟机的模块化备份、恢复和企业级全局去重系统。本仓库是按照新架构从零重构的实现，不包含旧项目兼容层。

本项目不保留或新增单元、集成、回归、冒烟、E2E 或 CTest 测试用例。开发变更通过受影响生产 Target 构建、静态/架构检查以及必要的人工运行或 UI 验证确认。

开发前必须阅读：

- [文档中心](docs/README.md)
- [C++ 工程开发规范](docs/development/CPP_ENGINEERING_STANDARD.md)
- [模块化目标架构](docs/architecture/MODULAR_ARCHITECTURE.md)
- [模块开发文档](docs/modules/README.md)
- [个人版 `.bkf` V6 格式](docs/format/PERSONAL_BACKUP_FORMAT_V6.md)

## 构建

Windows 构建固定使用 Visual Studio 2026 Insiders 的 x64 编译器、CMake 和 Ninja：

```powershell
.\scripts\build.cmd Debug
```

脚本先加载 VS 2026 Insiders 的 `VsDevCmd`，不使用系统中的旧版 Visual Studio；构建完成后执行源码规模检查。
