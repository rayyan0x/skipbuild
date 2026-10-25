#pragma once

#include "config.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace skipbuild {

// Launcher-level options found in argv.
struct CliOverrides {
    std::optional<CacheConfig> config;
    CliOverrideMask mask;
    std::filesystem::path cache_dir;
    bool compiler_check_set = false;
    CompilerCheckMode compiler_check = CompilerCheckMode::Mtime;
    std::vector<bool> consumed;  // per argv index: handled by the launcher
};

// Scan argv for launcher options. Everything after "--" belongs to the wrapped
// compiler and is never interpreted or consumed here, even when it spells a
// launcher option such as "--cache-dir".
inline CliOverrides parse_cli_overrides(int argc, char* const argv[]) {
    CliOverrides result;
    result.consumed.assign(static_cast<std::size_t>(argc < 0 ? 0 : argc), false);

    auto overrides = [&result]() -> CacheConfig& {
        if (!result.config) result.config = CacheConfig{};
        return *result.config;
    };
    auto index = [](int i) { return static_cast<std::size_t>(i); };

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--") break;

        if (arg == "--cache-dir" && i + 1 < argc) {
            result.cache_dir = argv[++i];
            result.consumed[index(i)] = true;
            result.consumed[index(i - 1)] = true;
        } else if (arg == "--max-cache-size" && i + 1 < argc) {
            overrides().max_cache_size_bytes = ConfigParser::parse_size(argv[++i]);
            result.consumed[index(i)] = true;
            result.consumed[index(i - 1)] = true;
        } else if (arg == "--max-age" && i + 1 < argc) {
            try { overrides().max_age_days = std::stoi(argv[++i]); } catch (...) {}
            result.consumed[index(i)] = true;
            result.consumed[index(i - 1)] = true;
        } else if (arg == "--compression" && i + 1 < argc) {
            overrides().compression_enabled = ConfigParser::parse_bool(argv[++i]);
            result.mask.compression_enabled = true;
            result.consumed[index(i)] = true;
            result.consumed[index(i - 1)] = true;
        } else if (arg == "--read-only" && i + 1 < argc) {
            overrides().read_only = ConfigParser::parse_bool(argv[++i]);
            result.mask.read_only = true;
            result.consumed[index(i)] = true;
            result.consumed[index(i - 1)] = true;
        } else if (arg == "--remote-url" && i + 1 < argc) {
            overrides().remote_url = argv[++i];
            result.consumed[index(i)] = true;
            result.consumed[index(i - 1)] = true;
        } else if (arg == "--remote-read-only") {
            overrides().remote_read_only = true;
            result.mask.remote_read_only = true;
            result.consumed[index(i)] = true;
        } else if (arg == "--remote-timeout" && i + 1 < argc) {
            try { overrides().remote_connect_timeout_ms = std::stoi(argv[++i]); } catch (...) {}
            overrides().remote_read_timeout_ms = overrides().remote_connect_timeout_ms;
            result.consumed[index(i)] = true;
            result.consumed[index(i - 1)] = true;
        } else if (arg == "--disable-incremental-msbuild" && i + 1 < argc) {
            overrides().disable_incremental_msbuild = ConfigParser::parse_bool(argv[++i]);
            result.mask.disable_incremental_msbuild = true;
            result.consumed[index(i)] = true;
            result.consumed[index(i - 1)] = true;
        } else if (arg == "--compiler-check" && i + 1 < argc) {
            result.compiler_check = parse_compiler_check_mode(argv[++i]);
            result.compiler_check_set = true;
            result.consumed[index(i)] = true;
            result.consumed[index(i - 1)] = true;
        }
    }

    return result;
}

}  // namespace skipbuild
