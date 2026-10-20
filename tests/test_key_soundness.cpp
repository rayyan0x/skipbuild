// Verifies that the cache key actually depends on everything that affects the
// compiled object: preprocessor defines, language-standard and other flags, and
// the content of transitively-included headers.
#include "core.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <string>

namespace fs = std::filesystem;

static skipbuild::CacheKey key_for(const std::string& compiler,
                                   const std::vector<std::string>& args) {
    std::vector<std::string> full;
    full.push_back(compiler);
    full.insert(full.end(), args.begin(), args.end());

    std::vector<char*> argv;
    argv.reserve(full.size());
    for (auto& a : full) argv.push_back(const_cast<char*>(a.c_str()));

    auto cmd = skipbuild::parse_compile_command(static_cast<int>(argv.size()), argv.data());
    if (!cmd) {
        std::cerr << "parse_compile_command failed for " << compiler << "\n";
        std::exit(2);
    }
    return skipbuild::Cache::compute_key_from_command(*cmd);
}

int main() {
    int failures = 0;
    auto check = [&](bool cond, const char* msg) {
        if (cond) {
            std::cout << "PASS: " << msg << "\n";
        } else {
            std::cerr << "FAIL: " << msg << "\n";
            ++failures;
        }
    };

    // Defines must change the key (attached GCC and MSVC forms).
    check(key_for("fake-g++.exe", {"-c", "a.cpp", "-DFOO=1"}).hash !=
          key_for("fake-g++.exe", {"-c", "a.cpp", "-DFOO=2"}).hash,
          "GCC -DFOO=1 vs -DFOO=2 produce different keys");

    check(key_for("fake-cl.exe", {"/c", "a.cpp", "/DFOO=1"}).hash !=
          key_for("fake-cl.exe", {"/c", "a.cpp", "/DFOO=2"}).hash,
          "MSVC /DFOO=1 vs /DFOO=2 produce different keys");

    // Separate-form define.
    check(key_for("fake-g++.exe", {"-c", "a.cpp", "-D", "FOO=1"}).hash !=
          key_for("fake-g++.exe", {"-c", "a.cpp", "-D", "FOO=2"}).hash,
          "GCC separate -D FOO=1 vs FOO=2 produce different keys");

    // Language-standard flags must change the key.
    check(key_for("fake-cl.exe", {"/c", "a.cpp", "/std:c++17"}).hash !=
          key_for("fake-cl.exe", {"/c", "a.cpp", "/std:c++20"}).hash,
          "MSVC /std:c++17 vs /std:c++20 produce different keys");

    // Other previously-unhashed flags must change the key.
    check(key_for("fake-g++.exe", {"-c", "a.cpp", "-O2", "-pthread"}).hash !=
          key_for("fake-g++.exe", {"-c", "a.cpp", "-O2"}).hash,
          "GCC -pthread changes the key");

    check(key_for("fake-cl.exe", {"/c", "a.cpp", "/MD"}).hash !=
          key_for("fake-cl.exe", {"/c", "a.cpp", "/MT"}).hash,
          "MSVC /MD vs /MT produce different keys");

    // Determinism: identical commands produce identical keys.
    check(key_for("fake-g++.exe", {"-c", "a.cpp", "-DFOO=1"}).hash ==
          key_for("fake-g++.exe", {"-c", "a.cpp", "-DFOO=1"}).hash,
          "identical commands produce identical keys");

    // Order sensitivity: -D/-U and include order are semantically significant, so
    // reordering them must change the key.
    check(key_for("fake-g++.exe", {"-c", "a.cpp", "-DFOO", "-UFOO"}).hash !=
          key_for("fake-g++.exe", {"-c", "a.cpp", "-UFOO", "-DFOO"}).hash,
          "GCC -DFOO -UFOO vs -UFOO -DFOO produce different keys");

    check(key_for("fake-g++.exe", {"-c", "a.cpp", "-I", "inc1", "-I", "inc2"}).hash !=
          key_for("fake-g++.exe", {"-c", "a.cpp", "-I", "inc2", "-I", "inc1"}).hash,
          "GCC include order affects the key");

    // Header dependencies: changing header content must change the key.
    fs::path dir = fs::temp_directory_path() / "skipbuild_key_soundness";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    fs::path header = dir / "h.h";
    fs::path source = dir / "a.cpp";
    { std::ofstream(source) << "#include \"h.h\"\nint f(){return 0;}\n"; }
    { std::ofstream(header) << "#define V 1\n"; }

    auto key_with_deps = [&](const std::vector<std::string>& deps) {
        std::vector<std::string> full = {"fake-g++.exe", "-c", source.string()};
        std::vector<char*> argv;
        for (auto& a : full) argv.push_back(const_cast<char*>(a.c_str()));
        auto cmd = skipbuild::parse_compile_command(static_cast<int>(argv.size()), argv.data());
        cmd->dependencies = deps;
        return skipbuild::Cache::compute_key_from_command(*cmd);
    };

    auto k1 = key_with_deps({header.string()});
    { std::ofstream(header) << "#define V 2\n"; }
    auto k2 = key_with_deps({header.string()});
    check(k1.hash != k2.hash, "header content change invalidates the key");

    // Same header content -> same key (stability).
    auto k3 = key_with_deps({header.string()});
    check(k2.hash == k3.hash, "unchanged header keeps the key stable");

    fs::remove_all(dir, ec);

    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "All key-soundness checks passed\n";
    return 0;
}
