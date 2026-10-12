# skipbuild ⚡

[![Build and Test](https://github.com/rayyan0x/skipbuild/workflows/Build%20and%20Test/badge.svg)](https://github.com/rayyan0x/skipbuild/actions)
[![Release](https://img.shields.io/github/v/release/rayyan0x/skipbuild)](https://github.com/rayyan0x/skipbuild/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platforms](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey)](#supported-platforms-and-compilers)

**A zero-setup, cross-platform C++ build cache that makes rebuilds near-instant.**

---

## ⚡ 1. Installation & Quick Start

### Step 1: Build from Source
```bash
git clone https://github.com/rayyan0x/skipbuild.git
cd skipbuild
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

### Step 2: Environment Setup / Install

| Platform | Command |
|---|---|
| **macOS / Linux** | `curl -fsSL https://raw.githubusercontent.com/rayyan0x/skipbuild/main/scripts/install.sh \| bash` |
| **Windows (PowerShell 5.1+)** | `irm https://raw.githubusercontent.com/rayyan0x/skipbuild/main/scripts/install.ps1 \| iex` |

> **Note:** Pre-built binaries from GitHub Releases. Scripts verify checksums and add to PATH.

**From source build:**
```bash
cmake --install build --config Release --prefix ~/.local   # Linux/macOS
cmake --install build --config Release --prefix $env:USERPROFILE\.local  # Windows
```

### Step 3: 30-Second Verification Test
```powershell
# Developer PowerShell for VS 2022
echo '#include <stdio.h>
int main() { printf("Hello, skipbuild!\n"); return 0; }' > hello.cpp

# Cold run (MISS)
build\Release\skipbuild.exe -- cl.exe /c hello.cpp /Fohello.obj
# [skipbuild] MISS 48476142e635 -> compiling...

# Hot run (HIT)
build\Release\skipbuild.exe -- cl.exe /c hello.cpp /Fohello.obj
# [skipbuild] HIT 48476142e635

# Verify stats
build\Release\skipbuild.exe --stats
```

---

## 📋 Table of Contents
- [Why skipbuild](#why-skipbuild)
- [Benchmarks](#benchmarks)
- [Integration](#integration)
- [How It Works](#how-it-works)
- [Supported Platforms and Compilers](#supported-platforms-and-compilers)
- [Configuration](#configuration)
- [CLI Reference](#cli-reference)
- [Cache Management](#cache-management)
- [Troubleshooting & Limitations](#troubleshooting--limitations)
- [Security, Testing & License](#security-testing--license)

---

## 💡 Why skipbuild

### The Problem
Large C++ projects suffer from 40+ minute rebuilds. Changing one file, switching git branches, or updating a compiler forces recompilation of everything.

### What it Does
- Wraps your compiler, hashes everything affecting output (source, includes, flags, compiler binary, env)
- Restores cached objects when inputs haven't changed
- Native MSVC `/Zi` PDB + `.idb` support, deterministic cross-platform keys
- Zero external dependencies (WinCrypto / POSIX sockets, no OpenSSL/cURL)
- Transparent fallback — cache errors never break the build

### What it Doesn't Do
- Does not replace your build system
- Does not cache linking (only `-c`/`/c` compilation)
- Does not require a remote cache server — local works out of the box

---

## 📊 Benchmarks

Measured on **50 source files × 10 headers**, `-O2 -std=c++20`.

| Scenario | Time |
|---|---|
| Cold build (no cache) | 22.0 s |
| Hot rebuild (all cache hits) | 0.2 s |
| **Speedup** | **~110x** |
| Overhead per file on cache miss | ~4 ms (<1% of compile time) |

**Hardware/OS/Compilers:** Windows 11 (MSVC 19.51) / Ubuntu 24.04 (GCC 13.3) / macOS 26 (Apple Clang 21)

---

## 🔄 Integration

### A. Global / Automatic Integration

| Build System | One-Line Config |
|---|---|
| **CMake (Ninja/Make)** | `set(CMAKE_CXX_COMPILER_LAUNCHER "skipbuild")` |
| **CMake (per-target)** | `set_target_properties(tgt PROPERTIES CXX_COMPILER_LAUNCHER "skipbuild")` |
| **Ninja** | `rule cxx: command = skipbuild -- g++ -c $in -o $out $flags` |
| **Make** | `CXX := skipbuild g++` |

> **Note:** VS generator sets launcher but shows no `[skipbuild]` output in logs.

### B. Isolated / Target-Specific Configuration

**After install:**
```cmake
find_package(skipbuild REQUIRED)
skipbuild_enable_target(my_target)
```

**MSVC manual (verified):**
```powershell
skipbuild -- cl.exe /c hello.cpp /Fohello.obj
```

**Generic wrapper:**
```bash
skipbuild -- clang++ -c foo.cpp -o foo.o -Iinclude -O2
```

---

## 🛠️ How It Works

### Deterministic Cache Key Formula

The cache key is a SHA-256 over:

1. **Source file content** — full text of `.cpp`/`.c`/`.cc`
2. **Normalized include paths** — `-I`/`/I` resolved to build root, forward slashes, case-folded on Windows
3. **Sorted compiler flags** — all flags except output/file args, sorted for determinism
4. **Compiler binary hash** — SHA-256 of `cl.exe`, `g++`, `clang++`
5. **Compiler type + version** — e.g., `MSVC 19.51.36260`, `GCC 13.3`
6. **Relevant environment** — `BUILDCACHE_*`, `SKIPBUILD_*`

### Hit / Miss / Store Flow

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
  compress (zstd level 3) + sidecars → store atomically
       │
       ▼
  return success (MISS)
```

### Excluded Invocations & Boundaries

| Not Cached | Reason |
|---|---|
| Linker invocations (`link.exe`, `ld`, `lld`) | Too many inputs, non-deterministic timestamps |
| Commands without `-c`/`/c` | Produces executable/shared library, not object file |
| Compiler detection failures | Cannot compute reliable key |
| `SKIPBUILD_DISABLE=1` set | Explicit opt-out for debugging |

---

## 🖥️ Supported Platforms and Compilers

| OS | Compiler | Version Tested | CI Status |
|---|---|---|---|
| Windows | MSVC | 19.51 (2022 BuildTools) | ✅ Verified on local PC |
| Linux | GCC | 13.3 (Ubuntu 24.04) | ✅ CI green |
| macOS | Apple Clang | 21 (macOS 26) | ✅ CI green |

**CI Run:** https://github.com/rayyan0x/skipbuild/actions/runs/38118686561 — All 3 jobs green

> Minimums: MSVC 19.30+, GCC 11+, Clang 13+ (C++20 requirement)

---

## ⚙️ Configuration

**Precedence:** CLI flags > Environment variables > Config file

| Setting | CLI Flag | Environment Variable | Config Key | Default |
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

> **Note:** Tool is `skipbuild` but uses `BUILDCACHE_*` env vars (legacy). `SKIPBUILD_*` only for `DISABLE`/`DEBUG`.

**Config file locations (priority order):**
1. `BUILDCACHE_CONFIG` env var
2. `.buildcache.conf` (project root)
3. `.buildcache/config` (project root)
4. Windows: `%APPDATA%\buildcache\config` / `%USERPROFILE%\.buildcache\config`
5. Unix: `~/.config/buildcache/config` / `~/.buildcache/config`

---

## 📟 CLI Reference

```
skipbuild - C++ build cache (compiler launcher)

Modes:
  skipbuild g++ -c foo.cpp -o foo.o     # Wrap compiler
  skipbuild --cache-dir <path>          # Print cache directory (does not set)
  skipbuild --stats                     # Show cache statistics
  skipbuild --clear-stats               # Reset statistics to zero
  skipbuild --clean                     # Remove all cache entries
  skipbuild --version                   # Show version

Options:
  --max-cache-size <size>   Max cache size (10G, 500M)
  --max-age <days>          Max age in days
  --compression <bool>      Enable/disable compression
  --read-only <bool>        Read-only cache
```

---

## 🧹 Cache Management

| Command | Effect |
|---|---|
| `--stats` | Hits, misses, hit rate, bytes/time saved, entry count, total size |
| `--clear-stats` | Resets statistics to zero |
| `--clean` | Removes all cache entries (`rm -rf ~/.cache/buildcache`) |

**Cache directory:** `~/.cache/buildcache` (Windows: `%LOCALAPPDATA%\buildcache`)  
**Size limit:** 5 GB default, 85% target, LRU + max-age (30 days) eviction

---

## 🔍 Troubleshooting & Limitations

### Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| Cache always misses | Compiler not in PATH, different path each run | Use full path; ensure same compiler binary |
| Wrong compiler detected | Called without `--` separator | Use `skipbuild -- cl.exe ...` |
| Stale objects after compiler upgrade | Compiler hash not captured | Upgrade invalidates automatically via binary hash |
| Permission errors on cache | Cache dir not writable | Set `BUILDCACHE_DIR` to writable path; check `--clean` |
| MSVC PDB not restored | `/Zi` but PDB path not tracked | Ensure `/Fo` specifies output; PDB inferred from `.obj` |
| `--cache-dir` doesn't work | It prints, doesn't set | Use `BUILDCACHE_DIR` env or config file |

### Limitations

| Limitation | Status |
|---|---|
| Linker caching | Not implemented — only `-c`/`/c` |
| Remote cache (HTTP/S3) | Code exists, untested vs real server, no auth |
| GCC/Clang PCH (`-x c++-header`) | Detected, not fully validated |
| MSVC `/Yc`/`/Yu` PCH | Detected, not fully validated |
| Windows/Linux ARM64 | Not tested |
| FreeBSD / other BSDs | Not tested |
| Distributed builds (distcc/icecream) | Not tested |
| VS generator | Launcher set but no visible `[skipbuild]` output |

---

## 🛡️ Security, Testing & License

**Security:**
- Cache stores: compiler output (`.obj`/`.o`), debug sidecars (`.pdb`, `.idb`, `.dwo`), metadata (key, toolchain hash, timestamps). No source code.
- Disk: zstd level 3 compressed blobs in `~/.cache/buildcache/aa/bb/<hash>/` with `meta.json`
- Remote auth: Not implemented — do not expose to untrusted networks
- Reporting: See [SECURITY.md](SECURITY.md)

**Testing:**
```bash
ctest --test-dir build -C Release --output-on-failure --timeout 120
```
All 10 tests pass on Windows, Linux, macOS (CI verified).

**License:** MIT — see [LICENSE](LICENSE)