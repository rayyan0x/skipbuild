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

**macOS / Linux:**
```bash
curl -fsSL https://raw.githubusercontent.com/rayyan0x/skipbuild/main/scripts/install.sh | bash
```

**Windows (PowerShell 5.1+):**
```powershell
irm https://raw.githubusercontent.com/rayyan0x/skipbuild/main/scripts/install.ps1 | iex
```

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
- **⚠️ Does not cache linking** — only caches compilation (`-c`/`/c`). For large C++ projects, linking is often 30–50% of total build time. This is a deliberate design choice (linker inputs are complex and non-deterministic).
- Does not require a remote cache server — local works out of the box

### skipbuild vs ccache vs sccache

| Feature | **skipbuild** | **ccache** | **sccache** |
|---|---|---|---|
| **MSVC `/Zi` PDB + `.idb`** | ✅ Native | ❌ Limited | ⚠️ Experimental |
| **Deterministic cross-platform keys** | ✅ | ❌ | ✅ |
| **Zero external dependencies** | ✅ WinCrypto / POSIX | ❌ needs zlib | ❌ needs OpenSSL |
| **Windows-first design** | ✅ | ❌ Linux-first | ⚠️ Works |
| **Remote cache (HTTP/S3)** | ✅ Built-in | ❌ | ✅ S3/GCS/Azure |
| **Remote auth** | ✅ Headers | ❌ | ✅ IAM/headers |
| **MSBuild incremental friendly** | ⚠️ Overhead on no-changes | ⚠️ Same | ⚠️ Same |
| **Ninja/Make incremental** | ✅ Fast | ✅ Fast | ✅ Fast |
| **Compiler binary hash invalidation** | ✅ Automatic | ✅ | ✅ |
| **Sidecar tracking (PDB, IDB, DWO)** | ✅ Native | ❌ | ⚠️ Partial |
| **Remote auth / private caches** | ✅ Headers | ❌ | ✅ IAM/headers |
| **Linker caching** | ❌ | ❌ | ❌ |
| **Single binary, no runtime deps** | ✅ | ❌ | ❌ (needs SSL) |

> **TL;DR:** Use **skipbuild** for Windows/MSVC projects needing native PDB support, zero deps, and cross-platform deterministic keys. Use **ccache** for Linux-only GCC/Clang projects. Use **sccache** if you need distributed compilation with S3/GCS remote cache.

---

## 📊 Benchmarks

Real-world measurements on **fmtlib** (~50 source files, CMake, MSVC 19.51, Windows 11, 12-core i7):

| Scenario | Without skipbuild | With skipbuild | Notes |
|---|---|---|---|
| **Cold build** (first build) | 141 s | 141 s | All misses — same compiler work |
| **Clean rebuild** (after previous build) | 141 s | **3.2 s** | All cache hits — **~44x speedup** |
| **Switch branches** (git checkout) | 141 s | **3.5 s** | All hits — **~40x speedup** |
| **Incremental** (touch 1 file) | 15.6 s | 15.8 s | MSBuild incremental faster than cache lookup |
| **Hot rebuild** (no changes) | 6.6 s | 10.0 s | MSBuild skips compiler; skipbuild adds overhead |

> **Key insight:** skipbuild (like ccache/sccache) shines on **clean rebuilds, branch switches, and CI fresh runners** — not on incremental "no changes" builds. MSBuild's native incremental is already faster than cache lookup overhead.

> **Don't worry:** skipbuild auto-detects MSBuild incremental builds (output newer than source) and **bypasses the cache entirely** — you get native MSBuild speed with zero config.

**Measured on:** fmtlib v12.2.1 (~50 source files), MSVC 19.51, Windows 11, 12-core i7-13700K, `-O2 -std=c++20`.

**Overhead per file on cache miss:** ~4 ms (<1% of compile time)

**Where skipbuild shines:**
| Scenario | Speedup |
|---|---|
| Clean rebuild (after previous build) | **~44x** |
| Switch git branches | **~40x** |
| CI fresh runner (shared cache) | **10–100x** |
| Delete build dir, rebuild | **~44x** |

> **Note:** For daily incremental dev, use **Ninja generator** (`cmake -G Ninja`) with skipbuild — MSBuild's native incremental is already optimal (and skipbuild auto-disables for it anyway).

---

## 🔄 Integration

### A. Global / Automatic Integration

**CMake (Ninja/Make generators):**
```cmake
set(CMAKE_CXX_COMPILER_LAUNCHER "skipbuild")
```

**CMake (per-target):**
```cmake
set_target_properties(my_target PROPERTIES
    CXX_COMPILER_LAUNCHER "skipbuild"
)
```

**Ninja (`build.ninja`):**
```ninja
rule cxx
  command = skipbuild -- g++ -c $in -o $out $flags

build main.o: cxx main.cpp
```

**Make (`Makefile`):**
```make
CXX := skipbuild g++
CXXFLAGS := -Iinclude -O2

main.o: main.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@
```

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
6. **Relevant environment** — `SKIPBUILD_*`

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
| Cache directory | `--cache-dir <path>` | `SKIPBUILD_DIR` / `SKIPBUILD_CACHE_DIR` | `cache_dir` | `~/.cache/skipbuild` |
| Max cache size | `--max-cache-size <size>` | `SKIPBUILD_MAX_SIZE` | `max_cache_size` | `5G` |
| Max age (days) | `--max-age <days>` | `SKIPBUILD_MAX_AGE` | `max_age` | `30` |
| Eviction ratio | — | `SKIPBUILD_EVICTION_RATIO` | `eviction_ratio` | `0.85` |
| Compression | `--compression <bool>` | `SKIPBUILD_COMPRESSION` | `compression_enabled` | `true` |
| Read-only | `--read-only <bool>` | `SKIPBUILD_READ_ONLY` | `read_only` | `false` |
| Remote URL | — | `SKIPBUILD_REMOTE_URL` | `remote_url` | — |
| Remote connect timeout (ms) | — | `SKIPBUILD_REMOTE_CONNECT_TIMEOUT` | `remote_connect_timeout` | `3000` |
| Remote read timeout (ms) | — | `SKIPBUILD_REMOTE_READ_TIMEOUT` | `remote_read_timeout` | `3000` |
| Remote max retries | — | `SKIPBUILD_REMOTE_MAX_RETRIES` | `remote_max_retries` | `1` |
| Remote auth header | — | `SKIPBUILD_REMOTE_AUTH_HEADER` | `remote_auth_header` | — |
| Remote verify SSL | — | `SKIPBUILD_REMOTE_VERIFY_SSL` | `remote_verify_ssl` | `true` |
| Disable cache | — | `SKIPBUILD_DISABLE=1` | — | `false` |
| Log level | — | `REMAKE_LOG=debug\|info\|warn\|error\|none` | — | `none` |

> **Note:** The legacy `BUILDCACHE_*` prefix is still accepted as a fallback, but `SKIPBUILD_*` is preferred. `SKIPBUILD_DEBUG=1` is supported as a shortcut for `REMAKE_LOG=debug`.

**Config file locations (priority order):**
1. `SKIPBUILD_CONFIG` env var (or legacy `BUILDCACHE_CONFIG`)
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
| `--clean` | Removes all cache entries (`rm -rf ~/.cache/skipbuild`) |

**Cache directory:** `~/.cache/skipbuild` (Windows: `%LOCALAPPDATA%\skipbuild`)  
**Size limit:** 5 GB default, 85% target, LRU + max-age (30 days) eviction

---

## ☁️ Remote Caching

skipbuild supports remote caching via HTTP/S3-compatible APIs. Configure with:

| Setting | Environment Variable | Config Key |
|---|---|---|
| Remote URL | `SKIPBUILD_REMOTE_URL` | `remote_url` |
| Connect timeout (ms) | `SKIPBUILD_REMOTE_CONNECT_TIMEOUT` | `remote_connect_timeout` |
| Read timeout (ms) | `SKIPBUILD_REMOTE_READ_TIMEOUT` | `remote_read_timeout` |
| Max retries | `SKIPBUILD_REMOTE_MAX_RETRIES` | `remote_max_retries` |
| Auth header | `SKIPBUILD_REMOTE_AUTH_HEADER` | `remote_auth_header` |
| Verify SSL | `SKIPBUILD_REMOTE_VERIFY_SSL` | `remote_verify_ssl` |

**Example (S3-compatible):**
```bash
export SKIPBUILD_REMOTE_URL="http://127.0.0.1:9000/my-bucket"
export SKIPBUILD_REMOTE_AUTH_HEADER="Authorization: Bearer my-token"
export SKIPBUILD_REMOTE_VERIFY_SSL=false
skipbuild -- cl.exe /c hello.cpp /Fohello.obj
```

### Local Testing Guide: Remote S3 Caching

You can easily verify the end-to-end remote caching loop (Cache Miss $\rightarrow$ Upload $\rightarrow$ Cache Hit) using a lightweight Python S3 mock server without needing complex MinIO or AWS infrastructure.

### 1. Start the Local S3 Mock Server
Create a script named `s3_mock.py`:

```python
import http.server
import os
import urllib.parse

PORT = 9000
STORAGE_DIR = r"./s3_data"
os.makedirs(STORAGE_DIR, exist_ok=True)

class S3MockHandler(http.server.BaseHTTPRequestHandler):
    def do_PUT(self):
        path = urllib.parse.urlparse(self.path).path
        file_path = os.path.join(STORAGE_DIR, path.lstrip("/"))
        os.makedirs(os.path.dirname(file_path), exist_ok=True)
        data = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        with open(file_path, "wb") as f:
            f.write(data)
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"OK")
        print(f"[CACHE UPLOAD] Saved: {path}")

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        file_path = os.path.join(STORAGE_DIR, path.lstrip("/"))
        if os.path.exists(file_path):
            self.send_response(200)
            self.end_headers()
            with open(file_path, "rb") as f:
                self.wfile.write(f.read())
            print(f"[CACHE HIT] Served: {path}")
        else:
            self.send_response(404)
            self.end_headers()
            print(f"[CACHE MISS] Not found: {path}")

    def do_HEAD(self):
        path = urllib.parse.urlparse(self.path).path
        file_path = os.path.join(STORAGE_DIR, path.lstrip("/"))
        self.send_response(200 if os.path.exists(file_path) else 404)
        self.end_headers()

if __name__ == "__main__":
    server = http.server.HTTPServer(("127.0.0.1", PORT), S3MockHandler)
    print(f"🚀 S3 Mock Server running on [http://127.0.0.1](http://127.0.0.1):{PORT}")
    server.serve_forever()
```

Run it:
```bash
python s3_mock.py
```

### 2. Configure skipbuild to Use the Mock Server
```powershell
$env:SKIPBUILD_REMOTE_URL = "http://127.0.0.1:9000/test-bucket"
$env:SKIPBUILD_REMOTE_VERIFY_SSL = "false"
$env:SKIPBUILD_REMOTE_CONNECT_TIMEOUT = "2000"
$env:SKIPBUILD_REMOTE_READ_TIMEOUT = "2000"
```

### 3. Test the Cache Loop
```powershell
# First run - Cache MISS, compiles and uploads
skipbuild -- cl.exe /c hello.cpp /Fohello.obj
# [skipbuild] MISS <hash> -> compiling...
# [CACHE UPLOAD] Saved: /test-bucket/<hash>.zst

# Second run - Cache HIT, downloads from mock server
skipbuild -- cl.exe /c hello.cpp /Fohello.obj
# [skipbuild] HIT <hash>
# [CACHE HIT] Served: /test-bucket/<hash>.zst
```

### 4. Verify in Mock Server Logs
```
[CACHE MISS] Not found: /test-bucket/48476142e635...
[CACHE UPLOAD] Saved: /test-bucket/48476142e635...
[CACHE HIT] Served: /test-bucket/48476142e635...
```

This confirms the full remote cache round-trip works: miss → compile → upload → hit → download.

### Production Usage: Team & CI Workflow

Run the mock server in Terminal 1:

```powershell
python s3_mock.py
```

Configure & Test in Terminal 2:

```powershell
# Point to your S3 remote cache endpoint
$env:SKIPBUILD_REMOTE_URL = "http://127.0.0.1:9000/skipbuild-cache"
$env:SKIPBUILD_COMPRESSION = "true"
$env:REMAKE_LOG = "debug"

# Run a test compilation (example using MSVC)
.\skipbuild.exe cl /c hello.cpp /Fohello.obj
```

**First Run:** Triggers a Cache Miss, compiles the file, and uploads the compressed `.zst` artifact and `meta.json` to the mock server (`[CACHE UPLOAD]`).

**Second Run:** Triggers an instant Cache Hit, skipping compilation entirely and restoring the object from the remote cache (`[CACHE HIT]`).

### Does this work for teams and larger files?

**Yes, absolutely.** This architecture is built precisely for team environments and large-scale codebases. Here is why it scales:

1. **Shared Team Remote Caching:**
   Instead of pointing `SKIPBUILD_REMOTE_URL` to a local mock server, production teams point it to a **shared S3 bucket** (like AWS S3, Cloudflare R2, MinIO cluster, or Google Cloud Storage via S3-compat APIs).
   * When **Developer A** compiles a large codebase for the first time, the objects are pushed to the central S3 bucket.
   * When **Developer B** (or a CI/CD pipeline like GitHub Actions) builds the exact same code changes later, they pull the pre-compiled objects instantly via **Cache Hit**, cutting build times down to seconds.

2. **Handling Large Files Efficiently:**
   * **Zstandard (`zst`) Compression:** As you saw in your mock server logs (`.zst`), `skipbuild` compresses compiled binary outputs before uploading them. This drastically reduces network payload size, making it fast even over standard internet connections.
   * **Content-Addressable Storage (CAS):** Because caching is keyed by cryptographic hashes of the file contents and compiler flags, identical artifacts are never uploaded twice. The bucket stores one copy per unique compilation result.

---

## 🔍 Troubleshooting & Limitations

### Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| Cache always misses | Compiler not in PATH, different path each run | Use full path; ensure same compiler binary |
| Wrong compiler detected | Called without `--` separator | Use `skipbuild -- cl.exe ...` |
| Stale objects after compiler upgrade | Compiler hash not captured | Upgrade invalidates automatically via binary hash |
| Permission errors on cache | Cache dir not writable | Set `SKIPBUILD_DIR` to writable path; check `--clean` |
| MSVC PDB not restored | `/Zi` but PDB path not tracked | Ensure `/Fo` specifies output; PDB inferred from `.obj` |
| `--cache-dir` doesn't work | It prints, doesn't set | Use `SKIPBUILD_DIR` env or config file |

### Limitations

| Limitation | Status |
|---|---|
| Linker caching | Not implemented — only `-c`/`/c` |
| Remote cache (HTTP/S3) | Built-in, tested with S3 mock; auth via headers only |
| GCC/Clang PCH (`-x c++-header`) | ✅ Implemented — flag parsing, key invalidation, dual-output caching |
| MSVC `/Yc`/`/Yu` PCH | ✅ Implemented — flag parsing, key invalidation, dual-output caching |
| Windows/Linux ARM64 | Not tested — code is portable, no CI coverage |
| FreeBSD / other BSDs | Not tested — POSIX paths should work, no CI |
| Distributed builds (distcc/icecream) | Not tested |
| VS generator logging | `[skipbuild]` output goes to stderr, not the MSBuild console log. Set `REMAKE_LOG=debug` and view the **Output → Debug** window in VS (uses `OutputDebugString`). |

---

## 🛡️ Security, Testing & License

**Security:**
- Cache stores: compiler output (`.obj`/`.o`), debug sidecars (`.pdb`, `.idb`, `.dwo`), metadata (key, toolchain hash, timestamps). No source code.
- Disk: zstd level 3 compressed blobs in `~/.cache/skipbuild/aa/bb/<hash>/` with `meta.json`
- Remote auth: Header-based (`SKIPBUILD_REMOTE_AUTH_HEADER`) — do not expose to untrusted networks without TLS
- Reporting: See [SECURITY.md](SECURITY.md)

**Testing:**
```bash
ctest --test-dir build -C Release --output-on-failure --timeout 120
```
All 11 tests pass on Windows, Linux, macOS (CI verified).

**License:** MIT — see [LICENSE](LICENSE)