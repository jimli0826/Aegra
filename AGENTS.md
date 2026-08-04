# Aegra Repository Instructions

For every implementation, refactoring, review, test, or build task in this repository, use the project skill at `.agents/skills/aegra-cpp-development/SKILL.md`.

Before modifying production code, read:

1. `docs/development/CPP_ENGINEERING_STANDARD.md`
2. `docs/architecture/MODULAR_ARCHITECTURE.md` when the change affects modules, dependencies, contracts, persistence, processes, or data flow
3. `docs/modules/README.md` and the documents for every affected module

This product has not been released. Do not add compatibility paths, migration logic, aliases, or format fallbacks for code and artifacts from the previous repository unless an approved ADR explicitly requires them.

Use Visual Studio 2026 Insiders from `C:\Program Files\Microsoft Visual Studio\18\Insiders` for Windows builds.

Use QT 6.8.3 from C:\Qt6\6.8.3\msvc2022_64 for UI build.
