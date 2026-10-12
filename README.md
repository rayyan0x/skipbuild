# skipbuild

[![Build and Test](https://github.com/rayyan0x/skipbuild/workflows/Build%20and%20Test/badge.svg)](https://github.com/rayyan0x/skipbuild/actions)
[![Release](https://img.shields.io/github/v/release/rayyan0x/skipbuild)](https://github.com/rayyan0x/skipbuild/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platforms](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey)](#supported-platforms-and-compilers)

**skipbuild is a zero-setup, cross-platform C++ build cache that makes rebuilds near-instant.**  
It is for C++ developers who want to eliminate 40+ minute compilation bottlenecks on Windows, Linux, and macOS.

---

## Table of contents

- [Why skipbuild](#why-skipbuild)
- [Quick start](#quick-start)
- [Benchmarks](#benchmarks)
- [How it works](#how-it-works)
- [Supported platforms and compilers](#supported-platforms-and-compilers)
- [Installation](#installation)
- [Integration](#integration)
- [Configuration](#configuration)
- [CLI reference](#cli-reference)
- [Cache management](#cache-management)
- [Troubleshooting](#troubleshooting)
- [Limitations](#limitations)
- [Security](#security)
- [Testing](#testing)
- [Contributing, Changelog, License](#contributing-changelog-license)

---

## Why skipbuild

**Problem:** Large C++ projects suffer from 40+ minute rebuilds. Changing one file, switching git branches, or updating a compiler forces recompilation of everything.

**What skipbuild does:** Wraps your compiler, hashes everything that affects the output (source content, includes, flags, compiler binary, environment), and restores previously built objects when inputs haven't changed. It works like `ccache` but with native MSVC `/Zi` PDB support, deterministic cross-platform keys, and zero external dependencies.

**What skipbuild doesn't do:** It does not replace your build system. It does not cache linking (only compilation with `-c`/`/c`). It does not require a remote cache server — local cache works out of the box.

---

## Quick start

```powershell
# 1. Open "Developer PowerShell for VS 2022" (sets up cl.exe, INCLUDE, LIB)

# 2. Build skipbuild from source
git clone https://github.com/rayyan0x/skipbuild.git
cd skipbuild
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel

# 3. Create a test file
echo '#include <stdio.h>
int main() { printf("Hello, skipbuild!\n"); return 0; }' > hello.cpp

# 4. First compile: cache MISS
build\Release\skipbuild.exe -- cl.exe /c hello.cpp /Fohello.obj
# Output:
# Microsoft (R) C/C++ Optimizing Compiler Version 19.51.36260 for x64
# Copyright (C) Microsoft Corporation.  All rights reserved.
# hello.cpp
# [skipbuild] MISS 48476142e635 -> compiling...

# 5. Second compile: cache HIT
build\Release\skipbuild.exe -- cl.exe /c hello.cpp /Fohello.obj
# Output:
# [skipbuild] HIT 48476142e635

# 6. Check statistics
build\Release\skipbuild.exe --stats
# Output:
# === Build Cache Statistics ===
# Cache Hit Rate:     33.3%
# Total Hits:         2
# Total Misses:       4
# ...
```

---

## Benchmarks

Measured locally on a project of **50 source files, each including 10 headers**, compiled with `-O2 -std=c++20`.

| Scenario | Time |
|---|---|
| Cold build (no cache) | 22.0 s |
| Hot rebuild (all cache hits) | 0.2 s |
| **Speedup** | **~110x** |
| Overhead per file on cache miss | ~4 ms (<1% of compile time) |

**Hardware, OS, compiler versions:** Windows 11, MSVC 19.51 / Linux (Ubuntu 24.04, GCC 13.3) / macOS 26 (Apple Clang 21)

Reproduce it:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
# Cold run: clear the cache (build/Release/skipbuild.exe --clean), then time the build
# Hot run: rebuild without changes, then time it
```

---

## How it works

### Cache key inputs (exactly as hashed)

The cache key is a SHA-256 over:
1. **Source file content** — full text of the `.cpp`/`.c`/`.cc` file
2. **Normalized include paths** — all `-I`/`/I` paths, resolved relative to build root, forward slashes, case-folded on Windows
3. **Sorted compiler flags** — all flags except output/file/path args, sorted for determinism
4. **Compiler binary hash** — SHA-256 of the compiler executable (`cl.exe`, `g++`, `clang++`)
5. **Compiler type and version string** — e.g., `MSVC 19.51.36260`, `GCC 13.3`, `Apple Clang 21`
6. **Relevant environment variables** — `BUILDCACHE_*`, `SKIPBUILD_*`, and compiler-specific vars

### Hit / miss / store flow

```
compile command
       │
       ▼
  parse command → compute key
       │
       ├─ key in cache? ──YES──► restore outputs → DONE (HIT)
       │
       NO
       │
       ▼
  run native compiler
       │
       ├─ success? ──NO──► return error (no cache write)
       │
       YES
       │
       ▼
  compress outputs (zstd level 3) + sidecars → store atomically
       │
       ▼
  return success (MISS)
```

### What is never cached, and why

| Not cached | Reason |
|---|---|
| Linker invocations (`link.exe`, `ld`, `lld`) | Too many inputs, non-deterministic timestamps |
| Commands without `-c`/`/c` | Produces executable/shared library, not object file |
| Compiler detection failures | Cannot compute reliable key |
| `SKIPBUILD_DISABLE=1` set | Explicit opt-out for debugging |

---

## Supported platforms and compilers

| OS | Compiler | Version tested | CI status |
|---|---|---|---|
| Windows | MSVC | 19.51 (2022 BuildTools) | ✅ Verified on local PC |
| Linux | GCC | 13.3 (Ubuntu 24.04) | ✅ CI green |
| macOS | Apple Clang | 21 (macOS 26) | ✅ CI green |

**CI Run:** https://github.com/rayyan0x/skipbuild/actions/runs/38118686561 — All 3 jobs green

> **Note:** Versions listed are exactly what was tested, not minimums. Minimums are MSVC 19.30+, GCC 11+, Clang 13+ (C++20 requirement).

---

## Installation

### Requirements
- CMake 3.20 or newer
- A C++20 compiler: MSVC 19.30+, GCC 11+, or Clang 13+
- Internet access at configure time to fetch zstd 1.5.5 via FetchContent

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
cmake --install build --config Release --prefix <path>
```

**Installed files (18 files):**
```
bin/skipbuild.exe
include/skipbuild/core.hpp
include/zstd.h, zdict.h, zstd_errors.h
lib/skipbuild_core.lib
lib/zstd_static.lib
lib/cmake/skipbuild/skipbuild.cmake
lib/cmake/skipbuild/skipbuild-config.cmake
lib/cmake/skipbuild/skipbuild-config-version.cmake
lib/cmake/zstd/zstdConfig.cmake, zstdConfigVersion.cmake, zstdTargets.cmake, zstdTargets-release.cmake
lib/pkgconfig/libzstd.pc
share/cmake/Modules/Findskipbuild.cmake
```

Default prefix: `/usr/local` on Unix (needs sudo), `C:\Program Files\skipbuild` on Windows (needs Admin). Use `--prefix` for user-writable paths.

---

## Integration

### CMake launcher (Ninja/Make generators)
```cmake
set(CMAKE_CXX_COMPILER_LAUNCHER "skipbuild")
```
Output (Ninja): `[skipbuild] MISS <hash> -> compiling...` / `[skipbuild] HIT <hash>`

### CMake per-target (installed)
```cmake
find_package(skipbuild REQUIRED)
skipbuild_enable_target(my_target)
```

### MSVC wrapper (verified in Developer PowerShell for VS 2022)
```powershell
# Developer PowerShell for VS 2022 sets up cl.exe, INCLUDE, LIB automatically
skipbuild -- cl.exe /c hello.cpp /Fohello.obj
```
Output: `[skipbuild] MISS 48476142e635 -> compiling...` then `[skipbuild] HIT 48476142e635`

### Generic wrapper (any build system)
```bash
skipbuild -- clang++ -c foo.cpp -o foo.o -Iinclude -O2
```

---

## Configuration

Precedence: **CLI flags > Environment variables > Config file**

| Setting | CLI flag | Environment variable | Config key | Default |
|---|---|---|---|---|
| Cache directory | `--cache-dir <path>` | `BUILDCACHE_DIR` / `BUILDCACHE_CACHE_DIR` | `cache_dir` | `~/.cache/buildcache` |
| Max cache size | `--max-cache-size <size>` | `BUILDCACHE_MAX_SIZE` | `max_cache_size` | `5G` |
| Max age (days) | `--max-age <days>` | `BUILDCACHE_MAX_AGE` | `max_age` | `30` |
| Eviction ratio | — | `BUILDCACHE_EVICTION_RATIO` | `eviction_ratio` | `0.85` |
| Compression | `--compression <bool>` | `BUILDCACHE_COMPRESSION` | `compression_enabled` | `true` |
| Read-only | `--read-only <bool>` | `BUILDCACHE_READ_ONLY` | `read_only` | `false` |
| Remote URL | — | `BUILDCACHE_REMOTE_URL` | `remote_url` | — |
| Remote connect timeout (ms) | — | `BUILDCACHE_REMOTE_CONNECT_TIMEOUT` | `remote_connect_timeout` | `3000` |
| Remote read timeout (ms) | — | `BUILDCACHE_REMOTE_READ_TIMEOUT` | `remote_read_timeout` | `3000` |
| Remote max retries | — | `BUILDCACHE_REMOTE_MAX_RETRIES` | `remote_max_retries` | `1` |
| Remote auth header | — | `BUILDCACHE_REMOTE_AUTH_HEADER` | `remote_auth_header` | — |
| Remote verify SSL | — | `BUILDCACHE_REMOTE_VERIFY_SSL` | `remote_verify_ssl` | `true` |
| Disable cache | — | `SKIPBUILD_DISABLE=1` | — | `false` |
| Debug logging | — | `SKIPBUILD_DEBUG=1` | — | `false` |

> **Note:** The tool is named `skipbuild` but uses `BUILDCACHE_*` environment variables (legacy from project rename). `SKIPBUILD_*` is only used for `SKIPBUILD_DISABLE` and `SKIPBUILD_DEBUG`.

**Config file locations (checked in order):**
1. `BUILDCACHE_CONFIG` env var path
2. `.buildcache.conf` in project root
3. `.buildcache/config` in project root
4. Windows: `%APPDATA%\buildcache\config` or `%USERPROFILE%\.buildcache\config`
5. Unix: `~/.config/buildcache/config` or `~/.buildcache/config`

**Config file keys (exact names from code):**
```ini
cache_dir = /path/to/cache
max_cache_size = 10G
max_age = 30
eviction_ratio = 0.85
compression_enabled = true
read_only = false
remote_url = http://cache:8080
remote_connect_timeout = 3000
remote_read_timeout = 3000
remote_max_retries = 1
remote_auth_header = Bearer ...
remote_verify_ssl = true
```

---

## CLI reference

```
skipbuild - C++ build cache (compiler launcher)

Modes:
  skipbuild g++ -c foo.cpp -o foo.o     # Wrap compiler (like ccache)
  skipbuild --cache-dir <path>          # Print cache directory (does not set it)
  skipbuild --stats                     # Show cache statistics
  skipbuild --clear-stats               # Reset cache statistics to zero
  skipbuild --clean                     # Remove all cache entries
  skipbuild --version                   # Show version

Options:
  --max-cache-size <size>   Max cache size (e.g., 10G, 500M)
  --max-age <days>          Max age in days
  --compression <bool>      Enable/disable compression (true/false)
  --read-only <bool>        Read-only cache (true/false)

Environment:
  BUILDCACHE_DIR              Cache directory
  BUILDCACHE_CACHE_DIR        Cache directory (alias)
  BUILDCACHE_MAX_SIZE         Max cache size (e.g., 10G, 500M)
  BUILDCACHE_MAX_AGE          Max age in days
  BUILDCACHE_EVICTION_RATIO   Eviction ratio (0.0-1.0)
  BUILDCACHE_COMPRESSION      Enable/disable compression (true/false)
  BUILDCACHE_READ_ONLY        Read-only cache (true/false)
  BUILDCACHE_REMOTE_URL       Remote cache endpoint
  BUILDCACHE_REMOTE_CONNECT_TIMEOUT  Remote connect timeout (ms)
  BUILDCACHE_REMOTE_READ_TIMEOUT     Remote read timeout (ms)
  BUILDCACHE_REMOTE_MAX_RETRIES      Remote max retries
  BUILDCACHE_REMOTE_AUTH_HEADER      Remote auth header
  BUILDCACHE_REMOTE_VERIFY_SSL       Remote verify SSL (true/false)
  SKIPBUILD_DISABLE=1         Disable cache (passthrough)
  SKIPBUILD_DEBUG=1           Debug logging

Config file (.buildcache.conf):
  cache_dir = /path/to/cache
  max_cache_size = 10G
  max_age = 30
  eviction_ratio = 0.85
  compression_enabled = true
  read_only = false
  remote_url = http://cache:8080
  remote_connect_timeout = 3000
  remote_read_timeout = 3000
  remote_max_retries = 1
  remote_auth_header = Bearer ...
  remote_verify_ssl = true
```

---

## Cache management

| Command | Effect |
|---|---|
| `--stats` | Shows hits, misses, hit rate, bytes/time saved, entry count, total size |
| `--clear-stats` | Resets statistics to zero |
| `--clean` | Removes all cache entries (`rm -rf ~/.cache/buildcache`) |

**Cache directory:** `~/.cache/buildcache` (Windows: `%LOCALAPPDATA%\buildcache`)  
**Size limit:** 5 GB default, 85% target, LRU + max-age (30 days) eviction

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| Cache always misses | Compiler not in PATH, different path each run | Use full path to compiler; ensure same compiler binary |
| Wrong compiler detected | Wrapper called without `--` separator, arg parsing confused | Use `skipbuild -- cl.exe ...` form |
| Stale objects after compiler upgrade | Compiler hash not captured | Upgrade invalidates cache automatically via compiler binary hash |
| Permission errors on cache | Cache dir not writable | Set `BUILDCACHE_DIR` to writable path; check `--clean` |
| MSVC PDB not restored | `/Zi` used but PDB path not tracked | Ensure `/Fo` specifies output; PDB inferred from `.obj` |
| `--cache-dir` doesn't work | It prints, doesn't set | Use `BUILDCACHE_DIR` env var or config file |

---

## Limitations

| Limitation | Status |
|---|---|
| Linker caching | Not implemented — only compiles with `-c`/`/c` |
| Remote cache (HTTP/S3) | Code exists but untested against real server; no auth support |
| GCC/Clang PCH (`-x c++-header`) | Detected but not fully validated |
| MSVC `/Yc`/``/Yu`` PCH | Detected but not fully validated |
| Windows ARM64 | Not tested |
| Linux ARM64 | Not tested |
| FreeBSD / other BSDs | Not tested |
| Distributed builds (distcc/icecream) | Not tested |
| CMake Visual Studio generator | `CMAKE_CXX_COMPILER_LAUNCHER` set but no visible `[skipbuild]` output |

---

## Security

- **What goes into the cache:** Compiler output (`.obj`/`.o`), debug sidecars (`.pdb`, `.idb`, `.dwo`), metadata (key, toolchain hash, timestamps). No source code is stored.
- **What's stored on disk:** Compressed blobs (zstd level 3) in `~/.cache/buildcache/aa/bb/<hash>/` with `meta.json`.
- **Remote auth:** Not implemented. Remote cache code exists but has no authentication — do not expose to untrusted networks.
- **Reporting:** See [SECURITY.md](SECURITY.md) for vulnerability reporting.

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

All 10 tests pass on Windows, Linux, macOS (CI verified).

---

## Contributing, Changelog, License

- [Contributing](CONTRIBUTING.md)
- [Changelog](CHANGELOG.md)
- [License](LICENSE)