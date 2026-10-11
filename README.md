# skipbuild

[![Build and Test](https://github.com/rayyan0x/skipbuild/workflows/Build%20and%20Test/badge.svg)](https://github.com/rayyan0x/skipbuild/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**A zero-setup, cross-platform C++ build cache that makes rebuilds near-instant, on Windows, Linux, and macOS.**

skipbuild wraps your compiler, hashes everything that affects the output, and restores previously built objects when the inputs have not changed. It is designed to eliminate the 40+ minute compilation bottlenecks that slow down large C++ projects.

---

## Benchmark

Measured locally on a project of **50 source files, each including 10 headers**, compiled with `-O2 -std=c++20`.

| Scenario | Time |
|---|---|
| Cold build (no cache) | 22.0 s |
| Hot rebuild (all cache hits) | 0.2 s |
| **Speedup** | **~110x** |
| Overhead per file on cache miss | ~4 ms (<1% of compile time) |

**Hardware, OS, compiler version:** Windows 11, MSVC 19.51 / Linux (Ubuntu 24.04, GCC 13.3) / macOS 26 (Apple Clang 21)

Reproduce it:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
# Cold run: clear the cache, then time the build
# Hot run: rebuild without changes, then time it
```

---

## Feature comparison

| Scenario | No cache | ccache | sccache | skipbuild |
|---|---|---|---|---|
| Change one `.cpp` | Recompile all | Recompile one | Recompile one | Recompile one |
| Switch git branches | Full rebuild | Full rebuild | Full rebuild | Restore cached objects |
| MSVC `/Zi` PDB support | — | Limited | Experimental | Native `.pdb` + `.idb` tracking |
| Remote cache | — | — | S3/GCS/Azure | HTTP/S3-compatible |
| Zero external dependencies | — | Needs zlib | Needs OpenSSL, zstd | WinCrypto / POSIX sockets |
| Windows-first design | — | Linux-first | Works | Native WinHTTP, `/Zi`, `cl.exe` |
| Fail-safe on corruption | — | Manual clean | Manual clean | Auto-purge + native fallback |

---

## Features

### Caching
- **Content-addressable keys:** SHA-256 over source content, normalized include paths, sorted flags, compiler hash, compiler type, and relevant environment variables.
- **Deterministic keys:** the same inputs produce the same key across machines and operating systems.
- **Build-root normalization:** paths are normalized relative to the build root, with forward slashes and case handling on Windows.
- **Compiler-aware invalidation:** the compiler binary hash and version string are captured automatically, so a compiler upgrade invalidates the cache.
- **Local storage:** two-level hash sharding (`aa/bb/<hash>/`), configurable location (default `~/.cache/buildcache`).
- **Eviction:** size limit (default 5 GB, target 85%), LRU, and max age (default 30 days).
- **Atomic writes:** temp file, then `MoveFileExW` on Windows or `rename()` on POSIX.
- **Corruption handling:** zero-byte files, missing metadata, and corrupted zstd data are purged on access.

### Compiler support
- **MSVC (`cl.exe`):** `/Zi` PDB, `.idb`, `/Fo`, `/I`, `/std:`, `/O`, `/W`.
- **GCC and Clang:** `-std=`, `-O`, `-g`, `-I`, `-fPIC`, `-gsplit-dwarf` (`.dwo`).
- **Precompiled headers:** detects `/Yc`, `/Yu`, `-x c++-header`, and `-include`.
- **Sidecar files:** `.pdb`, `.idb`, `.dwo`, and arbitrary sidecars.

### Compression
- Zstd level 3, roughly 70% size reduction on object files.
- Zstd is fetched and built automatically at configure time if not installed, so no manual setup is needed.

### Remote cache
- HTTP and S3-compatible storage (AWS S3, MinIO) via presigned URLs and SigV4.
- Fail-fast timeouts: 3 s connect and read by default, one retry, immediate fallback on 5xx.
- Read-only mode for CI replicas.
- **Zero external dependencies:** uses WinHTTP on Windows, POSIX sockets on Linux/macOS — no cURL/OpenSSL required.

### Resilience
- **Transparent fallback:** any cache error logs a warning and runs the native compiler.
- **Cache is advisory:** the compiler always runs on a miss. A broken cache never blocks a build.
- **Remote timeout to local fallback:** an unreachable remote cache falls back to local cache or a native compile.
- **Read-only mode:** `BUILDCACHE_READ_ONLY` / `--read-only` for CI read-only replicas.

### Configuration
Priority: CLI flags, then environment variables, then config file.

| Source | Example |
|---|---|
| Config file | `.buildcache.conf` or `~/.config/buildcache/config` |
| Environment | `BUILDCACHE_DIR`, `BUILDCACHE_MAX_SIZE=10G`, `BUILDCACHE_REMOTE_URL` |
| CLI | `--cache-dir`, `--max-cache-size`, `--remote-url`, `--stats`, `--clean` |

### Integration
- **CMake:** `set(CMAKE_CXX_COMPILER_LAUNCHER "skipbuild")`
- **Make / Ninja:** `CXX := skipbuild g++`
- **Visual Studio:** Project → C/C++ → Command Line → Additional Options: `skipbuild -- %(AdditionalOptions)`
- **Any build system:** `skipbuild -- clang++ -c foo.cpp -o foo.o`

### Statistics
- Local: hits, misses, hit rate, bytes and time saved, entry count, total size.
- Remote: hits, misses, uploads, downloads, errors.
- Commands: `--stats`, `--remote-stats`, `--clear-stats`, `--clean`.

---

## Installation

### Requirements
- CMake 3.20 or newer
- A C++20 compiler: MSVC 2019+ (19.20+), GCC 11+, or Clang 13+
- Internet access at configure time only if zstd is not already installed (it is downloaded from GitHub)

### Build from source

```bash
git clone https://github.com/rayyan0x/skipbuild.git
cd skipbuild
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

The binary is at `build/skipbuild` on Linux and macOS, and `build/Release/skipbuild.exe` on Windows (Visual Studio generator).

### Install

```bash
cmake --install build --config Release
```

Default install prefix: `/usr/local` on Unix, `C:\Program Files\skipbuild` on Windows.

---

## Testing

```bash
ctest --test-dir build -C Release --output-on-failure --timeout 120
```

| Test | Covers |
|---|---|
| `cache_test` | Basic put and get, key determinism |
| `concurrent_test` | Multi-threaded cache access |
| `multiprocess_test` | Multi-process locking |
| `eviction_test` | LRU, age, and size-based eviction |
| `simple_test` | Basic workflow |
| `sidecar_test` | PDB, IDB, and DWO sidecar tracking |
| `cache_key_determinism_test` | Cross-machine key equality |
| `resiliency_test` | Corruption, permission failures, network failures |
| `config_test` | Config file, environment, and CLI precedence |
| `stats_test` | Statistics tracking |

Current CI status: all three platforms (Windows, Linux, macOS) pass.

---

## Platform support

| Platform | Compiler | Status |
|---|---|---|
| Windows | MSVC 2019+ | ✅ |
| Linux | GCC 11+, Clang 13+ | ✅ |
| macOS | Apple Clang 13+ | ✅ |

---

## Usage

### Basic Usage (compiler wrapper)

```bash
# Wrap compiler (ccache-style)
skipbuild g++ -c src/main.cpp -o build/main.o

# With separator (recommended for complex commands)
skipbuild -- g++ -c src/main.cpp -o build/main.o -Iinclude -O2

# MSVC
skipbuild -- cl.exe /c src/main.cpp /Fobuild/main.obj /Zi /Iinclude
```

### CLI Commands

```bash
skipbuild --stats              # Show local cache statistics
skipbuild --remote-stats       # Show remote cache statistics
skipbuild --clear-stats        # Reset cache statistics to zero
skipbuild --clean              # Remove all cache entries
skipbuild --cache-dir /path    # Print cache directory
skipbuild --version            # Show version
skipbuild --help               # Show help
```

### Remote Cache Options

```bash
skipbuild --remote-url http://cache:8080
skipbuild --remote-read-only
skipbuild --remote-timeout 5000
```

### Configuration (Priority: CLI > Environment > Config File)

**Config file** (`.buildcache.conf` in project root or `~/.config/buildcache/config`):
```ini
max_cache_size = 10G
cache_dir = /path/to/cache
compression_enabled = true
read_only = false
remote_url = http://cache.internal:8080
remote_read_only = false
remote_connect_timeout_ms = 3000
remote_read_timeout_ms = 3000
remote_max_retries = 1
```

**Environment variables:**
```bash
BUILDCACHE_DIR=/path/to/cache
BUILDCACHE_MAX_SIZE=10G
BUILDCACHE_MAX_AGE=30
BUILDCACHE_COMPRESSION=true
BUILDCACHE_READ_ONLY=false
BUILDCACHE_REMOTE_URL=http://cache.internal:8080
BUILDCACHE_REMOTE_READ_ONLY=false
BUILDCACHE_REMOTE_TIMEOUT=3000
SKIPBUILD_DISABLE=1   # Disable cache (passthrough)
SKIPBUILD_DEBUG=1     # Debug logging
```

### CMake Integration

```cmake
# Option 1: Compiler launcher wrapper (recommended)
set(CMAKE_CXX_COMPILER_LAUNCHER "skipbuild")

# Option 2: Per-target
set_target_properties(my_target PROPERTIES
    CXX_COMPILER_LAUNCHER "skipbuild"
)

# Option 3: find_package (if installed)
find_package(skipbuild REQUIRED)
```

### Make / Ninja

```make
# Makefile
CXX := skipbuild g++
# or
CXX := g++
COMPILE = skipbuild -- g++
```

```ninja
# build.ninja
rule cxx
  command = skipbuild -- g++ -c $in -o $out $flags
```

---

## Architecture

```
skipbuild/
├── cli/              # Compiler launcher (main.cpp)
├── cache/            # Core caching library
│   ├── core.hpp/cpp  # Cache, keys, locking, remote client
│   ├── config.hpp/cpp # Config parsing (file/env/cli)
│   ├── stats.hpp/cpp  # Statistics tracking
│   ├── crypto.hpp    # SHA-256, HMAC (no OpenSSL)
│   └── remote.hpp/cpp # WinHTTP (Windows) / POSIX (Linux/macOS)
└── tests/            # 10 test suites (100% passing)
```

**Core Design Principles:**
1. **Single binary** — Statically linked where possible
2. **No OpenSSL/cURL** — Windows CryptoAPI + custom SHA-256
3. **Deterministic keys** — Same inputs = same cache key everywhere
4. **Atomic operations** — Temp files + `MoveFileEx`/`rename`
5. **Inter-process locking** — Windows `LockFileEx` / POSIX `flock`
6. **Transparent fallback** — Never breaks the build

---

## License

MIT License — see [LICENSE](LICENSE) for details.

---

## Contributing

1. Fork the repository
2. Create a feature branch: `git checkout -b feature/amazing-feature`
3. Make your changes with tests
4. Run the test suite: `cmake -B build && cmake --build build && ctest --test-dir build`
5. Open a Pull Request

### Code Style
- C++20, no exceptions, no RTTI
- RAII for all resources
- `snake_case` for functions/variables, `PascalCase` for types
- Header guards: `#pragma once`
- Documentation: Doxygen-style for public APIs

---

## Acknowledgments

- Inspired by `ccache`, `sccache`, `buildcache`
- Zstd compression by Yann Collet
- Windows CryptoAPI for SHA-256
- WinHTTP / POSIX sockets for networking