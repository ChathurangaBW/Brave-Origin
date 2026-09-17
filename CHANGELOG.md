# Changelog

## Mini consolidation - 2026-09-17
- Consolidated the repo to the mini-only repair app: `src/mini_gui.cpp` plus the shared engine (`src/engine.cpp` / `src/engine.h`), lion icon resources, and a single-target `build.ps1` producing `dist/BraveOriginMini.exe`.
- Removed the full GUI, CLI entry points, Linux port, tests, and QA fixtures/docs.

## 1.0.0 - 2026-09-15
- Added the shared C++17 engine: local-state scan, guarded repair with timestamped backups, atomic writes, and verification.
- Added the mini Scan + Patch window reusing the engine.
- Added static MSYS2/MinGW builds with manifest/version resources and the lion icon.
