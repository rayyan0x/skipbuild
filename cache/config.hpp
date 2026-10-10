#pragma once
#include <string>
#include <optional>
#include <filesystem>
#include <map>

namespace remake {

struct CacheConfig {
    int64_t max_cache_size_bytes = 5LL * 1024 * 1024 * 1024;  // 5 GB default
    int max_age_days = 30;
    double eviction_target_ratio = 0.85;
    std::filesystem::path cache_dir;
    bool compression_enabled = true;
    bool read_only = false;
    std::string remote_url;
    int remote_connect_timeout_ms = 3000;
    int remote_read_timeout_ms = 3000;
    int remote_max_retries = 1;
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
        const std::optional<CacheConfig>& cli_overrides = std::nullopt);
    
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