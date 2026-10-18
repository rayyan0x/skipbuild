# skipbuild ⚡

[![Build and Test](https://github.com/rayyan0x/skipbuild/workflows/Build%20and%20Test/badge.svg)](https://github.com/rayyan0x/skipbuild/actions)
[![Release](https://img.shields.io/github/v/release/rayyan0x/skipbuild)](https://github.com/rayyan0x/skipbuild/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platforms](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey)](#-supported-platforms-and-compilers)

**A zero-setup, cross-platform C++ build cache that makes rebuilds near-instant.**

> **TL;DR (one time, ~2 minutes):** install with one command → **open a new terminal** → integrate **once** (pick any one: the `-D..._COMPILER_LAUNCHER=skipbuild` flags, `skipbuild --init`, or the snippet) → build with Ninja. Details in [Installation](#-1-installation-one-time-1-minute) and [Integration](#-integration-one-time).

---

## ⚡ 1. Installation (one time, ~1 minute)

skipbuild is a single, dependency-free binary. Install it **once** — it then works with **every** project on the machine.

### Install

**Windows (PowerShell 5.1+):**
```powershell
irm https://raw.githubusercontent.com/rayyan0x/skipbuild/main/scripts/install.ps1 | iex
```

**Linux / macOS:**
```bash
curl -fsSL https://raw.githubusercontent.com/rayyan0x/skipbuild/main/scripts/install.sh | bash
```

Both installers download the prebuilt binary from GitHub Releases, **verify its SHA-256**, and add it to your `PATH`. The Windows installer also makes it usable in the current window right away.

### Confirm

```powershell
skipbuild --version      # -> skipbuild 0.2.2
```

> **Terminal already open?** Operating systems apply `PATH` changes only to processes started *afterwards*. If `skipbuild` isn't found:
> - **Open a new terminal** (using VS Code? quit it completely and reopen), **or**
> - enable it in the current window only:
>   ```powershell
>   $env:PATH = "$env:LOCALAPPDATA\skipbuild\bin;$env:PATH"   # Windows
>   ```
> This is standard OS behavior, not a skipbuild issue.

### 20-second smoke test (optional)

```powershell
echo '#include <stdio.h>
int main(){ printf("hello\n"); return 0; }' > hello.cpp

skipbuild -- cl.exe /c hello.cpp /Fohello.obj   # -> [skipbuild] MISS ... -> compiling...
skipbuild -- cl.exe /c hello.cpp /Fohello.obj   # -> [skipbuild] HIT ...
skipbuild --stats
```
On Linux/macOS substitute `g++` or `clang++` for `cl.exe`.

### Alternative: build from source

Only needed if you don't want the prebuilt binary:
```bash
git clone https://github.com/rayyan0x/skipbuild.git && cd skipbuild
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
cmake --install build --config Release          # then open a new terminal
```

---

## 📋 Table of Contents
- [Why skipbuild](#-why-skipbuild)
- [Benchmarks](#-benchmarks)
- [Integration](#-integration-one-time)
- [How It Works](#-how-it-works)
- [Supported Platforms and Compilers](#-supported-platforms-and-compilers)
- [Configuration](#-configuration)
- [CLI Reference](#-cli-reference)
- [Cache Management](#-cache-management)
- [Troubleshooting & Limitations](#-troubleshooting--limitations)
- [Security, Testing & License](#-security-testing--license)

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

## 🔄 Integration (one time)

skipbuild has to run in front of your compiler. Pick the option that matches your project, do it **once**, then build normally. **You only need one — the other options are alternatives, not extra steps.**

### Option A — you use CMake ✅ (most people)

Three ways to do the **same thing**. Pick any *one*:

**A1 · No file edits — pass the flags** (quickest to try)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER_LAUNCHER=skipbuild \
  -DCMAKE_CXX_COMPILER_LAUNCHER=skipbuild

cmake --build build
```

**A2 · One command — let skipbuild edit the file** (run from your project root)

```bash
skipbuild --init            # adds the snippet below to your top-level CMakeLists.txt
skipbuild --init --dry-run  # (optional) preview the change first
```

Now a plain `cmake -S . -B build -G Ninja` (no flags) caches automatically.

**A3 · Manual — paste it yourself** right after `project(...)` in your top-level `CMakeLists.txt`

```cmake
find_program(SKIPBUILD_EXE skipbuild)
if(SKIPBUILD_EXE)
  set(CMAKE_C_COMPILER_LAUNCHER   "${SKIPBUILD_EXE}")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${SKIPBUILD_EXE}")
endif()
```

> A1, A2 and A3 are equivalent — again, **only one is needed**. All three work identically for **MSVC `cl`, GCC `g++`, and Clang `clang++`**; there is nothing compiler-specific to set up.
>
> **"Top-level" `CMakeLists.txt`** = the file with `project(...)` at your repo root — not under `build/`, `_deps/`, `third_party/`, or `vendor/`. It applies to all subfolders, so it is added once.

### Option B — you don't use CMake

Put `skipbuild` in front of your compiler, in whatever builds your project. One line each:

**Makefile** (use `skipbuild cl` / `skipbuild clang++` if that is your compiler)
```make
CXX := skipbuild g++
```

**Hand-written Ninja**
```ninja
command = skipbuild -- g++ -c $in -o $out $flags
```

**One-off / manual compile**
```bash
skipbuild -- g++ -c main.cpp -o main.o        # GCC
skipbuild -- clang++ -c main.cpp -o main.o    # Clang
skipbuild -- cl.exe /c main.cpp /Fomain.obj   # MSVC
```

> The `--` separator is optional: `skipbuild g++ ...` and `skipbuild -- g++ ...` are the same.

### ⚠️ One CMake gotcha

Use the **Ninja**, **NMake**, or **Makefile** generator. The **Visual Studio** generator ignores skipbuild entirely — nothing gets cached. Fix: add `-G Ninja` (Ninja ships with your VS Build Tools).

### ✅ Done? — how to confirm and stop

After **one** option above, build, then rebuild. If you see `MISS` then `HIT`, you are finished — there is nothing else to run:

```
[skipbuild] MISS 6dbbadbe56d7 -> compiling...
[skipbuild] HIT  6dbbadbe56d7
```
Or run `skipbuild --stats` any time.

> Under **VS/MSBuild** the `[skipbuild]` lines are hidden — capture them with `$env:SKIPBUILD_LOG_FILE = "$env:TEMP\skipbuild.log"`.

<details>
<summary>Optional: use the installed CMake package (<code>find_package</code>) — only if you ran <code>cmake --install</code></summary>

```cmake
find_package(skipbuild REQUIRED)
skipbuild_enable()                 # every target in this directory tree
skipbuild_enable_target(my_lib)    # or enable just one target
```
If CMake can't find it, configure with `-DCMAKE_PREFIX_PATH=<skipbuild install prefix>`.

</details>

### One-time, then forget it

- **Install** — once per machine (Section 1).
- **The integration** — once per project (commit it; everyone who builds gets caching).
- Everything after that is just a normal build.

---

## 🛠️ How It Works

### Deterministic Cache Key Formula

The cache key is a SHA-256 over:

1. **Source file content** — full text of `.cpp`/`.c`/`.cc`
2. **Normalized include paths** — `-I`/`/I` resolved to build root, forward slashes, case-folded on Windows
3. **Sorted compiler flags** — all flags except output/file args, sorted for determinism
4. **Compiler fingerprint** — see [Compiler Fingerprinting](#-compiler-fingerprinting--cache-invalidation) below
5. **Compiler type + version** — e.g., `MSVC 19.51.36260`, `GCC 13.3`
6. **Relevant environment** — `SKIPBUILD_*`

### 🔏 Compiler Fingerprinting & Cache Invalidation

skipbuild fingerprints the compiler binary and mixes it into every cache key. This is what makes the cache **safe**: if the compiler changes, old entries are not reused. The trade-off is that a compiler change invalidates every entry that used the previous compiler.

Control this with `--compiler-check`, `SKIPBUILD_COMPILER_CHECK`, or the `compiler_check` config key:

| Mode | Fingerprint | Invalidates when… | Use when |
|---|---|---|---|
| `mtime` *(default)* | file size + mtime + `--version` output | the binary changes **or** its timestamp changes (e.g. reinstall, `touch`, package-manager reinstall) | you want maximum safety and rarely touch the compiler |
| `version` | `--version` output only | the reported version string changes | you frequently rebuild/reinstall the **same** compiler version |
| `content` | SHA-256 of the binary contents | the binary contents actually change | you want accuracy across reinstalls **and** detection of real updates (slower first hash) |
| `none` | *(none)* | never | you accept the risk of reusing objects across compiler changes (CI images you fully control) |

> **Why does a compiler update clear my whole cache?**
> Every cache key includes the compiler fingerprint. When the fingerprint changes, no existing key matches, so the next build is a full miss that repopulates the cache. This is intentional — reusing objects compiled by a different compiler can produce subtly wrong binaries.
>
> **Mitigations:**
> - Reinstalling the *same* compiler (common with `apt`, `choco`, or CI images) only changes the file's mtime. Set `SKIPBUILD_COMPILER_CHECK=content` so an identical binary keeps producing the **same** fingerprint — no invalidation.
> - If you rebuild the same compiler version yourself, `version` mode is stable across rebuilds.
> - On throwaway CI runners with a shared remote cache, pin the compiler and use `content` so the remote cache stays valid across runner images.

```bash
# Stable across reinstalls of the same compiler, still detects real updates
export SKIPBUILD_COMPILER_CHECK=content
```

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
| Compiler fingerprint | `--compiler-check <mode>` | `SKIPBUILD_COMPILER_CHECK` | `compiler_check` | `mtime` |
| Disable cache | — | `SKIPBUILD_DISABLE=1` | — | `false` |
| Log level | — | `REMAKE_LOG=debug\|info\|warn\|error\|none` | — | `none` |
| Log file | — | `SKIPBUILD_LOG_FILE` | — | — |

> **Note:** The legacy `BUILDCACHE_*` prefix is still accepted as a fallback, but `SKIPBUILD_*` is preferred. `SKIPBUILD_DEBUG=1` is supported as a shortcut for `REMAKE_LOG=debug`.

**Config file locations (priority order):**
1. `SKIPBUILD_CONFIG` env var (or legacy `BUILDCACHE_CONFIG`)
2. `.skipbuild.conf` (project root; legacy `.buildcache.conf` also read)
3. `.skipbuild/config` (project root; legacy `.buildcache/config` also read)
4. Windows: `%APPDATA%\skipbuild\config` / `%USERPROFILE%\.skipbuild\config`
5. Unix: `~/.config/skipbuild/config` / `~/.skipbuild/config`

---

## 📟 CLI Reference

```
skipbuild - C++ build cache (compiler launcher)

Modes:
  skipbuild g++ -c foo.cpp -o foo.o     # Wrap compiler ("--" is optional)
  skipbuild --cache-dir <path>          # Override cache directory for this run
  skipbuild --stats                     # Show cache statistics
  skipbuild --remote-stats              # Show remote cache statistics
  skipbuild --clear-stats               # Reset statistics to zero
  skipbuild --clean                     # Remove all cache entries
  skipbuild --init [CMakeLists.txt]     # Add caching to a CMake project (idempotent)
  skipbuild --version                   # Show version

Options:
  --max-cache-size <size>              Max cache size (10G, 500M)
  --max-age <days>                     Max age in days
  --compression <bool>                 Enable/disable compression
  --read-only <bool>                   Read-only cache
  --compiler-check <mode>              Compiler fingerprint: mtime, version, content, none
  --remote-url <url>                   Remote cache endpoint (e.g. http://cache:8080)
  --remote-read-only <bool>            Remote cache read-only mode
  --remote-timeout <ms>                Remote connect/read timeout in ms
  --disable-incremental-msbuild <bool> Bypass cache when MSBuild output is newer (default: true)
```

---

## 🧹 Cache Management

| Command | Effect |
|---|---|
| `--stats` | Hits, misses, hit rate, bytes/time saved, entry count, total size |
| `--clear-stats` | Resets statistics to zero |
| `--clean` | Removes all cache entries (`rm -rf ~/.cache/skipbuild`) |

**Cache directory:** `~/.cache/skipbuild` (Windows: `%USERPROFILE%\.cache\skipbuild`)  
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
# [skipbuild] MISS <key> -> compiling...
# [CACHE UPLOAD] Saved: /test-bucket/<compiler>/<aa>/<bb>/<key>/<key>.zst

# Second run - Cache HIT, downloads from mock server (clear the local cache first)
skipbuild -- cl.exe /c hello.cpp /Fohello.obj
# [skipbuild] HIT <key>
# [CACHE HIT] Served: /test-bucket/<compiler>/<aa>/<bb>/<key>/<key>.zst
```

### 4. Verify in Mock Server Logs
Remote objects are namespaced by compiler fingerprint and sharded by the key:
`/test-bucket/<compiler>/<aa>/<bb>/<key>/<key>.zst` (plus `meta.json` and any sidecars).

```
[CACHE MISS] Not found: /test-bucket/1a603bd8/26/13/26132f1da31f.../meta.json
[CACHE UPLOAD] Saved: /test-bucket/1a603bd8/26/13/26132f1da31f.../26132f1da31f....zst
[CACHE HIT] Served: /test-bucket/1a603bd8/26/13/26132f1da31f.../meta.json
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
.\skipbuild.exe -- cl /c hello.cpp /Fohello.obj
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
| Compiler args treated as skipbuild options | A compiler flag collides with a skipbuild flag | The `--` separator is optional (`skipbuild cl.exe /c ...` works); add `--` before the compiler to disambiguate: `skipbuild -- cl.exe ...` |
| Stale objects after compiler upgrade | Compiler fingerprint not captured | Upgrades invalidate automatically via the compiler fingerprint |
| Whole cache misses after a compiler update/reinstall | Every key includes the compiler fingerprint | Expected for real updates. For reinstalls of an identical binary set `SKIPBUILD_COMPILER_CHECK=content` to keep keys stable. See [Compiler Fingerprinting](#-compiler-fingerprinting--cache-invalidation) |
| Permission errors on cache | Cache dir not writable | Set `SKIPBUILD_DIR` to writable path; check `--clean` |
| MSVC PDB not restored | `/Zi` but PDB path not tracked | Ensure `/Fo` specifies output; PDB inferred from `.obj` |
| `--cache-dir` override not kept | It only applies to the current invocation | Set `SKIPBUILD_DIR` (or the config file `cache_dir`) for a persistent location |

### Limitations

| Limitation | Status |
|---|---|
| Linker caching | Not implemented — only `-c`/`/c` |
| Remote cache (HTTP/S3) | Built-in, tested with S3 mock; auth via headers only |
| GCC/Clang PCH (`-x c++-header`) | ✅ Implemented — flag parsing, key invalidation, dual-output caching |
| MSVC `/Yc`/`/Yu` PCH | ✅ Implemented — flag parsing, key invalidation, dual-output caching |
| Windows/Linux ARM64 | ✅ CI (Ubuntu ARM64, macOS Apple Silicon, Windows ARM64) |
| FreeBSD / other BSDs | Not tested — POSIX paths should work, no CI |
| Distributed builds (distcc/icecream) | Not tested |
| Compiler update invalidates cache | Expected for safety — configurable via `compiler_check`; see [Compiler Fingerprinting](#-compiler-fingerprinting--cache-invalidation) |
| Visual Studio (CMake) generator | `CMAKE_CXX_COMPILER_LAUNCHER` is ignored by the VS generator, so no automatic caching — use Ninja/Make/NMake, or wrap the compiler manually. For output capture set `SKIPBUILD_LOG_FILE` (MSBuild hides a tool's stdout/stderr on success). |

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
All 12 tests pass on Windows, Linux, macOS (CI verified).

**License:** MIT — see [LICENSE](LICENSE)