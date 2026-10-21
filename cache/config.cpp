#include "config.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <cctype>

namespace fs = std::filesystem;
namespace skipbuild {

CompilerCheckMode parse_compiler_check_mode(const std::string& value) {
    std::string v = value;
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (v == "mtime" || v == "default" || v == "time") {
        return CompilerCheckMode::Mtime;
    }
    if (v == "version" || v == "ver") {
        return CompilerCheckMode::Version;
    }
    if (v == "content" || v == "hash" || v == "binary") {
        return CompilerCheckMode::Content;
    }
    if (v == "none" || v == "off" || v == "disabled") {
        return CompilerCheckMode::None;
    }
    return CompilerCheckMode::Mtime;  // Unknown values fall back to the default
}

const char* compiler_check_mode_to_string(CompilerCheckMode mode) {
    switch (mode) {
        case CompilerCheckMode::Mtime:   return "mtime";
        case CompilerCheckMode::Version: return "version";
        case CompilerCheckMode::Content: return "content";
        case CompilerCheckMode::None:    return "none";
    }
    return "mtime";
}

std::string ConfigParser::trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        start++;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        end--;
    }
    return s.substr(start, end - start);
}

std::map<std::string, std::string> ConfigParser::parse_string(const std::string& content) {
    std::map<std::string, std::string> result;
    std::istringstream iss(content);
    std::string line;
    
    while (std::getline(iss, line)) {
        line = trim(line);
        
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') {
            continue;
        }
        
        // Find first '='
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            continue; // Invalid line, skip
        }
        
        std::string key = trim(line.substr(0, eq_pos));
        std::string value = trim(line.substr(eq_pos + 1));
        
        // Remove surrounding quotes if present
        if (value.size() >= 2) {
            if ((value[0] == '"' && value[value.size() - 1] == '"') ||
                (value[0] == '\'' && value[value.size() - 1] == '\'')) {
                value = value.substr(1, value.size() - 2);
            }
        }
        
        if (!key.empty()) {
            result[key] = value;
        }
    }
    
    return result;
}

std::map<std::string, std::string> ConfigParser::parse_file(const fs::path& path) {
    std::map<std::string, std::string> result;
    
    std::ifstream file(path);
    if (!file) {
        return result; // Return empty map if file doesn't exist or can't be read
    }
    
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return parse_string(content);
}

int64_t ConfigParser::parse_size(const std::string& size_str) {
    std::string s = trim(size_str);
    if (s.empty()) return 0;
    
    // Check for unit suffix
    char unit = 0;
    if (!s.empty() && std::isalpha(static_cast<unsigned char>(s.back()))) {
        unit = static_cast<char>(std::toupper(static_cast<unsigned char>(s.back())));
        s = s.substr(0, s.size() - 1);
    }
    
    try {
        double value = std::stod(s);
        switch (unit) {
            case 'G': case 'g': return static_cast<int64_t>(value * 1024 * 1024 * 1024);
            case 'M': case 'm': return static_cast<int64_t>(value * 1024 * 1024);
            case 'K': case 'k': return static_cast<int64_t>(value * 1024);
            default: return static_cast<int64_t>(value);
        }
    } catch (...) {
        return 0;
    }
}

bool ConfigParser::parse_bool(const std::string& bool_str) {
    std::string s = trim(bool_str);
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s == "true" || s == "1" || s == "yes" || s == "on";
}

void ConfigParser::apply_values(CacheConfig& config, const std::map<std::string, std::string>& values) {
    for (const auto& [key, value] : values) {
        std::string lower_key = key;
        std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
        
        if (lower_key == "max_cache_size" || lower_key == "max_size") {
            config.max_cache_size_bytes = parse_size(value);
        } else if (lower_key == "max_age_days" || lower_key == "max_age") {
            try {
                config.max_age_days = std::stoi(value);
            } catch (...) {}
        } else if (lower_key == "eviction_target_ratio" || lower_key == "eviction_ratio") {
            try {
                config.eviction_target_ratio = std::stod(value);
            } catch (...) {}
        } else if (lower_key == "auto_clear_ratio" || lower_key == "auto_clear") {
            try {
                config.auto_clear_ratio = std::stod(value);
            } catch (...) {}
        } else if (lower_key == "cache_dir" || lower_key == "cache_directory") {
            config.cache_dir = value;
        } else if (lower_key == "compression_enabled" || lower_key == "compression") {
            config.compression_enabled = parse_bool(value);
        } else if (lower_key == "read_only" || lower_key == "readonly") {
            config.read_only = parse_bool(value);
        } else if (lower_key == "remote_url" || lower_key == "remote") {
            config.remote_url = value;
        } else if (lower_key == "remote_connect_timeout_ms" || lower_key == "connect_timeout") {
            try {
                config.remote_connect_timeout_ms = std::stoi(value);
            } catch (...) {}
        } else if (lower_key == "remote_read_timeout_ms" || lower_key == "read_timeout") {
            try {
                config.remote_read_timeout_ms = std::stoi(value);
            } catch (...) {}
        } else if (lower_key == "remote_max_retries" || lower_key == "max_retries") {
            try {
                config.remote_max_retries = std::stoi(value);
            } catch (...) {}
        } else if (lower_key == "disable_incremental_msbuild" || lower_key == "disable_incremental") {
            config.disable_incremental_msbuild = parse_bool(value);
        } else if (lower_key == "compiler_check" || lower_key == "compiler_hash") {
            config.compiler_check = parse_compiler_check_mode(value);
        }
        // Unknown keys are silently ignored
    }
}

fs::path ConfigParser::find_global_config() {
    // Check environment variable first
    if (const char* env = std::getenv("SKIPBUILD_CONFIG")) {
        fs::path p(env);
        if (fs::exists(p)) return p;
    }
    // Legacy support
    if (const char* env = std::getenv("BUILDCACHE_CONFIG")) {
        fs::path p(env);
        if (fs::exists(p)) return p;
    }
    
    // Platform-specific default locations (preferred: skipbuild)
    #ifdef _WIN32
    if (const char* home = std::getenv("USERPROFILE")) {
        fs::path p = fs::path(home) / ".skipbuild" / "config";
        if (fs::exists(p)) return p;
    }
    if (const char* appdata = std::getenv("APPDATA")) {
        fs::path p = fs::path(appdata) / "skipbuild" / "config";
        if (fs::exists(p)) return p;
    }
    // Legacy locations
    if (const char* home = std::getenv("USERPROFILE")) {
        fs::path p = fs::path(home) / ".buildcache" / "config";
        if (fs::exists(p)) return p;
    }
    if (const char* appdata = std::getenv("APPDATA")) {
        fs::path p = fs::path(appdata) / "buildcache" / "config";
        if (fs::exists(p)) return p;
    }
    #else
    if (const char* home = std::getenv("HOME")) {
        fs::path p = fs::path(home) / ".config" / "skipbuild" / "config";
        if (fs::exists(p)) return p;
    }
    if (const char* home = std::getenv("HOME")) {
        fs::path p = fs::path(home) / ".skipbuild" / "config";
        if (fs::exists(p)) return p;
    }
    // Legacy locations
    if (const char* home = std::getenv("HOME")) {
        fs::path p = fs::path(home) / ".config" / "buildcache" / "config";
        if (fs::exists(p)) return p;
    }
    if (const char* home = std::getenv("HOME")) {
        fs::path p = fs::path(home) / ".buildcache" / "config";
        if (fs::exists(p)) return p;
    }
    #endif
    
    return "";
}

fs::path ConfigParser::find_project_config(const fs::path& project_root) {
    if (project_root.empty()) return "";

    std::error_code ec;
    fs::path dir = fs::weakly_canonical(project_root, ec);
    if (ec) {
        ec.clear();
        dir = fs::absolute(project_root, ec);
        if (ec) dir = project_root;
    }

    // Walk the directory and its ancestors so a project config at the source
    // root is still found when the cache is invoked from a build subdirectory.
    while (!dir.empty()) {
        // Preferred: .skipbuild.conf / .skipbuild/config
        fs::path p = dir / ".skipbuild.conf";
        if (fs::exists(p)) return p;
        p = dir / ".skipbuild" / "config";
        if (fs::exists(p)) return p;

        // Legacy: .buildcache.conf / .buildcache/config
        p = dir / ".buildcache.conf";
        if (fs::exists(p)) return p;
        p = dir / ".buildcache" / "config";
        if (fs::exists(p)) return p;

        fs::path parent = dir.parent_path();
        if (parent == dir) break;  // reached the filesystem root
        dir = parent;
    }

    return "";
}

CacheConfig ConfigParser::load_config(
    const fs::path& project_root,
    const std::optional<CacheConfig>& cli_overrides,
    const CliOverrideMask& cli_mask) {
    
    CacheConfig config;
    
    // 1. Start with defaults (already set in struct)
    
    // 2. Load global config
    fs::path global_config = find_global_config();
    if (!global_config.empty()) {
        auto values = parse_file(global_config);
        apply_values(config, values);
    }
    
    // 3. Load project config
    fs::path project_config = find_project_config(project_root);
    if (!project_config.empty()) {
        auto values = parse_file(project_config);
        apply_values(config, values);
    }
    
    // 4. Apply environment variable overrides
    // Support both SKIPBUILD_* (preferred) and BUILDCACHE_* (legacy) prefixes
    auto get_env = [](const char* skipbuild_var, const char* buildcache_var) -> const char* {
        if (const char* v = std::getenv(skipbuild_var)) return v;
        return std::getenv(buildcache_var);
    };
    
    if (const char* env = get_env("SKIPBUILD_DIR", "BUILDCACHE_DIR")) {
        config.cache_dir = env;
    } else if (const char* cache_dir_env = get_env("SKIPBUILD_CACHE_DIR", "BUILDCACHE_CACHE_DIR")) {
        config.cache_dir = cache_dir_env;
    }
    
    if (const char* env = get_env("SKIPBUILD_MAX_SIZE", "BUILDCACHE_MAX_SIZE")) {
        config.max_cache_size_bytes = parse_size(env);
    }
    
    if (const char* env = get_env("SKIPBUILD_MAX_AGE", "BUILDCACHE_MAX_AGE")) {
        try { config.max_age_days = std::stoi(env); } catch (...) {}
    }
    
    if (const char* env = get_env("SKIPBUILD_EVICTION_RATIO", "BUILDCACHE_EVICTION_RATIO")) {
        try { config.eviction_target_ratio = std::stod(env); } catch (...) {}
    }
    
    if (const char* env = get_env("SKIPBUILD_AUTO_CLEAR_RATIO", "BUILDCACHE_AUTO_CLEAR_RATIO")) {
        try { config.auto_clear_ratio = std::stod(env); } catch (...) {}
    }
    
    if (const char* env = get_env("SKIPBUILD_COMPRESSION", "BUILDCACHE_COMPRESSION")) {
        config.compression_enabled = parse_bool(env);
    }
    
    if (const char* env = get_env("SKIPBUILD_READ_ONLY", "BUILDCACHE_READ_ONLY")) {
        config.read_only = parse_bool(env);
    }
    
    if (const char* env = get_env("SKIPBUILD_REMOTE_URL", "BUILDCACHE_REMOTE_URL")) {
        config.remote_url = env;
    }
    
    if (const char* env = get_env("SKIPBUILD_REMOTE_CONNECT_TIMEOUT", "BUILDCACHE_REMOTE_CONNECT_TIMEOUT")) {
        try { config.remote_connect_timeout_ms = std::stoi(env); } catch (...) {}
    }
    
    if (const char* env = get_env("SKIPBUILD_REMOTE_READ_TIMEOUT", "BUILDCACHE_REMOTE_READ_TIMEOUT")) {
        try { config.remote_read_timeout_ms = std::stoi(env); } catch (...) {}
    }
    
    if (const char* env = get_env("SKIPBUILD_REMOTE_MAX_RETRIES", "BUILDCACHE_REMOTE_MAX_RETRIES")) {
        try { config.remote_max_retries = std::stoi(env); } catch (...) {}
    }
    
    if (const char* env = get_env("SKIPBUILD_DISABLE_INCREMENTAL_MSBUILD", "BUILDCACHE_DISABLE_INCREMENTAL_MSBUILD")) {
        config.disable_incremental_msbuild = parse_bool(env);
    }
    
    if (const char* env = get_env("SKIPBUILD_COMPILER_CHECK", "BUILDCACHE_COMPILER_CHECK")) {
        config.compiler_check = parse_compiler_check_mode(env);
    }
    
    // 5. Apply CLI overrides (highest priority)
    if (cli_overrides) {
        if (cli_overrides->max_cache_size_bytes > 0) config.max_cache_size_bytes = cli_overrides->max_cache_size_bytes;
        if (cli_overrides->max_age_days > 0) config.max_age_days = cli_overrides->max_age_days;
        if (cli_overrides->eviction_target_ratio > 0) config.eviction_target_ratio = cli_overrides->eviction_target_ratio;
        if (!cli_overrides->cache_dir.empty()) config.cache_dir = cli_overrides->cache_dir;
        if (!cli_overrides->remote_url.empty()) config.remote_url = cli_overrides->remote_url;
        // Booleans are only applied when the CLI actually passed the flag;
        // otherwise the value resolved from global/project/env config is kept.
        if (cli_mask.compression_enabled) config.compression_enabled = cli_overrides->compression_enabled;
        if (cli_mask.read_only) config.read_only = cli_overrides->read_only;
        if (cli_overrides->remote_connect_timeout_ms > 0) config.remote_connect_timeout_ms = cli_overrides->remote_connect_timeout_ms;
        if (cli_overrides->remote_read_timeout_ms > 0) config.remote_read_timeout_ms = cli_overrides->remote_read_timeout_ms;
        if (cli_overrides->remote_max_retries >= 0) config.remote_max_retries = cli_overrides->remote_max_retries;
    }
    
    // 6. Resolve cache_dir if not set
    if (config.cache_dir.empty()) {
        #ifdef _WIN32
        if (const char* home = std::getenv("USERPROFILE")) {
            config.cache_dir = fs::path(home) / ".cache" / "skipbuild";
        }
        #else
        if (const char* home = std::getenv("HOME")) {
            config.cache_dir = fs::path(home) / ".cache" / "skipbuild";
        }
        #endif
        if (config.cache_dir.empty()) {
            config.cache_dir = fs::temp_directory_path() / "skipbuild";
        }
    }
    
    // Normalize cache_dir
    config.cache_dir = fs::weakly_canonical(config.cache_dir);
    
    return config;
}

} // namespace skipbuild
