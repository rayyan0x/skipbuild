# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Remote cache support with WinHTTP (Windows) and POSIX sockets (Linux/macOS)
- Cross-platform HTTP client for artifact upload/download
- CLI options: `--remote-url`, `--remote-read-only`, `--remote-timeout`, `--remote-stats`
- Environment variable configuration: `BUILDCACHE_REMOTE_URL`, `BUILDCACHE_REMOTE_READ_ONLY`, `BUILDCACHE_REMOTE_TIMEOUT`
- Automatic fallback: local cache → remote cache → native compilation
- Multi-file artifact support (primary outputs + sidecars: .pdb, .idb, .dwo)
- Compression support (ZSTD when available)

### Changed
- Cache key determinism improvements
- Stats tracking with persistent JSON storage
- Sidecar file handling and restoration

### Fixed
- Test suite crashes during cleanup (stats tracker destructor ordering)
- Cache key determinism across different build directories

## [0.1.0] - 2026-10-10

### Added
- Initial release of remake (skipbuild)
- Local compilation cache with content-addressable storage
- Support for GCC, Clang, and MSVC compilers
- Support for link.exe, lld-link, and g++/clang++ linkers
- Precompiled header (PCH) support
- Split DWARF (.dwo) support
- MSVC PDB/IDB sidecar support
- Config file support (.buildcache.conf)
- Environment variable configuration
- Statistics tracking and reporting
- Cache eviction (LRU + age-based)
- Resiliency: corruption detection, automatic purge, fail-safe fallback
- Multi-process safe cache access (file locking)
- ZSTD compression for artifact storage