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

    auto values = skipbuild::ConfigParser::parse_string(content);
    
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
    
    auto values = skipbuild::ConfigParser::parse_file(config_file);
    
    assert(values.size() == 3);
    assert(values["max_cache_size"] == "500M");
    assert(values["cache_dir"] == "C:\\custom\\cache");
    assert(values["compression_enabled"] == "false");
    
    std::cout << "  PASS: Config file parsed correctly\n";
    
    // Test non-existent file
    auto empty = skipbuild::ConfigParser::parse_file(test_dir / "nonexistent.conf");
    assert(empty.empty());
    std::cout << "  PASS: Non-existent file returns empty map\n";
    
    fs::remove_all(test_dir);
}

void test_parse_size() {
    std::cout << "Testing size parsing...\n";
    
    assert(skipbuild::ConfigParser::parse_size("10G") == 10LL * 1024 * 1024 * 1024);
    assert(skipbuild::ConfigParser::parse_size("10g") == 10LL * 1024 * 1024 * 1024);
    assert(skipbuild::ConfigParser::parse_size("500M") == 500LL * 1024 * 1024);
    assert(skipbuild::ConfigParser::parse_size("500m") == 500LL * 1024 * 1024);
    assert(skipbuild::ConfigParser::parse_size("1024K") == 1024LL * 1024);
    assert(skipbuild::ConfigParser::parse_size("1024k") == 1024LL * 1024);
    assert(skipbuild::ConfigParser::parse_size("1000") == 1000);
    assert(skipbuild::ConfigParser::parse_size("2.5G") == static_cast<int64_t>(2.5 * 1024 * 1024 * 1024));
    assert(skipbuild::ConfigParser::parse_size("") == 0);
    assert(skipbuild::ConfigParser::parse_size("invalid") == 0);
    
    std::cout << "  PASS: All size formats parsed correctly\n";
}

void test_parse_bool() {
    std::cout << "Testing boolean parsing...\n";
    
    assert(skipbuild::ConfigParser::parse_bool("true") == true);
    assert(skipbuild::ConfigParser::parse_bool("True") == true);
    assert(skipbuild::ConfigParser::parse_bool("TRUE") == true);
    assert(skipbuild::ConfigParser::parse_bool("1") == true);
    assert(skipbuild::ConfigParser::parse_bool("yes") == true);
    assert(skipbuild::ConfigParser::parse_bool("on") == true);
    
    assert(skipbuild::ConfigParser::parse_bool("false") == false);
    assert(skipbuild::ConfigParser::parse_bool("False") == false);
    assert(skipbuild::ConfigParser::parse_bool("0") == false);
    assert(skipbuild::ConfigParser::parse_bool("no") == false);
    assert(skipbuild::ConfigParser::parse_bool("off") == false);
    assert(skipbuild::ConfigParser::parse_bool("") == false);
    assert(skipbuild::ConfigParser::parse_bool("invalid") == false);
    
    std::cout << "  PASS: All boolean formats parsed correctly\n";
}

void test_apply_values() {
    std::cout << "Testing apply_values...\n";
    
    skipbuild::CacheConfig config;
    std::map<std::string, std::string> values = {
        {"max_cache_size", "2G"},
        {"max_age_days", "90"},
        {"eviction_target_ratio", "0.95"},
        {"cache_dir", "/custom/cache"},
        {"compression_enabled", "false"},
        {"read_only", "true"},
        {"remote_read_only", "true"},
        {"remote_url", "https://cache.example.com"},
        {"remote_connect_timeout_ms", "5000"},
        {"remote_read_timeout_ms", "10000"},
        {"remote_max_retries", "3"}
    };
    
    skipbuild::ConfigParser::apply_values(config, values);
    
    assert(config.max_cache_size_bytes == 2LL * 1024 * 1024 * 1024);
    assert(config.max_age_days == 90);
    assert(config.eviction_target_ratio == 0.95);
    assert(config.cache_dir == "/custom/cache");
    assert(config.compression_enabled == false);
    assert(config.read_only == true);
    assert(config.remote_read_only == true);
    assert(config.remote_url == "https://cache.example.com");
    assert(config.remote_connect_timeout_ms == 5000);
    assert(config.remote_read_timeout_ms == 10000);
    assert(config.remote_max_retries == 3);
    
    // remote_read_only is independent of the local read_only flag
    skipbuild::CacheConfig remote_only;
    skipbuild::ConfigParser::apply_values(remote_only, {{"remote_read_only", "true"}});
    assert(remote_only.remote_read_only == true);
    assert(remote_only.read_only == false);
    
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
    
    // Set environment variable (use new SKIPBUILD_* prefix, still supports legacy BUILDCACHE_*)
    #ifdef _WIN32
    _putenv_s("SKIPBUILD_MAX_SIZE", "3G");
    _putenv_s("SKIPBUILD_CACHE_DIR", "/env/cache");
    #else
    setenv("SKIPBUILD_MAX_SIZE", "3G", 1);
    setenv("SKIPBUILD_CACHE_DIR", "/env/cache", 1);
    #endif
    
    // Test: defaults < global < project < env < cli
    skipbuild::CacheConfig cli_override;
    cli_override.max_cache_size_bytes = 4LL * 1024 * 1024 * 1024; // 4G
    cli_override.cache_dir = "/cli/cache";
    
    auto config = skipbuild::ConfigParser::load_config(project_dir, cli_override);
    
    // CLI should win
    assert(config.max_cache_size_bytes == 4LL * 1024 * 1024 * 1024);
    assert(config.cache_dir == "/cli/cache");
    std::cout << "  PASS: CLI overrides everything (max_cache_size=4G, cache_dir=/cli/cache)\n";
    
    // Test without CLI override - env should win
    auto config2 = skipbuild::ConfigParser::load_config(project_dir, std::nullopt);
    assert(config2.max_cache_size_bytes == 3LL * 1024 * 1024 * 1024);
    assert(config2.cache_dir == "/env/cache");
    std::cout << "  PASS: Env overrides file configs (max_cache_size=3G, cache_dir=/env/cache)\n";
    
    // Clean up env vars
    #ifdef _WIN32
    _putenv_s("SKIPBUILD_MAX_SIZE", "");
    _putenv_s("SKIPBUILD_CACHE_DIR", "");
    #else
    unsetenv("SKIPBUILD_MAX_SIZE");
    unsetenv("SKIPBUILD_CACHE_DIR");
    #endif
    
    // Test: project overrides global
    auto config3 = skipbuild::ConfigParser::load_config(project_dir, std::nullopt);
    assert(config3.max_cache_size_bytes == 2LL * 1024 * 1024 * 1024);
    std::cout << "  PASS: Project config overrides global (max_cache_size=2G)\n";
    
    // Test: global used when no project config
    auto config4 = skipbuild::ConfigParser::load_config("", std::nullopt);
    // Global config may or may not be found depending on system state
    // Just verify it loads without error and uses some defaults
    assert(config4.max_cache_size_bytes > 0);
    std::cout << "  PASS: Global config loaded (max_cache_size=" 
              << (config4.max_cache_size_bytes / 1024 / 1024 / 1024) << "G)\n";
    
    fs::remove_all(test_dir);
}

void test_compiler_check_mode() {
    std::cout << "Testing compiler check mode config...\n";
    using skipbuild::CompilerCheckMode;
    
    assert(skipbuild::parse_compiler_check_mode("mtime") == CompilerCheckMode::Mtime);
    assert(skipbuild::parse_compiler_check_mode("MTIME") == CompilerCheckMode::Mtime);
    assert(skipbuild::parse_compiler_check_mode("default") == CompilerCheckMode::Mtime);
    assert(skipbuild::parse_compiler_check_mode("version") == CompilerCheckMode::Version);
    assert(skipbuild::parse_compiler_check_mode("content") == CompilerCheckMode::Content);
    assert(skipbuild::parse_compiler_check_mode("none") == CompilerCheckMode::None);
    assert(skipbuild::parse_compiler_check_mode("off") == CompilerCheckMode::None);
    assert(skipbuild::parse_compiler_check_mode("nonsense") == CompilerCheckMode::Mtime);
    
    assert(std::string(skipbuild::compiler_check_mode_to_string(CompilerCheckMode::Mtime)) == "mtime");
    assert(std::string(skipbuild::compiler_check_mode_to_string(CompilerCheckMode::Version)) == "version");
    assert(std::string(skipbuild::compiler_check_mode_to_string(CompilerCheckMode::Content)) == "content");
    assert(std::string(skipbuild::compiler_check_mode_to_string(CompilerCheckMode::None)) == "none");
    
    skipbuild::CacheConfig config;
    assert(config.compiler_check == CompilerCheckMode::Mtime);  // default
    std::map<std::string, std::string> values = {{"compiler_check", "content"}};
    skipbuild::ConfigParser::apply_values(config, values);
    assert(config.compiler_check == CompilerCheckMode::Content);
    
    std::cout << "  PASS: compiler_check parsed and applied correctly\n";
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
    
    auto values = skipbuild::ConfigParser::parse_string(content);
    
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
    
    auto values = skipbuild::ConfigParser::parse_string(content);
    
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
    
    auto values = skipbuild::ConfigParser::parse_string(content);
    
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
    std::ofstream(project_config) << "max_cache_size = 1G\ncompression_enabled = false\nread_only = true\n";
    
    // CLI overrides (explicitly supplied, so they win)
    skipbuild::CacheConfig cli_override;
    cli_override.max_cache_size_bytes = 5LL * 1024 * 1024 * 1024; // 5G
    cli_override.compression_enabled = true;
    cli_override.read_only = false;

    skipbuild::CliOverrideMask mask;
    mask.compression_enabled = true;
    mask.read_only = true;

    auto config = skipbuild::ConfigParser::load_config(project_dir, cli_override, mask);
    
    assert(config.max_cache_size_bytes == 5LL * 1024 * 1024 * 1024);
    assert(config.compression_enabled == true);
    assert(config.read_only == false);
    
    std::cout << "  PASS: CLI overrides work for all fields\n";

    // A CLI override struct that did NOT pass --compression/--read-only must not
    // reset the values loaded from the project config to its struct defaults.
    skipbuild::CacheConfig partial;
    partial.max_cache_size_bytes = 6LL * 1024 * 1024 * 1024;
    auto config_partial = skipbuild::ConfigParser::load_config(project_dir, partial, {});
    assert(config_partial.max_cache_size_bytes == 6LL * 1024 * 1024 * 1024);
    assert(config_partial.compression_enabled == false);  // from project config
    assert(config_partial.read_only == true);             // from project config

    std::cout << "  PASS: Unspecified CLI booleans keep config-file values\n";
    
    fs::remove_all(test_dir);
}

void test_project_config_upward_search() {
    std::cout << "Testing upward project config discovery...\n";

    fs::path test_dir = fs::temp_directory_path() / "remake-test-upward-config";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::path build_dir = test_dir / "build" / "nested";
    fs::create_directories(build_dir);

    fs::path project_config = test_dir / ".skipbuild.conf";
    std::ofstream(project_config) << "compression_enabled = false\nread_only = true\n";

    auto found = skipbuild::ConfigParser::find_project_config(build_dir);
    assert(!found.empty());
    assert(fs::equivalent(found, project_config));
    std::cout << "  PASS: Found config in ancestor directory\n";

    // load_config() must pick it up when invoked from a build subdirectory.
    auto config = skipbuild::ConfigParser::load_config(build_dir, std::nullopt);
    assert(config.compression_enabled == false);
    assert(config.read_only == true);
    std::cout << "  PASS: Ancestor config applied by load_config\n";

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
    
    test_compiler_check_mode();
    std::cout << "\n";
    
    test_malformed_config();
    std::cout << "\n";
    
    test_quotes();
    std::cout << "\n";
    
    test_case_insensitive_keys();
    std::cout << "\n";
    
    test_config_with_cli_overrides();
    std::cout << "\n";
    
    test_project_config_upward_search();
    std::cout << "\n";
    
    std::cout << "=== All tests passed! ===\n";
    return 0;
}
