#include "args.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

namespace {

skipbuild::CliOverrides parse(std::vector<std::string>& storage) {
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& arg : storage) argv.push_back(arg.data());
    return skipbuild::parse_cli_overrides(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

int main() {
    // Everything after "--" belongs to the compiler: a compiler argument that
    // happens to spell a launcher option must be neither consumed nor applied.
    {
        std::vector<std::string> args{
            "skipbuild", "--", "g++", "-c", "a.cpp", "--cache-dir", "/tmp/x"};
        auto r = parse(args);
        assert(!r.config.has_value());
        assert(r.cache_dir.empty());
        for (std::size_t i = 2; i < args.size(); ++i) {
            assert(!r.consumed[i] && "argument after -- was consumed");
        }
    }

    // Options before the separator are consumed and removed from the command.
    {
        std::vector<std::string> args{
            "skipbuild", "--cache-dir", "/tmp/x", "--max-age", "7", "--", "g++", "-c"};
        auto r = parse(args);
        assert(r.cache_dir == std::filesystem::path("/tmp/x"));
        assert(r.config.has_value());
        assert(r.config->max_age_days == 7);
        for (std::size_t i = 1; i <= 4; ++i) assert(r.consumed[i]);
        assert(!r.consumed[5] && "the separator itself must stay in the command");
        assert(!r.consumed[6]);
    }

    // Wrapper form keeps working: options after the compiler are launcher
    // options (there is no separator).
    {
        std::vector<std::string> args{"skipbuild", "g++", "--cache-dir", "/tmp/y", "-c"};
        auto r = parse(args);
        assert(r.cache_dir == std::filesystem::path("/tmp/y"));
        assert(r.consumed[2] && r.consumed[3]);
        assert(!r.consumed[1] && !r.consumed[4]);
    }

    // Boolean overrides record their mask and explicit value.
    {
        std::vector<std::string> args{
            "skipbuild", "--read-only", "true", "--compression", "false",
            "--remote-read-only", "--disable-incremental-msbuild", "false", "--", "g++"};
        auto r = parse(args);
        assert(r.config.has_value());
        assert(r.mask.read_only && r.config->read_only);
        assert(r.mask.compression_enabled && !r.config->compression_enabled);
        assert(r.mask.remote_read_only && r.config->remote_read_only);
        assert(r.mask.disable_incremental_msbuild && !r.config->disable_incremental_msbuild);
    }

    // A trailing separator with no compiler still parses.
    {
        std::vector<std::string> args{"skipbuild", "--"};
        auto r = parse(args);
        assert(!r.config.has_value());
        assert(!r.consumed[1]);
    }

    std::cout << "All CLI argument tests passed!\n";
    return 0;
}
