#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <optional>
#include <mutex>
#include <map>
#include <memory>
#include <iostream>
#include "config.hpp"
#include "stats.hpp"

// Defined by the build system from the CMake project version.
// Fallback keeps non-CMake builds compiling without hardcoding a version.
#ifndef SKIPBUILD_VERSION
#define SKIPBUILD_VERSION "unknown"
#endif
#ifndef SKIPBUILD_WVERSION
#define SKIPBUILD_WVERSION L"unknown"
#endif

namespace skipbuild {

// Cross-platform file locking for inter-process synchronization
class FileLock {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    enum class LockType { Exclusive, Shared };
    
    explicit FileLock(const std::filesystem::path& lock_path);
    ~FileLock();
    
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    FileLock(FileLock&&) noexcept;
    FileLock& operator=(FileLock&&) noexcept;
    
    // Acquire lock (blocks until acquired or timeout)
    bool lock(LockType type = LockType::Exclusive, int timeout_ms = 5000);
    
    // Try to acquire lock (non-blocking)
    bool try_lock(LockType type = LockType::Exclusive);
    
    // Release lock
    void unlock();
    
    // Check if lock is held
    bool is_locked() const;
    
    // RAII scoped lock
    class ScopedLock {
        FileLock& lock_;
        bool acquired_;
    public:
        ScopedLock(FileLock& lock, LockType type = LockType::Exclusive, int timeout_ms = 10000) 
            : lock_(lock), acquired_(lock.lock(type, timeout_ms)) {}
        ~ScopedLock() { if (acquired_) lock_.unlock(); }
        ScopedLock(const ScopedLock&) = delete;
        ScopedLock& operator=(const ScopedLock&) = delete;
        explicit operator bool() const { return acquired_; }
    };
};

// Windows path utilities
#ifdef _WIN32
std::string to_long_path(const std::string& path);
std::string from_long_path(const std::string& path);
std::filesystem::path normalize_path(const std::filesystem::path& path);
#else
inline std::string to_long_path(const std::string& path) { return path; }
inline std::string from_long_path(const std::string& path) { return path; }
inline std::filesystem::path normalize_path(const std::filesystem::path& path) { return path; }
#endif

enum class CompilerType {
    GCC,
    Clang,
    MSVC,
    Unknown
};

struct CacheKey {
    std::string hash;
    std::string toolchain_id;
};

enum class SidecarType {
    None,
    PDB,           // MSVC .pdb
    IDB,           // MSVC .idb
    DWO,           // GCC/Clang Split DWARF .dwo
    Other          // Other sidecar files
};

struct SidecarInfo {
    SidecarType type = SidecarType::None;
    std::filesystem::path relative_path;  // Relative path from cache entry directory
    std::string hash;                      // SHA256 hash for validation
    std::string original_path;             // Original absolute path where it should be restored
};

struct CacheEntry {
    CacheKey key;
    std::vector<std::filesystem::path> outputs;  // Primary outputs (.obj, .o)
    std::vector<SidecarInfo> sidecars;           // Debug/sidecar files (.pdb, .dwo, etc.)
    // Decompressed copies materialised inside the entry directory by get().
    // They are regenerable from the stored .zst blobs, so the caller may delete
    // them once the outputs have been copied out (see Cache::release).
    std::vector<std::filesystem::path> transient_files;
    int64_t timestamp;
    int64_t size_bytes;
};

#include "remote.hpp"

enum class PCHMode {
    None,           // Regular compilation
    Create,         // Creating PCH: /Yc, -x c++-header
    Use             // Using PCH: /Yu, -include
};

struct PCHInfo {
    PCHMode mode = PCHMode::None;
    std::filesystem::path pch_source;    // Source file for PCH (e.g., stdafx.h)
    std::filesystem::path pch_output;    // Output PCH file (.pch, .gch)
    std::string pch_hash;                // Hash of PCH content for validation
};

struct CompileCommand {
    std::filesystem::path source;
    std::filesystem::path output;
    std::vector<std::string> args;
    std::vector<std::string> includes;
    std::vector<std::string> flags;
    std::vector<std::string> dependencies;  // resolved header files (from dep discovery)
    std::string compiler_path;
    std::string compiler_hash;
    PCHInfo pch;
    
    // Debug/sidecar file detection
    bool has_debug_info = false;
    std::vector<std::filesystem::path> expected_sidecars;  // Expected sidecar output paths
    std::vector<SidecarType> expected_sidecar_types;       // Types of expected sidecars
};

enum class LinkerType {
    GCC,        // g++, clang++
    MSVC,       // link.exe
    LLVM_LLD,   // lld-link, ld.lld
    Unknown
};

struct LinkerCommand {
    std::vector<std::string> args;
    std::vector<std::filesystem::path> inputs;      // .o, .obj, .a, .lib files
    std::vector<std::string> libraries;             // -lfoo, foo.lib
    std::vector<std::string> library_dirs;          // -L, /LIBPATH:
    std::filesystem::path output;                   // .exe, .dll, .so
    std::string linker_path;
    std::string linker_hash;
    LinkerType type = LinkerType::Unknown;
    std::vector<std::string> flags;                 // Other linker flags
};

class Cache {
    std::filesystem::path root_;
    mutable std::recursive_mutex mutex_;
    CacheConfig config_;
    mutable std::unique_ptr<StatsTracker> stats_tracker_;
    mutable std::unique_ptr<RemoteCache> remote_cache_;
    
    struct EntryMeta {
        std::filesystem::path path;
        int64_t timestamp;
        int64_t size_bytes;
        int64_t last_access;
    };
    
    void update_access_time(const std::string& hash) const;
    std::vector<EntryMeta> collect_entries() const;
    void purge_corrupted_entry(const EntryMeta& entry) const;
public:
    explicit Cache(std::filesystem::path root, CacheConfig config = CacheConfig()) 
        : root_(std::move(root)), config_(std::move(config)) {
        stats_tracker_ = std::make_unique<StatsTracker>(root_);
        
        // Initialize remote cache if configured
        if (!config_.remote_url.empty()) {
            RemoteCache::Config remote_config = RemoteCache::from_env();
            remote_config.url = config_.remote_url;
            remote_config.read_only = config_.read_only;
            remote_config.connect_timeout_ms = config_.remote_connect_timeout_ms;
            remote_config.read_timeout_ms = config_.remote_read_timeout_ms;
            remote_config.max_retries = config_.remote_max_retries;
            remote_cache_ = std::make_unique<RemoteCache>(std::move(remote_config), root_);
        }
    }
    
    // Stats access
    CacheStats get_detailed_stats() const {
        return stats_tracker_->get_stats();
    }
    
    void record_direct_execute() const {
        stats_tracker_->record_direct_execute();
    }
    
    void clear_stats() const {
        stats_tracker_->clear();
    }
    
    void print_stats() const {
        stats_tracker_->print_summary();
    }
    
    static CacheKey compute_key(const std::filesystem::path& source,
                                 const std::vector<std::string>& includes,
                                 const std::vector<std::string>& flags,
                                 const std::string& compiler_hash,
                                 CompilerType compiler_type = CompilerType::Unknown);
    
    static CacheKey compute_key_from_command(const CompileCommand& cmd);
    
    bool put(const CacheKey& key, const std::vector<std::filesystem::path>& outputs,
             const std::vector<std::pair<std::filesystem::path, SidecarType>>& sidecars = {});
    
std::optional<CacheEntry> get(const CacheKey& key) const;
    
    // Safe cache get with automatic fallback on any error
    // Returns nullopt on any error (corrupted entry, IO error, etc.) and logs warning
    std::optional<CacheEntry> safe_get(const CacheKey& key) const;
    
    // Safe cache put - never throws, logs warning on failure, returns false on error
    bool safe_put(const CacheKey& key, const std::vector<std::filesystem::path>& outputs,
                  const std::vector<std::pair<std::filesystem::path, SidecarType>>& sidecars = {});

    void prune(int64_t max_size_bytes = -1, int max_age_days = -1);

    // Delete the ephemeral decompressed copies referenced by an entry that was
    // returned by get(). The compressed .zst blobs remain, so the entry is
    // still served on the next read. Safe to call more than once.
    void release(const CacheEntry& entry) const;
    
    struct Stats {
        int64_t entries = 0;
        int64_t total_size = 0;
        int64_t hits = 0;
        int64_t misses = 0;
    };
    Stats get_stats() const;
    
    const CacheConfig& get_config() const { return config_; }
    void set_config(const CacheConfig& config) { config_ = config; }
    
    // Remote cache access
    bool has_remote_cache() const { return remote_cache_ != nullptr; }
    
    RemoteCache::RemoteStats get_remote_stats() const {
        if (remote_cache_) return remote_cache_->get_stats();
        return RemoteCache::RemoteStats{};
    }
    
    bool test_remote_connection() const {
        if (remote_cache_) return remote_cache_->test_connection();
        return false;
    }
};

struct CompilerInfo {
    CompilerType type = CompilerType::Unknown;
    std::string path;
    std::string version_string;
    std::string hash;
};

CompilerInfo detect_compiler(const std::string& compiler_path);
std::optional<CompileCommand> parse_compile_command(int argc, char* argv[]);
std::string get_compiler_hash(const std::string& compiler_path);
std::string get_compiler_hash_with_version(const std::string& compiler_path);

// Compiler fingerprinting strategy used by get_compiler_hash().
// Defaults from SKIPBUILD_COMPILER_CHECK (or legacy BUILDCACHE_COMPILER_CHECK);
// the CLI overrides it from the loaded config via set_compiler_check_mode().
CompilerCheckMode get_compiler_check_mode();
void set_compiler_check_mode(CompilerCheckMode mode);
std::vector<std::string> canonicalize_flags(const std::vector<std::string>& flags, CompilerType type);

// Path normalization for cache key determinism
std::string normalize_path_for_key(const std::filesystem::path& path, const std::filesystem::path& build_root = "");
std::filesystem::path find_build_root(const std::filesystem::path& source_file);

// Environment variable handling for cache keys
std::string get_relevant_env_hash();
std::vector<std::string> get_relevant_env_vars();

// PCH support
PCHInfo detect_pch_mode(const CompileCommand& cmd, CompilerType type);
std::string compute_pch_hash(const std::filesystem::path& pch_file);
CacheKey compute_pch_key(const CompileCommand& cmd);

// Linker support
LinkerType detect_linker_type(const std::string& linker_path);
std::optional<LinkerCommand> parse_linker_command(int argc, char* argv[]);
std::string get_linker_hash(const std::string& linker_path);
CacheKey compute_linker_key(const LinkerCommand& cmd);

// Compression support (requires ZSTD)
#if REMAKE_HAVE_ZSTD
namespace skipbuild {
enum class CompressionAlgorithm {
    ZSTD,
    LZ4
};

struct CompressionConfig {
    CompressionAlgorithm algorithm = CompressionAlgorithm::ZSTD;
    int level = 3;  // ZSTD level 3 (fast, good ratio)
};

bool compress_buffer(const std::vector<uint8_t>& input, std::vector<uint8_t>& output, const CompressionConfig& config = {});
bool decompress_buffer(const std::vector<uint8_t>& input, std::vector<uint8_t>& output, const CompressionConfig& config = {});
bool compress_file(const std::filesystem::path& input_path, const std::filesystem::path& output_path, const CompressionConfig& config = {});
bool decompress_file(const std::filesystem::path& input_path, const std::filesystem::path& output_path, const CompressionConfig& config = {});
}
#endif // REMAKE_HAVE_ZSTD

// Resiliency & Fail-Safe Fallbacks
struct FallbackResult {
    int exit_code = -1;
    std::string stdout_output;
    std::string stderr_output;
    bool fallback_used = false;
    std::string error_message;
};

// Execute native compiler directly with full stdout/stderr passthrough
FallbackResult execute_native_compiler(const std::vector<std::string>& args, const std::string& compiler_path = "");

// Safe cache get with automatic fallback on any error
template<typename CacheType>
std::optional<typename CacheType::EntryType> safe_cache_get(CacheType& cache, const typename CacheType::KeyType& key, 
                                                               const std::vector<std::string>& compiler_args,
                                                               const std::string& compiler_path = "") {
    try {
        auto entry = cache.get(key);
        if (entry) return entry;
    } catch (const std::exception& e) {
        std::cerr << "[build-cache warning] Cache read failed: " << e.what() << ". Falling back to native compilation.\n";
        // Try to purge corrupted entry
        try {
            // Cache doesn't have direct purge by key, but get() already handles corruption
        } catch (...) {}
    } catch (...) {
        std::cerr << "[build-cache warning] Cache read failed: unknown error. Falling back to native compilation.\n";
    }
    return std::nullopt;
}

// Safe cache put - never throws, logs warning on failure
template<typename CacheType>
bool safe_cache_put(CacheType& cache, const typename CacheType::KeyType& key,
                    const std::vector<std::filesystem::path>& outputs,
                    const std::vector<std::pair<std::filesystem::path, SidecarType>>& sidecars = {}) {
    try {
        return cache.put(key, outputs, sidecars);
    } catch (const std::exception& e) {
        std::cerr << "[build-cache warning] Cache write failed: " << e.what() << ". Build output preserved.\n";
        return false;
    } catch (...) {
        std::cerr << "[build-cache warning] Cache write failed: unknown error. Build output preserved.\n";
        return false;
    }
}

// Network timeout configuration
struct NetworkTimeoutConfig {
    int connect_timeout_ms = 3000;   // 3 seconds
    int read_timeout_ms = 3000;      // 3 seconds
    int max_retries = 1;             // Minimal retries for fail-fast
};

// Logging
enum class LogLevel {
    None = 0,
    Error = 1,
    Warn = 2,
    Info = 3,
    Debug = 4
};

LogLevel get_log_level();
void log_message(LogLevel level, const std::string& message);
void log_debug(const std::string& message);
void log_info(const std::string& message);
void log_warn(const std::string& message);
void log_error(const std::string& message);

} // namespace skipbuild
