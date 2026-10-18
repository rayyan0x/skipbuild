# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- `skipbuild --init [CMakeLists.txt] [--dry-run]` — writes the compiler-launcher snippet right after `project(...)`, so integration is one command instead of a manual edit. It is idempotent (skips files that already set a launcher), preserves CRLF line endings and a UTF-8 BOM, falls back to `cmake_minimum_required(...)` when there is no `project()`, and prints the snippet to add when it finds no safe insertion point. Covered by `test_init`.

### Fixed
- Quoted compiler defines (e.g. `-DFOO="bar"`, MSVC `-DSKIPBUILD_VERSION="0.2.2"`) now reach the compiler intact — the compiler is replayed with an argv-preserving spawn instead of going through `std::system`/`cmd.exe`, which stripped inner quotes.

### Documentation
- Reworked the README "Integration" section to lead with the no-edit path (two `-D` flags), then `skipbuild --init`, then the manual snippet, with a compact per-build-system table and a single "VS generator is not supported" gotcha.

## [0.2.2] - 2026-10-18

### Added
- macOS x64 (Intel) release builds (`skipbuild-macos-x64`) via the `macos-15-intel` runner, so Intel Macs no longer 404 on install.

### Fixed
- `scripts/install.ps1` checksum verification on Windows PowerShell 5.1: use `-UseBasicParsing` and download `SHA256SUMS` to a file before comparing (GitHub serves it as binary, so the previous in-memory comparison silently never ran).

## [0.2.1] - 2026-10-18

### Added
- `--compiler-check` / `SKIPBUILD_COMPILER_CHECK` / `compiler_check` to control how the compiler binary is fingerprinted:
  - `mtime` (default) — size + mtime + `--version` output
  - `version` — `--version` output only (stable across reinstalls/rebuilds of the same version)
  - `content` — SHA-256 of the compiler binary (stable across reinstalls, detects real changes)
  - `none` — exclude the compiler from the cache key
- `SKIPBUILD_LOG_FILE` to append timestamped `[skipbuild]` log lines to a file — reliable output capture under the Visual Studio generator / MSBuild, which otherwise hides tool output. Setting it alone implies `info` level.

### Changed
- Compiler fingerprinting is now configurable, so frequent compiler reinstalls/updates no longer have to invalidate the entire cache (see README "Compiler Fingerprinting & Cache Invalidation")

### Removed
- Stray `cache/core_remote.cpp.bak` backup file that was accidentally tracked

### Fixed
- `find_package(skipbuild)` integration no longer requires OpenSSL (keeping skipbuild zero-dependency) and no longer includes a non-existent `skipbuild-targets.cmake`; it now works after `cmake --install` and locates the launcher relative to the install prefix
- Release assets now use canonical per-platform names (`skipbuild-{linux,macos}-{x64,arm64}`, `skipbuild-windows-{x64,arm64}.exe`) plus a `SHA256SUMS` file, so `scripts/install.sh` and `scripts/install.ps1` download successfully and checksum verification passes
- `scripts/install.sh` uses `shasum -a 256` when `sha256sum` is unavailable (macOS)

### Documentation
- Corrected README: `--cache-dir` overrides the cache directory for the current run (it does not "print" it); the Windows cache directory is `%USERPROFILE%\.cache\skipbuild`; the Visual Studio CMake generator ignores `CMAKE_CXX_COMPILER_LAUNCHER` (Ninja/Makefile generators only); the `--` separator is optional; and the CLI reference is now complete

## [0.2.0] - 2026-10-17

### Added
- Remote cache support with WinHTTP (Windows) and POSIX sockets (Linux/macOS)
- Cross-platform HTTP client for artifact upload/download
- CLI options: `--remote-url`, `--remote-read-only`, `--remote-timeout`, `--remote-stats`
- Environment variable configuration using the `SKIPBUILD_*` prefix (e.g. `SKIPBUILD_REMOTE_URL`, `SKIPBUILD_DIR`, `SKIPBUILD_MAX_SIZE`)
- `REMAKE_LOG` environment variable for log levels (`debug`, `info`, `warn`, `error`, `none`)
- Visual Studio / MSBuild logging via `OutputDebugString` (visible in VS Output → Debug window)
- Automatic fallback: local cache → remote cache → native compilation
- Multi-file artifact support (primary outputs + sidecars: .pdb, .idb, .dwo)
- Compression support (ZSTD when available)

### Changed
- **Breaking:** Renamed environment variable prefix from `BUILDCACHE_*` to `SKIPBUILD_*` to match the tool name. The legacy `BUILDCACHE_*` names are still accepted as a fallback, but `SKIPBUILD_*` takes precedence.
- **Breaking:** Default cache directory and config paths now use `skipbuild` (e.g. `~/.cache/skipbuild`, `.skipbuild.conf`) instead of `buildcache`.
- Version is now centralized via a `SKIPBUILD_VERSION` compile definition instead of hardcoded strings in the CLI and HTTP user-agent
- Cache key determinism improvements
- Stats tracking with persistent JSON storage
- Sidecar file handling and restoration

### Removed
- Orphaned `cache/remote.cpp` — it was never compiled and duplicated the `RemoteCache` implementation in `cache/core.cpp`

### Fixed
- Test suite crashes during cleanup (stats tracker destructor ordering)
- Cache key determinism across different build directories
- Response file (`.rsp`) parser: strict bounds checking, recursion depth guard, UTF-8 BOM stripping, CRLF handling, and quote stripping
- Fatal `0xC0000409` (STATUS_STACK_BUFFER_OVERRUN) crash when parsing quoted arguments in response files on Windows

## [0.1.0] - 2026-10-10

### Added
- Initial release of remake (skipbuild)
- Local compilation cache with content-addressable storage
- Support for GCC, Clang, and MSVC compilers
- Support for link.exe, lld-link, and g++/clang++ linkers
- Precompiled header (PCH) support
- Split DWARF (.dwo) support
- MSVC PDB/IDB sidecar support
- Config file support (`.skipbuild.conf`, legacy `.buildcache.conf`)
- Environment variable configuration
- Statistics tracking and reporting
- Cache eviction (LRU + age-based)
- Resiliency: corruption detection, automatic purge, fail-safe fallback
- Multi-process safe cache access (file locking)
- ZSTD compression for artifact storage