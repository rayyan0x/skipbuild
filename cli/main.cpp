#include "core.hpp"
#include "config.hpp"
#include "init.hpp"
#include <iostream>
#include <fstream>
#include <iterator>
#include <vector>
#include <string>
#include <filesystem>
#include <chrono>
#include <cstdlib>
#include <array>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <optional>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

static int spawn_argv(const std::vector<std::string>& args);

// Execute native compiler with full passthrough (fallback)
int execute_native_fallback(const std::vector<std::string>& args, const std::string& compiler_path = "") {
    std::vector<std::string> full_args = args;
    if (!compiler_path.empty() && !full_args.empty()) {
        full_args[0] = compiler_path;
    }
    return spawn_argv(full_args);
}

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options] -- <compiler> [compiler_args...]\n";
    std::cerr << "\n";
    std::cerr << "skipbuild - C++ build cache (compiler launcher)\n";
    std::cerr << "\n";
    std::cerr << "Modes:\n";
    std::cerr << "  skipbuild g++ -c foo.cpp -o foo.o     # Wrap compiler (like ccache)\n";
    std::cerr << "  skipbuild --cache-dir <path>          # Set cache directory\n";
    std::cerr << "  skipbuild --stats                     # Show cache statistics\n";
    std::cerr << "  skipbuild --remote-stats              # Show remote cache statistics\n";
    std::cerr << "  skipbuild --clear-stats               # Reset cache statistics to zero\n";
    std::cerr << "  skipbuild --clean                     # Clean cache\n";
    std::cerr << "  skipbuild --init [CMakeLists.txt]     # Add caching to a CMake project (idempotent)\n";
    std::cerr << "  skipbuild --version                   # Show version\n";
    std::cerr << "\n";
    std::cerr << "Options:\n";
    std::cerr << "  --remote-url <url>              Remote cache endpoint (e.g., http://cache:8080)\n";
    std::cerr << "  --remote-read-only              Remote cache read-only mode\n";
    std::cerr << "  --remote-timeout <ms>           Remote connect/read timeout in ms\n";
    std::cerr << "  --disable-incremental-msbuild   Auto-disable for MSBuild incremental (true/false, default: true)\n";
    std::cerr << "  --compiler-check <mode>         Compiler fingerprint: mtime, version, content, none (default: mtime)\n";
    std::cerr << "\n";
    std::cerr << "Environment:\n";
    std::cerr << "  SKIPBUILD_DIR (or BUILDCACHE_DIR)                  Cache directory\n";
    std::cerr << "  SKIPBUILD_MAX_SIZE (or BUILDCACHE_MAX_SIZE)             Max cache size (e.g., 10G, 500M)\n";
    std::cerr << "  SKIPBUILD_MAX_AGE (or BUILDCACHE_MAX_AGE)              Max age in days\n";
    std::cerr << "  SKIPBUILD_COMPRESSION (or BUILDCACHE_COMPRESSION)          Enable/disable compression (true/false)\n";
    std::cerr << "  SKIPBUILD_READ_ONLY (or BUILDCACHE_READ_ONLY)            Read-only cache (true/false)\n";
    std::cerr << "  SKIPBUILD_REMOTE_URL (or BUILDCACHE_REMOTE_URL)           Remote cache endpoint\n";
    std::cerr << "  SKIPBUILD_REMOTE_READ_ONLY (or BUILDCACHE_REMOTE_READ_ONLY)     Remote read-only (true/false)\n";
    std::cerr << "  SKIPBUILD_REMOTE_CONNECT_TIMEOUT (or BUILDCACHE_REMOTE_CONNECT_TIMEOUT)  Remote connect timeout in ms\n";
    std::cerr << "  SKIPBUILD_REMOTE_READ_TIMEOUT (or BUILDCACHE_REMOTE_READ_TIMEOUT)      Remote read timeout in ms\n";
    std::cerr << "  SKIPBUILD_REMOTE_MAX_RETRIES (or BUILDCACHE_REMOTE_MAX_RETRIES)  Remote max retries\n";
    std::cerr << "  SKIPBUILD_REMOTE_AUTH_HEADER (or BUILDCACHE_REMOTE_AUTH_HEADER)  Remote auth header\n";
    std::cerr << "  SKIPBUILD_REMOTE_VERIFY_SSL (or BUILDCACHE_REMOTE_VERIFY_SSL)  Remote verify SSL (true/false)\n";
    std::cerr << "  SKIPBUILD_DISABLE_INCREMENTAL_MSBUILD (or BUILDCACHE_DISABLE_INCREMENTAL_MSBUILD)  Auto-disable for MSBuild incremental (true/false)\n";
    std::cerr << "  SKIPBUILD_COMPILER_CHECK (or BUILDCACHE_COMPILER_CHECK)  Compiler fingerprint: mtime, version, content, none (default: mtime)\n";
    std::cerr << "  SKIPBUILD_DISABLE=1             Disable cache (passthrough)\n";
    std::cerr << "  SKIPBUILD_DEBUG=1               Debug logging (legacy)\n";
    std::cerr << "  REMAKE_LOG=<level>              Log level: debug, info, warn, error, none (default: none)\n";
    std::cerr << "  SKIPBUILD_LOG_FILE=<path>       Append [skipbuild] logs to a file (useful under VS/MSBuild; implies info level)\n";
    std::cerr << "\n";
    std::cerr << "Config file (.skipbuild.conf):\n";
    std::cerr << "  max_cache_size = 10G\n";
    std::cerr << "  cache_dir = /path/to/cache\n";
    std::cerr << "  compression_enabled = true\n";
    std::cerr << "  read_only = false\n";
    std::cerr << "  remote_url = http://...\n";
    std::cerr << "  disable_incremental_msbuild = true\n";
    std::cerr << "  compiler_check = mtime\n";
}

skipbuild::CacheConfig load_cache_config(const fs::path& project_root = "") {
    // Check for explicit CLI cache dir override
    std::optional<skipbuild::CacheConfig> cli_overrides;
    
    // Parse command line for --cache-dir (will be handled later in main)
    // For now, just load with full precedence
    return skipbuild::ConfigParser::load_config(project_root, cli_overrides);
}

// Windows: find compiler in PATH
std::string find_compiler_in_path(const std::string& compiler) {
    #ifdef _WIN32
    const char* path_env = std::getenv("PATH");
    if (!path_env) return compiler;
    
    std::string path_str(path_env);
    std::stringstream ss(path_str);
    std::string dir;
    while (std::getline(ss, dir, ';')) {
        fs::path full = fs::path(dir) / compiler;
        if (fs::exists(full)) return full.string();
        // Try with .exe
        full += ".exe";
        if (fs::exists(full)) return full.string();
    }
    #endif
    return compiler;
}

// Replay the compiler with an argv-preserving spawn.
// std::system() re-parses the command through cmd.exe/sh and mangles arguments
// such as quoted defines (-DFOO="bar"), so spawn directly and pass the argument
// vector through unchanged.
#ifdef _WIN32
// Quote one argument exactly the way the MSVC CRT will un-quote it, so embedded
// quotes (\") survive the round-trip to the child process.
static void append_windows_arg(std::string& out, const std::string& arg) {
    if (arg.empty() || arg.find_first_of(" \t\"") != std::string::npos) {
        out += '"';
        size_t backslashes = 0;
        for (char c : arg) {
            if (c == '\\') {
                ++backslashes;
            } else if (c == '"') {
                out.append(backslashes * 2 + 1, '\\');
                out += '"';
                backslashes = 0;
            } else {
                out.append(backslashes, '\\');
                backslashes = 0;
                out += c;
            }
        }
        out.append(backslashes * 2, '\\');
        out += '"';
    } else {
        out += arg;
    }
}
#endif

static int spawn_argv(const std::vector<std::string>& args) {
    if (args.empty()) return 127;

#ifdef _WIN32
    std::string prog = args[0];
    if (prog.find('/') == std::string::npos && prog.find('\\') == std::string::npos) {
        prog = find_compiler_in_path(prog);
    }

    std::string cmdline;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) cmdline += ' ';
        append_windows_arg(cmdline, args[i]);
    }

    std::vector<char> buf(cmdline.begin(), cmdline.end());
    buf.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(prog.c_str(), buf.data(), nullptr, nullptr, TRUE,
                        0, nullptr, nullptr, &si, &pi)) {
        return 127;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 127;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
#else
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return 127;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 127;
#endif
}

int run_compiler(const std::vector<std::string>& args) {
    return spawn_argv(args);
}

// Build the passthrough argument vector for the wrapped compiler: drop argv[0]
// (the launcher itself) and an optional "--" separator, so
// `skipbuild -- g++ ...` and `skipbuild g++ ...` both replay `g++ ...`.
static std::vector<std::string> passthrough_compiler_args(char** argv, int argc) {
    int start = 1;
    if (start < argc && std::string(argv[start]) == "--") ++start;
    return std::vector<std::string>(argv + start, argv + argc);
}

// Run a process, capturing its combined stdout+stderr. Returns false if the
// process could not be started at all.
#ifdef _WIN32
static bool capture_argv(const std::vector<std::string>& args, std::string& out, int& exit_code) {
    if (args.empty()) return false;

    std::string prog = args[0];
    if (prog.find('/') == std::string::npos && prog.find('\\') == std::string::npos) {
        prog = find_compiler_in_path(prog);
    }

    std::string cmdline;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) cmdline += ' ';
        append_windows_arg(cmdline, args[i]);
    }
    std::vector<char> buf(cmdline.begin(), cmdline.end());
    buf.push_back('\0');

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    // A valid inheritable stdin is required when STARTF_USESTDHANDLES is set,
    // otherwise CreateProcess fails on the NULL handle.
    HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = (nul != INVALID_HANDLE_VALUE) ? nul : GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(prog.c_str(), buf.data(), nullptr, nullptr, TRUE,
                             0, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        return false;
    }

    char temp[4096];
    DWORD nread = 0;
    while (ReadFile(rd, temp, sizeof(temp), &nread, nullptr) && nread > 0) {
        out.append(temp, nread);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    exit_code = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return true;
}
#else
static bool capture_argv(const std::vector<std::string>& args, std::string& out, int& exit_code) {
    if (args.empty()) return false;

    int fds[2];
    if (pipe(fds) != 0) return false;

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(fds[1]);

    char temp[4096];
    ssize_t n;
    while ((n = read(fds[0], temp, sizeof(temp))) > 0) {
        out.append(temp, static_cast<size_t>(n));
    }
    close(fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return true;
}
#endif

// Split a makefile-style prerequisite list into paths. Handles the escapes GCC
// emits in dependency files: "\ " (space), "\\" (backslash) and "\#" (hash).
// Other backslash sequences (e.g. Windows path separators) are kept verbatim.
static std::vector<std::string> split_make_deps(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            char next = s[i + 1];
            if (next == ' ' || next == '\t' || next == '#' || next == '\\') {
                cur += next;
                ++i;
                continue;
            }
            cur += c;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Discover the header files a translation unit depends on by running the
// compiler in preprocess/parse-only mode. Returns nullopt if discovery could
// not be performed (in which case the caller must bypass the cache rather than
// risk a stale hit).
static std::optional<std::vector<std::string>> discover_dependencies(
    const std::vector<std::string>& compiler_args,
    skipbuild::CompilerType type) {

    std::vector<std::string> dep_args;
    if (type == skipbuild::CompilerType::MSVC) {
        for (size_t i = 0; i < compiler_args.size(); ++i) {
            const std::string& a = compiler_args[i];
            if (a.rfind("/Fo", 0) == 0 || a.rfind("/Fd", 0) == 0) {
                if (a.size() == 3 && i + 1 < compiler_args.size()) ++i;
                continue;
            }
            dep_args.push_back(a);
        }
        dep_args.push_back("/nologo");
        dep_args.push_back("/showIncludes");
        dep_args.push_back("/Zs");
    } else {
        for (size_t i = 0; i < compiler_args.size(); ++i) {
            const std::string& a = compiler_args[i];
            if (a == "-c") continue;
            if (a == "-o" || a == "-MF" || a == "-MT" || a == "-MQ") {
                if (i + 1 < compiler_args.size()) ++i;
                continue;
            }
            if (a.rfind("-o", 0) == 0 || a.rfind("-MF", 0) == 0) continue;
            dep_args.push_back(a);
        }
        dep_args.push_back("-MM");
        dep_args.push_back("-MG");
    }

    std::string output;
    int exit_code = 0;
    if (!capture_argv(dep_args, output, exit_code)) return std::nullopt;

    std::vector<std::string> deps;
    std::istringstream iss(output);
    std::string line;

    if (type == skipbuild::CompilerType::MSVC) {
        const std::string marker = "Note: including file:";
        while (std::getline(iss, line)) {
            auto pos = line.find(marker);
            if (pos == std::string::npos) continue;
            std::string p = line.substr(pos + marker.size());
            size_t s = p.find_first_not_of(" \t\r\n");
            if (s == std::string::npos) continue;
            size_t e = p.find_last_not_of(" \t\r\n");
            deps.push_back(p.substr(s, e - s + 1));
        }
        if (exit_code != 0 && deps.empty()) return std::nullopt;
    } else {
        // makefile-style: "target: dep1 dep2 \", with '\' line continuations.
        // A trailing backslash continues the line only when it is not itself
        // escaped (an even number of trailing backslashes is a literal one).
        std::string joined;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t trailing = 0;
            while (trailing < line.size() && line[line.size() - 1 - trailing] == '\\') ++trailing;
            if (trailing % 2 == 1) {
                joined += line.substr(0, line.size() - 1);
                joined += ' ';
            } else {
                joined += line;
                joined += '\n';
            }
        }
        std::istringstream js(joined);
        while (std::getline(js, line)) {
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            for (auto& dep : split_make_deps(line.substr(colon + 1))) {
                deps.push_back(std::move(dep));
            }
        }
        if (exit_code != 0) return std::nullopt;
    }
    return deps;
}

// `skipbuild --init [path] [--dry-run]`: insert the integration snippet into a
// CMakeLists.txt, right after project(...). Idempotent.
int run_init_command(int argc, char** argv) {
    bool dry_run = false;
    fs::path target;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--dry-run" || a == "--print" || a == "-n") {
            dry_run = true;
        } else if (target.empty()) {
            target = a;
        }
    }
    if (target.empty()) target = "CMakeLists.txt";

    std::error_code ec;
    if (fs::is_directory(target, ec)) target /= "CMakeLists.txt";
    if (!fs::exists(target, ec)) {
        std::cerr << "[skipbuild] " << target.string() << " not found.\n"
                  << "Run this from your project root, or pass a path:\n"
                  << "  skipbuild --init path/to/CMakeLists.txt\n";
        return 1;
    }

    std::ifstream in(target, std::ios::binary);
    if (!in) {
        std::cerr << "[skipbuild] cannot open " << target.string() << "\n";
        return 1;
    }
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    // Preserve a UTF-8 BOM if present.
    std::string bom;
    if (raw.size() >= 3 &&
        static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB &&
        static_cast<unsigned char>(raw[2]) == 0xBF) {
        bom = raw.substr(0, 3);
        raw = raw.substr(3);
    }

    auto result = skipbuild::apply_integration(raw);
    if (result.status == skipbuild::InitStatus::AlreadyDone) {
        std::cout << "[skipbuild] " << target.string()
                  << " is already integrated. Nothing to do.\n";
        return 0;
    }
    if (result.status == skipbuild::InitStatus::Error) {
        std::cerr << "[skipbuild] no `project(...)` or `cmake_minimum_required(...)` "
                  << "found in " << target.string() << ".\n"
                  << "Add this right after `project(...)` yourself:\n\n"
                  << skipbuild::integration_snippet("\n");
        return 1;
    }
    if (dry_run) {
        std::cout << "[skipbuild] would insert into " << target.string() << ":\n\n"
                  << skipbuild::integration_snippet("\n");
        return 0;
    }

    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "[skipbuild] cannot write " << target.string() << "\n";
        return 1;
    }
    out << bom << result.content;
    out.close();

    std::cout << "[skipbuild] integrated " << target.string() << "\n\n"
              << "Build with Ninja so caching kicks in:\n"
              << "  cmake -S . -B build -G Ninja && cmake --build build\n";
    return 0;
}

// Evict old/oversized cache entries. Runs at most once every few minutes, using
// a marker file in the cache root so the throttle is shared across the short-
// lived launcher processes (an in-process timer would reset every invocation).
static void maybe_prune(skipbuild::Cache& cache, const skipbuild::CacheConfig& config) {
    if (config.read_only) return;

    static constexpr int64_t kMinIntervalSeconds = 300;
    std::filesystem::path marker = config.cache_dir / ".last_cleanup";
    std::error_code ec;

    if (std::filesystem::exists(marker, ec) && !ec) {
        auto last = std::filesystem::last_write_time(marker, ec);
        if (!ec) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                std::filesystem::file_time_type::clock::now() - last).count();
            if (age >= 0 && age < kMinIntervalSeconds) return;
        }
    }

    // Touch the marker before pruning so concurrent processes don't all scan.
    {
        std::ofstream touch(marker, std::ios::trunc);
        touch << "cleanup\n";
    }

    cache.prune(config.max_cache_size_bytes, config.max_age_days);
}

int main(int argc, char* argv[]) {
    // Handle skipbuild-specific commands first
    if (argc >= 2) {
        std::string arg1 = argv[1];
        
        if (arg1 == "--version" || arg1 == "-V") {
            std::cout << "skipbuild " << SKIPBUILD_VERSION << std::endl;
            return 0;
        }
        if (arg1 == "--help" || arg1 == "-h") {
            print_usage(argv[0]);
            return 0;
        }
    }
    
    // Parse CLI args for config overrides before processing commands
    std::optional<skipbuild::CacheConfig> cli_overrides;
    skipbuild::CliOverrideMask cli_mask;
    fs::path cli_cache_dir;
    bool cli_compiler_check_set = false;
    skipbuild::CompilerCheckMode cli_compiler_check = skipbuild::CompilerCheckMode::Mtime;
    std::vector<bool> arg_consumed(argc, false);  // Track consumed arguments
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--cache-dir" && i + 1 < argc) {
            cli_cache_dir = argv[++i];
            arg_consumed[i] = true;
            arg_consumed[i-1] = true;
        } else if (arg == "--max-cache-size" && i + 1 < argc) {
            if (!cli_overrides) cli_overrides = skipbuild::CacheConfig{};
            cli_overrides->max_cache_size_bytes = skipbuild::ConfigParser::parse_size(argv[++i]);
            arg_consumed[i] = true;
            arg_consumed[i-1] = true;
        } else if (arg == "--max-age" && i + 1 < argc) {
            if (!cli_overrides) cli_overrides = skipbuild::CacheConfig{};
            try { cli_overrides->max_age_days = std::stoi(argv[++i]); } catch (...) {}
            arg_consumed[i] = true;
            arg_consumed[i-1] = true;
        } else if (arg == "--compression" && i + 1 < argc) {
            if (!cli_overrides) cli_overrides = skipbuild::CacheConfig{};
            cli_overrides->compression_enabled = skipbuild::ConfigParser::parse_bool(argv[++i]);
            cli_mask.compression_enabled = true;
            arg_consumed[i] = true;
            arg_consumed[i-1] = true;
        } else if (arg == "--read-only" && i + 1 < argc) {
            if (!cli_overrides) cli_overrides = skipbuild::CacheConfig{};
            cli_overrides->read_only = skipbuild::ConfigParser::parse_bool(argv[++i]);
            cli_mask.read_only = true;
            arg_consumed[i] = true;
            arg_consumed[i-1] = true;
        } else if (arg == "--remote-url" && i + 1 < argc) {
            if (!cli_overrides) cli_overrides = skipbuild::CacheConfig{};
            cli_overrides->remote_url = argv[++i];
            arg_consumed[i] = true;
            arg_consumed[i-1] = true;
        } else if (arg == "--remote-read-only" && i + 1 < argc) {
            if (!cli_overrides) cli_overrides = skipbuild::CacheConfig{};
            cli_overrides->read_only = skipbuild::ConfigParser::parse_bool(argv[++i]);
            cli_mask.read_only = true;
            arg_consumed[i] = true;
            arg_consumed[i-1] = true;
        } else if (arg == "--remote-timeout" && i + 1 < argc) {
            if (!cli_overrides) cli_overrides = skipbuild::CacheConfig{};
            try { cli_overrides->remote_connect_timeout_ms = std::stoi(argv[++i]); } catch (...) {}
            cli_overrides->remote_read_timeout_ms = cli_overrides->remote_connect_timeout_ms;
            arg_consumed[i] = true;
            arg_consumed[i-1] = true;
        } else if (arg == "--disable-incremental-msbuild" && i + 1 < argc) {
            if (!cli_overrides) cli_overrides = skipbuild::CacheConfig{};
            cli_overrides->disable_incremental_msbuild = skipbuild::ConfigParser::parse_bool(argv[++i]);
            arg_consumed[i] = true;
            arg_consumed[i-1] = true;
        } else if (arg == "--compiler-check" && i + 1 < argc) {
            cli_compiler_check = skipbuild::parse_compiler_check_mode(argv[++i]);
            cli_compiler_check_set = true;
            arg_consumed[i] = true;
            arg_consumed[i-1] = true;
        }
    }
    
    // Load config with precedence. The current directory is used as the project
    // root so a project-local .skipbuild.conf is discovered (searching upward
    // from the build directory, where the compiler wrapper usually runs).
    std::error_code cwd_ec;
    fs::path project_root = fs::current_path(cwd_ec);
    if (cwd_ec) project_root.clear();
    auto config = skipbuild::ConfigParser::load_config(project_root, cli_overrides, cli_mask);
    if (!cli_cache_dir.empty()) {
        config.cache_dir = cli_cache_dir;
    }
    if (cli_compiler_check_set) {
        config.compiler_check = cli_compiler_check;
    }
    // Apply the resolved compiler fingerprint strategy process-wide
    skipbuild::set_compiler_check_mode(config.compiler_check);
    
    // Build filtered argv (exclude consumed CLI options)
    std::vector<char*> filtered_argv;
    filtered_argv.push_back(argv[0]);  // Program name
    for (int i = 1; i < argc; ++i) {
        if (!arg_consumed[i]) {
            filtered_argv.push_back(argv[i]);
        }
    }
    int filtered_argc = static_cast<int>(filtered_argv.size());
    char** filtered_argv_data = filtered_argv.data();
    
    // Handle skipbuild-specific commands using filtered argv
    std::string arg1 = "";
    if (filtered_argc >= 2) {
        arg1 = filtered_argv_data[1];
    }
    
    if (arg1 == "--version" || arg1 == "-V") {
        std::cout << "skipbuild " << SKIPBUILD_VERSION << std::endl;
        return 0;
    }
    if (arg1 == "--help" || arg1 == "-h") {
        print_usage(filtered_argv_data[0]);
        return 0;
    }
    if (arg1 == "--stats") {
        auto cache = skipbuild::Cache(config.cache_dir, config);
        cache.print_stats();
        return 0;
    }
    if (arg1 == "--clear-stats" || arg1 == "--zero-stats") {
        auto cache = skipbuild::Cache(config.cache_dir, config);
        cache.clear_stats();
        std::cout << "Cache statistics cleared." << std::endl;
        return 0;
    }
    if (arg1 == "--clean") {
        fs::remove_all(config.cache_dir);
        std::cout << "Cache cleaned" << std::endl;
        return 0;
    }
    if (arg1 == "--init") {
        return run_init_command(filtered_argc, filtered_argv_data);
    }
    if (arg1 == "--remote-stats") {
        auto cache = skipbuild::Cache(config.cache_dir, config);
        auto rstats = cache.get_remote_stats();
        std::cout << "=== Remote Cache Statistics ===\n";
        std::cout << "Remote Hits:    " << rstats.remote_hits << "\n";
        std::cout << "Remote Misses:  " << rstats.remote_misses << "\n";
        std::cout << "Uploads:        " << rstats.uploads << "\n";
        std::cout << "Downloads:      " << rstats.downloads << "\n";
        std::cout << "Errors:         " << rstats.errors << "\n";
        if (rstats.remote_hits + rstats.remote_misses > 0) {
            double hit_rate = (static_cast<double>(rstats.remote_hits) / 
                              (rstats.remote_hits + rstats.remote_misses)) * 100.0;
            std::cout << "Remote Hit Rate: " << std::fixed << std::setprecision(1) << hit_rate << "%\n";
        }
        std::cout << "==============================\n";
        return 0;
    }
    if (arg1 == "--cache-dir") {
        if (filtered_argc >= 3) {
            std::cout << "Cache dir: " << filtered_argv_data[2] << std::endl;
            return 0;
        }
        std::cerr << "--cache-dir requires a path\n";
        return 1;
    }
    
    // Check for disable
    if (std::getenv("SKIPBUILD_DISABLE")) {
        return run_compiler(passthrough_compiler_args(filtered_argv_data, filtered_argc));
    }

    // Debug logging (moved earlier for MSBuild detection)
    // Use REMAKE_LOG environment variable for log level (debug/info/warn/error/none)
    // SKIPBUILD_DEBUG=1 is still supported for backwards compatibility
    bool debug = skipbuild::get_log_level() >= skipbuild::LogLevel::Debug;

    // Auto-disable for MSBuild incremental builds (MSBuild native incremental is faster)
    // Check if running under MSBuild and output is newer than source
    if (config.disable_incremental_msbuild) {
        bool is_msbuild = std::getenv("MSBUILD") != nullptr || 
                          std::getenv("BuildingInsideVisualStudio") != nullptr ||
                          std::getenv("MSBuildProjectDirectory") != nullptr;
        
        if (is_msbuild) {
            // Check if this is a compile command with -c or /c
            bool is_compile = false;
            for (int i = 1; i < filtered_argc; ++i) {
                std::string arg = filtered_argv_data[i];
                if (arg == "-c" || arg == "/c") {
                    is_compile = true;
                    break;
                }
            }
            
            if (is_compile) {
                // Try to find output file (/Fo or -o) and source file
                std::string output_file;
                std::string source_file;
                for (int i = 1; i < filtered_argc; ++i) {
                    std::string arg = filtered_argv_data[i];
                    if ((arg == "/Fo" || arg == "-o") && i + 1 < filtered_argc) {
                        output_file = filtered_argv_data[i + 1];
                    } else if (arg.rfind("/Fo", 0) == 0) {
                        output_file = arg.substr(3);
                    } else if (arg.rfind("-o", 0) == 0 && arg.size() > 2) {
                        output_file = arg.substr(2);
                    } else if (fs::path(arg).extension() == ".cpp" || 
                               fs::path(arg).extension() == ".cc" ||
                               fs::path(arg).extension() == ".cxx" ||
                               fs::path(arg).extension() == ".c") {
                        source_file = arg;
                    }
                }
                
                if (!output_file.empty() && !source_file.empty()) {
                    std::error_code ec;
                    auto out_time = fs::last_write_time(output_file, ec);
                    auto src_time = fs::last_write_time(source_file, ec);
                    if (!ec && out_time > src_time) {
                        // Output is newer than source - incremental build, skip cache
                        skipbuild::log_debug("MSBuild incremental detected (output newer than source), bypassing cache");
                        return run_compiler(passthrough_compiler_args(filtered_argv_data, filtered_argc));
                    }
                }
            }
        }
    }
    
    // Find separator "--"
    int sep = -1;
    for (int i = 1; i < filtered_argc; ++i) {
        if (std::string(filtered_argv_data[i]) == "--") {
            sep = i;
            break;
        }
    }
    
    int compiler_start;
    int compiler_argc;
    char** compiler_argv;
    
    if (sep == -1) {
        // No separator - treat as compiler wrapper: skipbuild g++ -c foo.cpp
        if (filtered_argc < 3) {
            print_usage(filtered_argv_data[0]);
            return 1;
        }
        compiler_start = 1;
        compiler_argc = filtered_argc - 1;
        compiler_argv = filtered_argv_data + 1;
    } else {
        // With separator: skipbuild -- g++ -c foo.cpp
        compiler_start = sep + 1;
        compiler_argc = filtered_argc - compiler_start;
        compiler_argv = filtered_argv_data + compiler_start;
        
        if (compiler_argc == 0) {
            std::cerr << "No compiler command after --\n";
            return 1;
        }
    }
    
    // Resolve command to full path for hashing
    std::string cmd_name = compiler_argv[0];
    std::string cmd_full = find_compiler_in_path(cmd_name);
    compiler_argv[0] = const_cast<char*>(cmd_full.c_str());
    
    // A compile-only invocation (-c on GCC/Clang, /c on MSVC) is never a link,
    // even when the driver is a C++ compiler such as g++/clang++ that also acts
    // as the linker. Without this, every g++/clang++ compile is misrouted to the
    // linker cache, whose key ignores the source and compile flags.
    bool has_compile_only_flag = false;
    for (int i = 1; i < compiler_argc; ++i) {
        if (std::string(compiler_argv[i]) == "-c" || std::string(compiler_argv[i]) == "/c") {
            has_compile_only_flag = true;
            break;
        }
    }

    // Detect if this is a linker command
    auto linker_type = skipbuild::detect_linker_type(cmd_full);
    bool is_linker = (!has_compile_only_flag) &&
                     (linker_type != skipbuild::LinkerType::Unknown);
    
    if (is_linker) {
        // Handle linker command
        auto link_cmd = skipbuild::parse_linker_command(compiler_argc, compiler_argv);
        if (!link_cmd) {
            std::vector<std::string> args(compiler_argv, compiler_argv + compiler_argc);
            return run_compiler(args);
        }
        
        skipbuild::log_debug("Linker: " + link_cmd->linker_path + " (" + link_cmd->linker_hash.substr(0, 8) + ")");
        skipbuild::log_debug("Output: " + link_cmd->output.string());
        skipbuild::log_debug("Inputs: " + std::to_string(link_cmd->inputs.size()));
        
        // Get cache
        auto cache = skipbuild::Cache(config.cache_dir, config);
        
        // Compute linker key
        auto key = skipbuild::compute_linker_key(*link_cmd);
        
        skipbuild::log_debug("Linker Key: " + key.hash.substr(0, 16));
        
        // Check cache with safe_get (fail-safe)
        auto entry = cache.safe_get(key);
        if (entry) {
            skipbuild::log_debug("LINKER CACHE HIT");
            std::cout << "[skipbuild] LINK HIT " << key.hash.substr(0, 12) << "\n";
            
            // Restore output file(s)
            bool restore_ok = true;
            for (auto& cached_out : entry->outputs) {
                std::error_code ec;
                // Restore each artifact under its original filename next to the
                // linker output. For DLL links both the .dll and its import .lib
                // are cached; writing them all to link_cmd->output would clobber
                // the DLL with the import library.
                fs::path target = link_cmd->output.parent_path() / cached_out.filename();
                fs::copy_file(cached_out, target, fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    restore_ok = false;
                    skipbuild::log_debug("Failed to restore: " + ec.message());
                }
                skipbuild::log_debug("Restored: " + target.string());
            }
            // Done with the staged outputs either way: release now so a failed
            // restore does not leak the private staging directory.
            cache.release(*entry);
            if (restore_ok) {
                return 0;
            }
            // If restore failed, fall through to native execution
            skipbuild::log_debug("Cache restore failed, falling back to native linker");
        }
        
        // Cache miss or restore failed - link natively
        skipbuild::log_debug("LINKER CACHE MISS");
        std::cout << "[skipbuild] LINK MISS " << key.hash.substr(0, 12) << " -> linking...\n";
        
        auto start = std::chrono::high_resolution_clock::now();
        std::vector<std::string> args(compiler_argv, compiler_argv + compiler_argc);
        int exit_code = run_compiler(args);
        auto end = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        
        if (exit_code != 0) {
            std::cerr << "[skipbuild] Linking failed with exit code " << exit_code << "\n";
            return exit_code;
        }
        
        skipbuild::log_debug("Linked in " + std::to_string(ms) + "ms");
        
        // Record direct execute for stats (fallback linking)
        cache.record_direct_execute();
        
        // Store in cache with safe_put (fail-safe)
        std::vector<fs::path> outputs;
        if (fs::exists(link_cmd->output)) {
            outputs.push_back(link_cmd->output);
            // Also capture import library for DLLs
            if (link_cmd->output.extension() == ".dll") {
                fs::path implib = link_cmd->output;
                implib.replace_extension(".lib");
                if (fs::exists(implib)) outputs.push_back(implib);
            }
        }
        if (!outputs.empty()) {
            cache.safe_put(key, outputs);
            skipbuild::log_debug("Stored in cache (" + std::to_string(outputs.size()) + " files)");
            maybe_prune(cache, config);
        }
        
        return 0;
    }
    
    // Compiler command
    auto cmd = skipbuild::parse_compile_command(compiler_argc, compiler_argv);
    if (!cmd) {
        // Not a compile command we understand, passthrough
        std::vector<std::string> args(compiler_argv, compiler_argv + compiler_argc);
        return run_compiler(args);
    }
    
    // Only cache compilation (-c or /c flag produces .o/.obj)
    bool is_compile_only = false;
    for (auto& a : cmd->args) {
        if (a == "-c" || a == "/c") { is_compile_only = true; break; }
    }
    if (!is_compile_only) {
        std::vector<std::string> args(compiler_argv, compiler_argv + compiler_argc);
        return run_compiler(args);
    }
    
    // Discover the headers this translation unit depends on so that editing any
    // header (directly or transitively) invalidates the cache entry. If we
    // cannot determine the dependency set, bypass the cache entirely rather
    // than risk returning a stale object.
    {
        std::vector<std::string> raw_args(compiler_argv, compiler_argv + compiler_argc);
        auto ctype = skipbuild::detect_compiler(cmd->compiler_path).type;
        auto deps = discover_dependencies(raw_args, ctype);
        if (!deps) {
            skipbuild::log_debug("Dependency discovery unavailable; bypassing cache");
            return run_compiler(raw_args);
        }
        if (debug) {
            std::cerr << "[skipbuild] Dependencies found: " << deps->size() << "\n";
            for (auto& d : *deps) std::cerr << "[skipbuild]   " << d << "\n";
        }
        cmd->dependencies = *deps;
    }
    
    // Get cache
    auto cache = skipbuild::Cache(config.cache_dir, config);
    
    // Compute key
    auto key = skipbuild::Cache::compute_key_from_command(*cmd);
    
    // Debug logging
    skipbuild::log_debug("Source: " + cmd->source.string());
    skipbuild::log_debug("Output: " + cmd->output.string());
    skipbuild::log_debug("Key: " + key.hash.substr(0, 16));
    skipbuild::log_debug("Compiler: " + cmd->compiler_path + " (" + cmd->compiler_hash.substr(0, 8) + ")");
    
    // Check cache with safe_get (fail-safe)
    auto entry = cache.safe_get(key);
    if (entry) {
        skipbuild::log_debug("CACHE HIT");
        std::cout << "[skipbuild] HIT " << key.hash.substr(0, 12) << "\n";
        
        // Restore output file(s)
        bool restore_ok = true;
        for (auto& cached_out : entry->outputs) {
            fs::path target;
            if (cached_out.filename() == cmd->pch.pch_output.filename() && !cmd->pch.pch_output.empty()) {
                target = cmd->pch.pch_output;
            } else {
                target = cmd->output;
            }
            std::error_code ec;
            fs::copy_file(cached_out, target, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                restore_ok = false;
                skipbuild::log_debug("Failed to restore: " + ec.message());
            }
            skipbuild::log_debug("Restored: " + target.string());
        }
        
        // Restore sidecar files
        for (auto& sidecar : entry->sidecars) {
            fs::path target = sidecar.original_path.empty() ? 
                (cmd->output.parent_path() / sidecar.relative_path.filename()) : 
                fs::path(sidecar.original_path);
            std::error_code ec;
            fs::copy_file(sidecar.relative_path, target, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                restore_ok = false;
                skipbuild::log_debug("Failed to restore sidecar: " + ec.message());
            }
            skipbuild::log_debug("Restored sidecar: " + target.string());
        }
        
        // Release the staged copies whether or not the restore succeeded, so a
        // failed restore does not leak the private staging directory.
        cache.release(*entry);
        if (restore_ok) {
            return 0;
        }
        // If restore failed, fall through to native execution
        skipbuild::log_debug("Cache restore failed, falling back to native compiler");
    }
    
    // Cache miss or restore failed - compile natively
    skipbuild::log_debug("CACHE MISS");
    std::cout << "[skipbuild] MISS " << key.hash.substr(0, 12) << " -> compiling...\n";
    
    auto start = std::chrono::high_resolution_clock::now();
    std::vector<std::string> args(compiler_argv, compiler_argv + compiler_argc);
    int exit_code = run_compiler(args);
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    if (exit_code != 0) {
        std::cerr << "[skipbuild] Compilation failed with exit code " << exit_code << "\n";
        return exit_code;
    }
    
    skipbuild::log_debug("Compiled in " + std::to_string(ms) + "ms");
    
    // Record direct execute for stats (fallback compilation)
    cache.record_direct_execute();
    
    // Store in cache with safe_put (fail-safe)
    std::vector<fs::path> outputs;
    std::vector<std::pair<fs::path, skipbuild::SidecarType>> sidecars;
    
    if (fs::exists(cmd->output)) {
        outputs.push_back(cmd->output);
    }
    
    // Also cache PCH file if creating one
    if (cmd->pch.mode == skipbuild::PCHMode::Create && !cmd->pch.pch_output.empty() && fs::exists(cmd->pch.pch_output)) {
        outputs.push_back(cmd->pch.pch_output);
        skipbuild::log_debug("Caching PCH: " + cmd->pch.pch_output.string());
    }
    
    // Collect debug sidecar files if debug info was requested
    if (cmd->has_debug_info) {
        for (size_t i = 0; i < cmd->expected_sidecars.size(); ++i) {
            fs::path sidecar_path = cmd->expected_sidecars[i];
            if (fs::exists(sidecar_path)) {
                sidecars.emplace_back(sidecar_path, cmd->expected_sidecar_types[i]);
                skipbuild::log_debug("Caching sidecar: " + sidecar_path.string());
            }
        }
    }
    
    if (!outputs.empty()) {
        cache.safe_put(key, outputs, sidecars);
        skipbuild::log_debug("Stored in cache (" + std::to_string(outputs.size()) + " outputs, " + std::to_string(sidecars.size()) + " sidecars)");
        maybe_prune(cache, config);
    }
    
    return 0;
}
