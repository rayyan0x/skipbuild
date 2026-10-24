# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Installable CMake package.** `install()` now ships every public header (`core.hpp`, `config.hpp`, `stats.hpp`, `remote.hpp`, `crypto.hpp`) and exports `skipbuild::core`, `skipbuild::skipbuild` and `skipbuild::libzstd_static`, so `find_package(skipbuild)` + `target_link_libraries(app PRIVATE skipbuild::core)` works from an installed tree instead of only in the source tree.
- **Installer refuses unverified binaries.** Both install scripts treated a missing `SHA256SUMS` (or an asset absent from it) as "skip verification"; checksum lookup is now mandatory and fails closed, with the asset name matched exactly rather than by substring.
- **Windows installer supports ARM64.** The asset was hardcoded to `skipbuild-windows-x64.exe`; it is now derived from the process architecture (`PROCESSOR_ARCHITEW6432`/`PROCESSOR_ARCHITECTURE`).

### Fixed
- **CMake integration module is self-consistent.** `Findskipbuild.cmake` was installed to `share/cmake/Modules` but included `skipbuild.cmake` from `lib/cmake/skipbuild`, so module-mode `find_package` failed; both files are now installed side by side in both locations. `skipbuild_enable()` also called an undefined `_add_executable_with_skipbuild_impl` on CMake < 3.19; it now applies the launcher to existing targets and warns instead.
- **`skipbuild-config.cmake` is a real package config.** It self-assigned `SKIPBUILD_EXECUTABLE` and never loaded the exported targets; it now uses `@PACKAGE_INIT@`, includes `skipbuildTargets.cmake`, prefers the imported launcher, and calls `check_required_components()`.
- **CMake minimum version matches the policies used.** `cmake_policy(SET CMP0135 NEW)` requires CMake 3.24 while the project claimed 3.20; the minimum is now 3.24 and the `CMAKE_POLICY_VERSION_MINIMUM` fallback no longer forces a cache entry.
- **Composite action uses this repository.** `action.yml` still referenced `remake` (placeholder `yourname/remake` URLs, `REMAKE_CACHE_DIR`, `~/.cache/remake`, `remake-` cache keys) and never enabled the launcher; it now installs from the checked-out `scripts/`, sets `SKIPBUILD_CACHE_DIR`/`CMAKE_*_COMPILER_LAUNCHER`, and caches `~/.cache/skipbuild`. The stale duplicate `install.sh`/`install.ps1` at the repository root were removed.
- **Windows installer PATH check is exact.** `-notlike "*$InstallDir*"` used wildcard matching (and appended to a `$null` user PATH); entries are now compared exactly.
- **CI gates the release.** The sanitizer job was `continue-on-error` and the strict (warnings-as-errors) job never ran the tests, so `release` could publish a build that only the matrix job had validated; sanitizers are now blocking, `strict` runs `ctest`, and `release` requires `build`, `strict`, `sanitizers` and both launcher smoke jobs.
- **Tests actually test something.** `test_simple` only printed state and `test_multiprocess` never started a second process (it counted failures and returned 0), so both passed unconditionally; `test_simple` is gone and `test_multiprocess` now spawns four real worker processes and fails on any worker error or corrupted entry. A new `cli_e2e_test` drives `skipbuild -- <compiler>` through miss -> hit -> header-edit invalidation.
- **Removed dead development files.** `compile.bat`, `dummy_test.cpp`, `test.cpp`, `test_skipbuild.bat` and `verify_stats.ps1` were tracked scratch files containing absolute paths from a single developer machine.
- **HTTP responses are bounded.** Both clients buffered the whole body with no limit, so a broken or hostile server could exhaust the build machine's memory; bodies are now capped at 512 MiB and oversized responses fail the request. The POSIX client also reports a receive error/timeout instead of silently restoring a truncated body.
- **Remote artifacts are checksum-verified.** `meta.json` now records a SHA-256 of each uploaded artifact and downloads whose bytes do not match are rejected (legacy entries without checksums remain readable), so truncation or corruption cannot install a bad entry.
- **`--disable-incremental-msbuild` actually applies.** The CLI flag populated the override struct but no mask bit existed and `load_config()` never read the value, so passing it had no effect; it now overrides `disable_incremental_msbuild` like the other booleans.
- **Empty sidecars are not cached.** Zero-byte `.pdb`/`.idb`/`.dwo` placeholders were stored and advertised, making restores report an empty symbol file; they are now skipped on store.
- **Failed stores leave no partial entry.** A failure while writing blobs or `meta.json` used to leave an unusable directory (and stale blobs) at the key path; when the entry did not already exist, everything created by the failed store is now removed.
- **Pruning reclaims orphaned entries and temp files.** Directories without a `meta.json` (crashed/failed stores) were invisible to `collect_entries()` and never removed; `prune()` now deletes them (only when untouched for an hour, so an in-flight store is not disturbed) along with stale `*.tmp` files inside valid entries.
- **A leftover temp file no longer purges a good entry.** `get()` treated any zero-byte non-`.zst` file as corruption; interrupted-write `*.tmp` files are now ignored (and swept by `prune()`), so a valid entry survives.
- **Compile keys are unambiguous.** Key fields were concatenated with `|` without escaping, so `-I"a|b"` could collide with `-Ia -Ib`; fields are now length-prefixed. `::tolower` is also called through `unsigned char`, avoiding undefined behaviour on non-ASCII paths.
- **Debug builds are not shared across checkouts.** Keys normalized the source root away even when `-g`/`/Zi` (or an absolute path used by `__FILE__`) embeds host-specific paths in the object; debug keys now include the absolute source and compilation directory, while `-g0` and ordinary builds keep cross-checkout reuse.
- **`tellg()` failure is handled.** A `-1` result was converted to `size_t`, requesting a near-`SIZE_MAX` allocation when reading an artifact for upload; it is now treated as a read error.

## [0.6.0] - 2026-10-22

### Highlights
Cache-integrity and remote-transport release: correct lock release on Windows, accurate sidecar metadata, a working `compression_enabled` switch, enforced HTTP timeouts, and no plaintext HTTPS on platforms without TLS support.

### Changed
- **`compression_enabled` is honoured.** Disabling compression (config, `SKIPBUILD_COMPRESSION`, or `--compression false`) now stores artifacts uncompressed instead of being ignored; readers accept both compressed and raw entries, and `meta.json` records the actual layout.

### Fixed
- **File locks fully release on Windows.** `unlock()` tried to unlock a `MAXDWORD` range while only one byte was locked, which fails and left the lock held until the handle closed; it now unlocks the exact range and closes the handle, so a shared lock can be dropped and re-acquired exclusively (needed to purge corrupt entries) without leaking handles.
- **Sidecar metadata no longer leaks the storage suffix.** `meta.json` recorded the stored `.zst` name and the hash of the compressed blob, so the real sidecar name/hash never matched. It now records the original filename and content hash, and the restore path matches it.
- **Remote HTTP client timeouts are enforced.** The WinHTTP session passed `0` (infinite) for the receive timeout and for name resolution, so a server that accepted a connection but never replied could hang a build despite `remote_read_timeout`; resolve/connect now use the connect timeout and send/receive use the read timeout. On POSIX, where the raw-socket client has no TLS, `https://` URLs are refused (and leave the remote cache disabled) instead of sending a plaintext request to port 443.

## [0.5.0] - 2026-10-21

### Highlights
Correctness and cache-integrity release: CLI routing and argument passthrough, order-correct compile/link keys, safe cache reads/writes, enforced read-only modes, upward config discovery, meta-driven remote artifacts, and working max-age + LRU pruning.

### Added
- **Automatic cache clear-to-zero.** Once usage reaches `auto_clear_ratio` (default `0.95`) of `max_cache_size_bytes`, `put()` purges every entry and frees the space back to zero instead of waiting for the throttled LRU prune. Configurable via `auto_clear_ratio`/`auto_clear` in the config file or `SKIPBUILD_AUTO_CLEAR_RATIO`/`BUILDCACHE_AUTO_CLEAR_RATIO`; `<= 0` disables it.

### Changed
- **Compile keys preserve argument order.** Include search directories and flags (`-D`/`-U`/`-I`, …) are no longer sorted before hashing, since their order is semantically significant (`-DFOO -UFOO` differs from `-UFOO -DFOO`; include order changes header resolution). A separate-form `-I <dir>` was also being swallowed by the attached-form parser branch and silently recorded as empty — now parsed correctly.
- **Linker keys are order- and content-sensitive.** `compute_linker_key()` keeps the command-line order of inputs, libraries and search directories (significant for static libraries / `--start-group`), folds the content hash of every input into the key, and includes the remaining link flags/args. Previously the inputs/libraries were sorted and only their paths were hashed, so editing an object or reordering libraries produced a stale `LINK HIT`.
- **Configuration precedence honors explicit CLI intent.** Boolean CLI overrides (`--compression`, `--read-only`, `--remote-read-only`) are applied only when the flag is actually passed; previously the override struct's defaults silently reset project/env values. The project config (`.skipbuild.conf`, legacy `.buildcache.conf`) is now discovered by walking up from the current directory, so builds invoked from a subdirectory find it.
- **`--remote-read-only` is a plain flag and only disables uploads.** It no longer consumes the next argument and no longer sets the local `read_only` flag (which, once read-only was enforced, would also have stopped local caching). A separate `remote_read_only` config key and `SKIPBUILD_REMOTE_READ_ONLY` env var were added.
- **Remote cache round-trips every artifact by name.** The remote `meta.json` now lists each uploaded output and sidecar (original filename + server file), and all of them are uploaded. A remote hit downloads the full set and installs it as a normal local entry, so multi-output links (`.dll` + import `.lib`), PCH creation (`.o` + `.pch`) and debug sidecars are restored under their real names instead of only the first output. Entries in the legacy single-primary layout are still readable.

### Fixed
- **`read_only` now blocks local writes.** `Cache::put()` returns without creating or modifying an entry, so a read-only cache only serves hits (remote uploads were already suppressed).
- **GCC dependency files with escaped spaces are parsed correctly.** `\ `, `\#`, `\\` and odd-trailing-backslash line continuations are handled, so headers under paths with spaces are hashed into the key (previously the path was split and silently ignored, allowing stale hits). MSVC's localized `/showIncludes` prefix is handled by forcing English (`VSLANG=1033`) for the dependency probe and restoring the caller's value afterwards.
- **Truncated zstd frames are rejected.** The streaming decoder treated "input exhausted before the frame ended" as success, so a truncated cache blob restored a partial object. It now requires the frame to complete (`ZSTD_decompressStream` returns 0) and reports corruption instead, which purges the entry; regression test added.
- **File writes are checked before they are trusted.** Compressed blobs, restored artifacts, `meta.json` and `stats.json` are now flushed and their stream state verified; a failed or short write removes the temporary file and reports failure instead of publishing a truncated entry (or overwriting good stats with a partial one).
- **Pruning honours `max_age` and its size target.** Age-based eviction was gated on the cache already being over its size limit and compared `file_time_type` ticks (1601 epoch on Windows) against `system_clock`, so expired entries were never removed. Age eviction now runs independently (a non-positive value disables it), timestamps are converted to Unix time, and the LRU pass no longer re-visits (and double-subtracts) entries already removed by the age pass.
- **Remote hits install a valid local entry.** Fetched artifacts are materialised in a private staging directory and installed under the entry lock with a `meta.json`, so subsequent lookups hit locally and no unaccounted `<hash>.obj` junk is left behind. Sidecars are restored under their real names (derived from the primary output stem) instead of hash-derived ones.
- **`g++`/`clang++` compile-only commands (`-c`) were misrouted to the linker cache**, whose key ignores the source and compile flags, so every recompile of a file replayed a stale cached object. Such commands now go through the compiler path.
- **Exit codes on bypass paths were inverted** (`return run_compiler(args) ? 0 : 1;`), and the optional `--` separator was passed to the compiler as a source file. Bypass paths now return the compiler's real exit code, propagate spawn failures (127), and drop `--`.
- **DLL link-hit restore clobbered the DLL with its import library** — every cached artifact was written to the linker output path. Each artifact is now restored under its own filename next to the output.
- **Concurrent cache reads could corrupt/delete each other's decompressed copies.** `get()` used to decompress every hit to a fixed path *inside* the shared entry directory while holding only a shared lock, so parallel builds could overwrite each other's copy, and `release()` deleted that shared file even if another reader was still copying it. Decompressed artifacts are now materialised in a private per-lookup staging directory (outside the entry directory) and `release()` only removes that directory. Sidecars now carry the path of the materialised file, and the CLI releases staged files even when a restore fails.
- **Purging a corrupt entry self-deadlocked.** The exclusive lock needed to delete the entry was requested while the same process still held its shared lock on the entry's `.lock`, so the acquire spun until timeout and the entry was never removed. The shared lock is now released first.
- **`--stats` reported the wrong cache size.** `update_cache_size()` stored the literal count `1` and the size of only the most recent entry on every `put()`, so the entry count was always 1 and the total size was meaningless. The real totals are now recomputed from the on-disk entries.
- **Cache hits no longer inflate the entry.** `Cache::get()` decompresses stored `.zst` blobs into a per-lookup staging directory, and those copies were left behind, roughly doubling an entry's on-disk size on every hit. The decompressed copies are now tracked (`CacheEntry::transient_files`) and deleted via the new `Cache::release()` once the CLI has copied the outputs out, so the compressed blob remains the only stored artifact. Size accounting also skips a decompressed copy that duplicates a `.zst` sibling.
- On Windows, cache-entry `.lock` files are opened with `FILE_SHARE_DELETE`, so purging an entry (which holds the lock open while calling `remove_all`) can actually delete it; previously `purge_corrupted_entry()` silently left the entry in place.
- **The test suite now actually asserts.** `assert()` is compiled out when `NDEBUG` is defined, which Release/RelWithDebInfo builds do — so the previous "14/14" CI result validated nothing. Test targets are now built with `NDEBUG` undefined (`/UNDEBUG` on MSVC, `-UNDEBUG` elsewhere), which surfaced and fixed six long-standing latent failures:
  - `get_compiler_hash()` on a non-existent compiler path returns a stable 64-hex SHA-256 instead of a variable-length decimal `std::hash` value, so the fingerprint shape is consistent.
  - The parser now accepts the attached MSVC PCH forms (`/Yc"pch.h"`, `/Yupch.h`, `/Ycpch.h`) and treats a header passed to `-x c++-header` as the input file; GCC/Clang PCH outputs follow the `<header>.gch` convention (appending `.gch`) and an explicit `-o` is honoured.
  - A cache miss is recorded once, not twice (`Cache::get()` and `Cache::safe_get()` were both counting it), so `--stats` miss counts are correct.
  - Zero-byte object files are no longer stored, so an empty/failed compile output cannot poison a cache key.
- `normalize_path_for_key()` now only canonicalizes absolute paths. `weakly_canonical()` resolves a relative path against the current working directory on some platforms (e.g. macOS libc++), which baked a machine-specific absolute path into the cache key; relative paths are kept relative so keys stay portable.
- Test fixtures were corrected where the *test* (not the code) was wrong: the response-file BOM case wrote too few bytes, the sidecar test assumed an uncompressed cache layout, and the eviction test used a hard-coded byte budget that didn't match the compressed entry sizes (and measured the cache size only after a `get()` had already decompressed blobs back into the entry directory).

## [0.3.0] - 2026-10-18

### Added
- `skipbuild --init [CMakeLists.txt] [--dry-run]` — writes the compiler-launcher snippet right after `project(...)`, so integration is one command instead of a manual edit. It is idempotent (skips files that already set a launcher), preserves CRLF line endings and a UTF-8 BOM, falls back to `cmake_minimum_required(...)` when there is no `project()`, and prints the snippet to add when it finds no safe insertion point. Covered by `test_init`.
- **Transitive header-dependency tracking.** skipbuild now discovers the full include set (`cl /showIncludes` or `cc -MM`) and mixes the content hash of every included header into the cache key, so editing any header invalidates the affected entries.
- `SKIPBUILD_REMOTE_ALLOW_INSECURE` to opt in to sending a remote auth header over an unencrypted transport (off by default).
- CMake options `SKIPBUILD_WERROR` (compile with `/W4 /WX` on MSVC, `-Wall -Wextra -Werror` elsewhere) and `SKIPBUILD_SANITIZE` (AddressSanitizer + UBSan on GCC/Clang), both off by default.

### Fixed
- **Cache-key soundness.** The key now covers everything that affects the object:
  - **Header edits invalidate the entry.** Previously only `-I` *directory paths* were hashed, so editing a header produced a stale hit.
  - **Defines and flags participate in the key.** `-D`/`/D`, `-U`/`/U`, `/std:`, `/MD`/`/MT`, `-f*`, `-m*`, `-pthread` and any other unrecognised flag are now hashed; previously only a small allow-list of flags was.
  - When the dependency set cannot be determined, the cache is bypassed for that file instead of risking a stale hit.
- **Eviction actually runs.** `Cache::prune()` (max-size + max-age LRU) is now invoked by the CLI after a successful store, throttled to once every 5 minutes via a `.last_cleanup` marker. Previously it was only reachable from tests, so caches grew unbounded despite the documented 5 GB limit.
- **Remote cache hardening.**
  - Credentials are no longer sent over an unencrypted transport: plaintext `http://` — and every non-Windows build, which has no TLS client — drop the auth header unless `SKIPBUILD_REMOTE_ALLOW_INSECURE=1` is set.
  - Remote-supplied artifact names are sanitised to a bare filename before writing, blocking path traversal.
  - Zstd blobs are decompressed with a streaming, output-bounded reader instead of trusting the frame header's declared size (blocks decompression bombs).
- Quoted compiler defines (e.g. `-DFOO="bar"`, MSVC `-DSKIPBUILD_VERSION="0.3.0"`) now reach the compiler intact — the compiler is replayed with an argv-preserving spawn instead of going through `std::system`/`cmd.exe`, which stripped inner quotes.
- Warnings-as-errors clean (`/W4 /WX`): removed unused locals/parameters, signed/unsigned and truncation warnings in `cache/core.cpp` and `cache/config.cpp`.

### Changed
- CI: added Ninja-based end-to-end launcher smoke tests on Linux/GCC (`launcher-smoke`) and Windows/MSVC (`launcher-smoke-windows`, covering the `cl /showIncludes` path), each asserting miss → hit → header-edit miss; plus a warnings-as-errors job (`/W4 /WX`) and a non-blocking ASan/UBSan job.

### Documentation
- Reworked the README "Integration" section to lead with the no-edit path (two `-D` flags), then `skipbuild --init`, then the manual snippet, with a compact per-build-system table and a single "VS generator is not supported" gotcha.
- Corrected the cache-key formula (header content, all flags), the eviction behaviour, and the remote-cache security notes.

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