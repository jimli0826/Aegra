# Aegra Repository Instructions

For every implementation, refactoring, review, or build task in this repository, use the project skill at `.agents/skills/aegra-cpp-development/SKILL.md`.

Before modifying production code, read:

1. `docs/development/CPP_ENGINEERING_STANDARD.md`
2. `docs/architecture/MODULAR_ARCHITECTURE.md` when the change affects modules, dependencies, contracts, persistence, processes, or data flow
3. `docs/modules/README.md` and the documents for every affected module

This product has not been released. Do not add compatibility paths, migration logic, aliases, or format fallbacks for code and artifacts from the previous repository unless an approved ADR explicitly requires them.

Use Visual Studio 2026 Insiders from `C:\Program Files\Microsoft Visual Studio\18\Insiders` for Windows builds.

Use QT 6.8.3 from C:\Qt6\6.8.3\msvc2022_64 for UI build.

## Testing policy

Do not add unit, integration, regression, smoke, end-to-end, or CTest test cases to this repository. Do not add test fixtures, test-only scripts, test executables, or CMake test registrations. Validate changes by building the affected production targets, running the repository's static and architecture checks, and performing focused manual runtime or UI verification when needed.
