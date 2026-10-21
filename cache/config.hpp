#pragma once
#include <string>
#include <optional>
#include <filesystem>
#include <map>

namespace skipbuild {

// How the compiler binary is fingerprinted when building cache keys.
// A compiler change invalidates cache entries that used the old compiler.
enum class CompilerCheckMode {
    Mtime,    // size + mtime + version output (default: fast, maximally safe)
    Version,  // version output only (stable across reinstalls/rebuilds of the same version)
    Content,  // SHA-256 of the compiler binary content (accurate, slower)
    None      // exclude the compiler from the cache key (fastest, least safe)
};

CompilerCheckMode parse_compiler_check_mode(const std::string& value);
const char* compiler_check_mode_to_string(CompilerCheckMode mode);

struct CacheConfig {
    int64_t max_cache_size_bytes = 5LL * 1024 * 1024 * 1024;  // 5 GB default
    int max_age_days = 30;
    double eviction_target_ratio = 0.85;
    // When cache usage reaches this fraction of max_cache_size_bytes the entire
    // cache is purged ("clear to zero"). <= 0 disables the behaviour.
    double auto_clear_ratio = 0.95;
    std::filesystem::path cache_dir;
    bool compression_enabled = true;
    bool read_only = false;         // Local cache: read, but never write
    bool remote_read_only = false;  // Remote cache: download, but never upload
    std::string remote_url;
    int remote_connect_timeout_ms = 3000;
    int remote_read_timeout_ms = 3000;
    int remote_max_retries = 1;
    bool disable_incremental_msbuild = true;  // Auto-disable for MSBuild incremental (output newer than source)
    CompilerCheckMode compiler_check = CompilerCheckMode::Mtime;  // Compiler fingerprinting strategy
};

// Which optional CLI overrides the caller actually supplied. A default-constructed
// CacheConfig cannot distinguish an explicit `false` from "not specified", so the
// CLI reports the flags it saw here; unspecified booleans fall through to the
// global/project/env values rather than resetting them to the struct defaults.
struct CliOverrideMask {
    bool compression_enabled = false;
    bool read_only = false;
    bool remote_read_only = false;
};

class ConfigParser {
public:
    // Parse a config file and return a map of key-value pairs
    static std::map<std::string, std::string> parse_file(const std::filesystem::path& path);
    
    // Parse a config string (for testing)
    static std::map<std::string, std::string> parse_string(const std::string& content);
    
    // Load config with full precedence hierarchy
    // Order: defaults < global < project < env < cli
    static CacheConfig load_config(
        const std::filesystem::path& project_root = "",
        const std::optional<CacheConfig>& cli_overrides = std::nullopt,
        const CliOverrideMask& cli_mask = {});
    
    // Find config files in standard locations
    static std::filesystem::path find_global_config();
    static std::filesystem::path find_project_config(const std::filesystem::path& project_root);
    
    // Apply parsed values to config struct
    static void apply_values(CacheConfig& config, const std::map<std::string, std::string>& values);
    
    // Parse size string (e.g., "10G", "500M", "1024K", "1000")
    static int64_t parse_size(const std::string& size_str);
    
    // Parse boolean string
    static bool parse_bool(const std::string& bool_str);

private:
    static std::string trim(const std::string& s);
};

} // namespace remake
