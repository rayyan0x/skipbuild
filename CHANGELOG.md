# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
- Cache key determinism improvements
- Stats tracking with persistent JSON storage
- Sidecar file handling and restoration

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