# Aegra

Aegra 是面向物理机与虚拟机的模块化备份、恢复和企业级全局去重系统。本仓库是按照新架构从零重构的实现，不包含旧项目兼容层。

开发前必须阅读：

- [C++ 工程开发规范](docs/development/CPP_ENGINEERING_STANDARD.md)
- [模块化目标架构](docs/architecture/MODULAR_ARCHITECTURE.md)

## 构建

Windows 构建固定使用 Visual Studio 2026 Insiders 的 x64 编译器、CMake 和 Ninja：

```powershell
.\scripts\build.cmd Debug
```

脚本先加载 VS 2026 Insiders 的 `VsDevCmd`，不使用系统中的旧版 Visual Studio。
