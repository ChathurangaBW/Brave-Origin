# Changelog

## 1.0.0 - 2026-09-15
- Added a DPI-aware native Win32 dark interface with channel cards, guarded repair, restore picker, activity log, and busy state.
- Preserved the vetted command-line engine and safety behavior in a shared C++17 core.
- Added static MSYS2/MinGW builds, manifest/version resources, documentation, and isolated end-to-end QA.
- Added a Linux CLI port (`brave-origin-fix-linux`) sharing the engine core semantics: `$HOME/.config/BraveSoftware/<channel>/` profiles, `/proc`-based exact `--user-data-dir` matching with fail-closed unreadable command lines, SIGTERM-then-SIGKILL shutdown, and atomic temp+fsync+rename writes. CLI only; the GUI remains Windows-only.
- Release distribution is via GitHub Releases (Windows GUI + CLI, Linux CLI, SHA256SUMS.txt); build outputs under `dist/` are no longer tracked in git.
