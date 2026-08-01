---
name: aegra-cpp-development
description: Enforce Aegra's C++ engineering standard and modular architecture when creating, modifying, reviewing, refactoring, testing, or building C++ source files, CMake targets, module boundaries, public contracts, backup formats, repository components, adapters, or application entry points in the Aegra project.
---

# Aegra C++ Development

Apply the repository's engineering and architecture rules to every implementation change.

## Required context

Before changing production code:

1. Read `../../../docs/development/CPP_ENGINEERING_STANDARD.md` completely.
2. Read `../../../docs/architecture/MODULAR_ARCHITECTURE.md` completely when changing modules, dependencies, public interfaces, persistence, processes, backup pipelines, or repository behavior.
3. Read `../../../docs/modules/README.md` and every affected module document completely.
4. Read the nearest `AGENTS.md` and any ADR or format document referenced by the affected module.
5. Treat these documents as merge requirements, not suggestions.

## Workflow

1. Inspect the affected targets, direct dependencies, tests, and working-tree changes.
2. Define the smallest coherent change and its allowed dependency direction.
3. Add or update tests before considering the implementation complete.
4. Implement in C++20. Keep ownership explicit, use RAII, and depend on ports rather than concrete adapters.
5. Keep functions at most 60 logical lines, nesting at most four levels, and source files at most 800 physical lines. Split responsibilities before exceeding a limit.
6. Do not introduce compatibility code for unreleased legacy formats. Record format or architecture decisions in an ADR when they affect durable contracts.
7. Build the directly affected targets, run their tests, then run architecture/static checks available in the repository.
8. Update design, format, and status documentation in the same change when behavior or contracts change.

## Hard boundaries

- Keep production and runtime implementation in C++.
- Keep `base`, `contracts`, `ports`, `format`, and `pipeline` independent of Windows, databases, network SDKs, UI frameworks, and vendor SDKs unless the architecture document explicitly assigns that dependency.
- Put concrete infrastructure in `adapters`; construct it only in `apps` composition roots.
- Never expose STL types or vendor objects across a C ABI, plugin ABI, DLL ABI, or process boundary.
- Never store plaintext secrets or log credentials, keys, tokens, or customer data.
- Never use global mutable state, owning raw pointers, hidden singleton dependencies, or output-directory link ordering.

## Verification report

Report:

- targets built and tests run;
- relevant dependency-boundary checks;
- any standard exception, including its ADR or documented rationale;
- remaining work only when it is genuinely outside the current change.
