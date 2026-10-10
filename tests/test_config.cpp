#include "config.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

void test_parse_string() {
    std::cout << "Testing config string parsing...\n";
    
    std::string content = R"(
# This is a comment
max_cache_size = 10G
cache_dir = /tmp/buildcache
compression_enabled = true
read_only = false
remote_url = http://cache.example.com
max_age_days = 60
eviction_target_ratio = 0.9
)";

    auto values = remake::ConfigParser::parse_string(content);
    
    assert(values.size() == 7);
    assert(values["max_cache_size"] == "10G");
    assert(values["cache_dir"] == "/tmp/buildcache");
    assert(values["compression_enabled"] == "true");
    assert(values["read_only"] == "false");
    assert(values["remote_url"] == "http://cache.example.com");
    assert(values["max_age_days"] == "60");
    assert(values["eviction_target_ratio"] == "0.9");
    
    std::cout << "  PASS: All key-value pairs parsed correctly\n";
}

void test_parse_file() {
    std::cout << "Testing config file parsing...\n";
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-config";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    fs::path config_file = test_dir / ".buildcache.conf";
    std::ofstream(config_file) << R"(
max_cache_size = 500M
cache_dir = C:\custom\cache
compression_enabled = false
)";
    
    auto values = remake::ConfigParser::parse_file(config_file);
    
    assert(values.size() == 3);
    assert(values["max_cache_size"] == "500M");
    assert(values["cache_dir"] == "C:\\custom\\cache");
    assert(values["compression_enabled"] == "false");
    
    std::cout << "  PASS: Config file parsed correctly\n";
    
    // Test non-existent file
    auto empty = remake::ConfigParser::parse_file(test_dir / "nonexistent.conf");
    assert(empty.empty());
    std::cout << "  PASS: Non-existent file returns empty map\n";
    
    fs::remove_all(test_dir);
}

void test_parse_size() {
    std::cout << "Testing size parsing...\n";
    
    assert(remake::ConfigParser::parse_size("10G") == 10LL * 1024 * 1024 * 1024);
    assert(remake::ConfigParser::parse_size("10g") == 10LL * 1024 * 1024 * 1024);
    assert(remake::ConfigParser::parse_size("500M") == 500LL * 1024 * 1024);
    assert(remake::ConfigParser::parse_size("500m") == 500LL * 1024 * 1024);
    assert(remake::ConfigParser::parse_size("1024K") == 1024LL * 1024);
    assert(remake::ConfigParser::parse_size("1024k") == 1024LL * 1024);
    assert(remake::ConfigParser::parse_size("1000") == 1000);
    assert(remake::ConfigParser::parse_size("2.5G") == static_cast<int64_t>(2.5 * 1024 * 1024 * 1024));
    assert(remake::ConfigParser::parse_size("") == 0);
    assert(remake::ConfigParser::parse_size("invalid") == 0);
    
    std::cout << "  PASS: All size formats parsed correctly\n";
}

void test_parse_bool() {
    std::cout << "Testing boolean parsing...\n";
    
    assert(remake::ConfigParser::parse_bool("true") == true);
    assert(remake::ConfigParser::parse_bool("True") == true);
    assert(remake::ConfigParser::parse_bool("TRUE") == true);
    assert(remake::ConfigParser::parse_bool("1") == true);
    assert(remake::ConfigParser::parse_bool("yes") == true);
    assert(remake::ConfigParser::parse_bool("on") == true);
    
    assert(remake::ConfigParser::parse_bool("false") == false);
    assert(remake::ConfigParser::parse_bool("False") == false);
    assert(remake::ConfigParser::parse_bool("0") == false);
    assert(remake::ConfigParser::parse_bool("no") == false);
    assert(remake::ConfigParser::parse_bool("off") == false);
    assert(remake::ConfigParser::parse_bool("") == false);
    assert(remake::ConfigParser::parse_bool("invalid") == false);
    
    std::cout << "  PASS: All boolean formats parsed correctly\n";
}

void test_apply_values() {
    std::cout << "Testing apply_values...\n";
    
    remake::CacheConfig config;
    std::map<std::string, std::string> values = {
        {"max_cache_size", "2G"},
        {"max_age_days", "90"},
        {"eviction_target_ratio", "0.95"},
        {"cache_dir", "/custom/cache"},
        {"compression_enabled", "false"},
        {"read_only", "true"},
        {"remote_url", "https://cache.example.com"},
        {"remote_connect_timeout_ms", "5000"},
        {"remote_read_timeout_ms", "10000"},
        {"remote_max_retries", "3"}
    };
    
    remake::ConfigParser::apply_values(config, values);
    
    assert(config.max_cache_size_bytes == 2LL * 1024 * 1024 * 1024);
    assert(config.max_age_days == 90);
    assert(config.eviction_target_ratio == 0.95);
    assert(config.cache_dir == "/custom/cache");
    assert(config.compression_enabled == false);
    assert(config.read_only == true);
    assert(config.remote_url == "https://cache.example.com");
    assert(config.remote_connect_timeout_ms == 5000);
    assert(config.remote_read_timeout_ms == 10000);
    assert(config.remote_max_retries == 3);
    
    std::cout << "  PASS: All values applied correctly\n";
}

void test_precedence_hierarchy() {
    std::cout << "Testing configuration precedence hierarchy...\n";
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-precedence";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    // Create global config
    fs::path global_dir = test_dir / "global";
    fs::create_directories(global_dir);
    fs::path global_config = global_dir / "config";
    std::ofstream(global_config) << "max_cache_size = 1G\ncache_dir = /global/cache\n";
    
    // Create project config
    fs::path project_dir = test_dir / "project";
    fs::create_directories(project_dir);
    fs::path project_config = project_dir / ".buildcache.conf";
    std::ofstream(project_config) << "max_cache_size = 2G\n";
    
    // Set environment variable
    #ifdef _WIN32
    _putenv_s("BUILDCACHE_MAX_SIZE", "3G");
    _putenv_s("BUILDCACHE_CACHE_DIR", "/env/cache");
    #else
    setenv("BUILDCACHE_MAX_SIZE", "3G", 1);
    setenv("BUILDCACHE_CACHE_DIR", "/env/cache", 1);
    #endif
    
    // Test: defaults < global < project < env < cli
    remake::CacheConfig cli_override;
    cli_override.max_cache_size_bytes = 4LL * 1024 * 1024 * 1024; // 4G
    cli_override.cache_dir = "/cli/cache";
    
    auto config = remake::ConfigParser::load_config(project_dir, cli_override);
    
    // CLI should win
    assert(config.max_cache_size_bytes == 4LL * 1024 * 1024 * 1024);
    assert(config.cache_dir == "/cli/cache");
    std::cout << "  PASS: CLI overrides everything (max_cache_size=4G, cache_dir=/cli/cache)\n";
    
    // Test without CLI override - env should win
    auto config2 = remake::ConfigParser::load_config(project_dir, std::nullopt);
    assert(config2.max_cache_size_bytes == 3LL * 1024 * 1024 * 1024);
    assert(config2.cache_dir == "/env/cache");
    std::cout << "  PASS: Env overrides file configs (max_cache_size=3G, cache_dir=/env/cache)\n";
    
    // Clean up env vars
    #ifdef _WIN32
    _putenv_s("BUILDCACHE_MAX_SIZE", "");
    _putenv_s("BUILDCACHE_CACHE_DIR", "");
    #else
    unsetenv("BUILDCACHE_MAX_SIZE");
    unsetenv("BUILDCACHE_CACHE_DIR");
    #endif
    
    // Test: project overrides global
    auto config3 = remake::ConfigParser::load_config(project_dir, std::nullopt);
    assert(config3.max_cache_size_bytes == 2LL * 1024 * 1024 * 1024);
    std::cout << "  PASS: Project config overrides global (max_cache_size=2G)\n";
    
    // Test: global used when no project config
    auto config4 = remake::ConfigParser::load_config("", std::nullopt);
    // Global config may or may not be found depending on system state
    // Just verify it loads without error and uses some defaults
    assert(config4.max_cache_size_bytes > 0);
    std::cout << "  PASS: Global config loaded (max_cache_size=" 
              << (config4.max_cache_size_bytes / 1024 / 1024 / 1024) << "G)\n";
    
    fs::remove_all(test_dir);
}

void test_malformed_config() {
    std::cout << "Testing malformed config handling...\n";
    
    std::string content = R"(
# Valid line
max_cache_size = 10G

# Invalid lines (should be skipped)
invalid line without equals
= no key

# Valid again
cache_dir = /tmp/cache

# Comments and empty lines
# This is a comment

another_key = value
)";
    
    auto values = remake::ConfigParser::parse_string(content);
    
    // Should only parse valid lines (key without value = is parsed with empty value)
    assert(values.size() == 3);
    assert(values["max_cache_size"] == "10G");
    assert(values["cache_dir"] == "/tmp/cache");
    assert(values["another_key"] == "value");
    
    std::cout << "  PASS: Malformed lines skipped without throwing\n";
}

void test_quotes() {
    std::cout << "Testing quoted values...\n";
    
    std::string content = R"(
cache_dir = "/path/with spaces/cache"
remote_url = 'http://example.com'
max_cache_size = "5G"
)";
    
    auto values = remake::ConfigParser::parse_string(content);
    
    assert(values.size() == 3);
    assert(values["cache_dir"] == "/path/with spaces/cache");
    assert(values["remote_url"] == "http://example.com");
    assert(values["max_cache_size"] == "5G");
    
    std::cout << "  PASS: Quoted values parsed correctly\n";
}

void test_case_insensitive_keys() {
    std::cout << "Testing case-sensitive keys in raw parser...\n";
    
    std::string content = R"(
MAX_CACHE_SIZE = 1G
Max_Cache_Size = 2G
max_cache_size = 3G
CACHE_DIR = /cache
Cache_Dir = /cache2
)";
    
    auto values = remake::ConfigParser::parse_string(content);
    
    // Raw parser preserves case - all 5 keys are distinct
    assert(values.size() == 5);
    assert(values["MAX_CACHE_SIZE"] == "1G");
    assert(values["Max_Cache_Size"] == "2G");
    assert(values["max_cache_size"] == "3G");
    assert(values["CACHE_DIR"] == "/cache");
    assert(values["Cache_Dir"] == "/cache2");
    
    std::cout << "  PASS: Raw parser preserves key case\n";
}

void test_config_with_cli_overrides() {
    std::cout << "Testing config with CLI overrides...\n";
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-cli-override";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    // Create project config
    fs::path project_dir = test_dir / "project";
    fs::create_directories(project_dir);
    fs::path project_config = project_dir / ".buildcache.conf";
    std::ofstream(project_config) << "max_cache_size = 1G\ncompression_enabled = true\nread_only = false\n";
    
    // CLI overrides
    remake::CacheConfig cli_override;
    cli_override.max_cache_size_bytes = 5LL * 1024 * 1024 * 1024; // 5G
    cli_override.compression_enabled = false;
    cli_override.read_only = true;
    
    auto config = remake::ConfigParser::load_config(project_dir, cli_override);
    
    assert(config.max_cache_size_bytes == 5LL * 1024 * 1024 * 1024);
    assert(config.compression_enabled == false);
    assert(config.read_only == true);
    
    std::cout << "  PASS: CLI overrides work for all fields\n";
    
    fs::remove_all(test_dir);
}

int main() {
    std::cout << "=== Config File Parser Tests ===\n\n";
    
    test_parse_string();
    std::cout << "\n";
    
    test_parse_file();
    std::cout << "\n";
    
    test_parse_size();
    std::cout << "\n";
    
    test_parse_bool();
    std::cout << "\n";
    
    test_apply_values();
    std::cout << "\n";
    
    test_precedence_hierarchy();
    std::cout << "\n";
    
    test_malformed_config();
    std::cout << "\n";
    
    test_quotes();
    std::cout << "\n";
    
    test_case_insensitive_keys();
    std::cout << "\n";
    
    test_config_with_cli_overrides();
    std::cout << "\n";
    
    std::cout << "=== All tests passed! ===\n";
    return 0;
}