#include "core.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;

void test_compiler_hash_changes_on_binary_change() {
    std::cout << "Testing compiler hash changes on binary change...\n";
    
    // Create a fake compiler binary
    fs::path test_dir = fs::temp_directory_path() / "remake-test-compiler-hash";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    fs::path compiler1 = test_dir / "fake_compiler.exe";
    fs::path compiler2 = test_dir / "fake_compiler2.exe";
    
    std::ofstream(compiler1) << "compiler binary v1";
    std::ofstream(compiler2) << "compiler binary v2";
    
    // Get hashes
    std::string hash1 = skipbuild::get_compiler_hash(compiler1.string());
    std::string hash2 = skipbuild::get_compiler_hash(compiler2.string());
    
    assert(hash1 != hash2);
    assert(hash1.size() == 64); // SHA256 hex
    assert(hash2.size() == 64);
    
    std::cout << "  Hash1: " << hash1.substr(0, 16) << "...\n";
    std::cout << "  Hash2: " << hash2.substr(0, 16) << "...\n";
    std::cout << "  PASS: Different binaries produce different hashes\n";
    
    // Test same file produces same hash
    std::string hash1_again = skipbuild::get_compiler_hash(compiler1.string());
    assert(hash1 == hash1_again);
    std::cout << "  PASS: Same binary produces same hash\n";
    
    fs::remove_all(test_dir);
}

void test_path_normalization() {
    std::cout << "Testing path normalization...\n";
    
    // Test 1: Backslash to forward slash
    fs::path p1 = "C:\\Users\\Project\\src\\file.cpp";
    std::string norm1 = skipbuild::normalize_path_for_key(p1, "C:\\Users\\Project");
    assert(norm1 == "src/file.cpp" || norm1 == "src\\file.cpp"); // separator depends on platform
    #ifdef _WIN32
    assert(norm1 == "src/file.cpp"); // Should be normalized to forward slash
    #endif
    std::cout << "  PASS: Backslash normalized: " << norm1 << "\n";
    
    // Test 2: Case normalization on Windows
    #ifdef _WIN32
    fs::path p2 = "C:\\Users\\PROJECT\\SRC\\FILE.CPP";
    std::string norm2 = skipbuild::normalize_path_for_key(p2, "C:\\Users\\Project");
    assert(norm2 == "src/file.cpp");
    std::cout << "  PASS: Case normalized: " << norm2 << "\n";
    #endif
    
    // Test 3: Absolute paths from different roots produce same relative path
    fs::path src1 = "/home/user1/project/src/main.cpp";
    fs::path src2 = "/home/user2/project/src/main.cpp";
    std::string norm_src1 = skipbuild::normalize_path_for_key(src1, "/home/user1/project");
    std::string norm_src2 = skipbuild::normalize_path_for_key(src2, "/home/user2/project");
    assert(norm_src1 == norm_src2);
    std::cout << "  PASS: Different absolute paths normalized to same: " << norm_src1 << "\n";
    
    // Test 4: Path with build root not at start
    // Use a relative path to avoid platform-specific absolute path handling
    fs::path p3 = "src/subdir/file.cpp";
    std::string norm3 = skipbuild::normalize_path_for_key(p3, "/home/user/project");
    // Relative path should remain unchanged (no root to strip)
    assert(norm3 == "src/subdir/file.cpp");
    std::cout << "  PASS: Relative path unchanged: " << norm3 << "\n";
}

void test_cache_key_determinism() {
    std::cout << "Testing cache key determinism...\n";
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-key-determinism";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    fs::path src = test_dir / "src" / "main.cpp";
    fs::create_directories(src.parent_path());
    std::ofstream(src) << "int main() { return 0; }";
    
    std::vector<std::string> includes = {"include", "src"};
    std::vector<std::string> flags = {"-O2", "-Wall"};
    std::string compiler_hash = "test-compiler-hash";
    
    // Compute key multiple times
    auto key1 = skipbuild::Cache::compute_key(src, includes, flags, compiler_hash, skipbuild::CompilerType::GCC);
    auto key2 = skipbuild::Cache::compute_key(src, includes, flags, compiler_hash, skipbuild::CompilerType::GCC);
    
    assert(key1.hash == key2.hash);
    assert(key1.toolchain_id == key2.toolchain_id);
    std::cout << "  PASS: Same inputs produce same key: " << key1.hash.substr(0, 16) << "...\n";
    
    // Test different source produces different key
    fs::path src2 = test_dir / "src" / "other.cpp";
    fs::create_directories(src2.parent_path());
    std::ofstream(src2) << "int foo() { return 1; }";
    
    auto key3 = skipbuild::Cache::compute_key(src2, includes, flags, compiler_hash, skipbuild::CompilerType::GCC);
    assert(key1.hash != key3.hash);
    std::cout << "  PASS: Different source produces different key\n";
    
    // Test different flags produce different key
    std::vector<std::string> flags2 = {"-O3", "-Wall"};
    auto key4 = skipbuild::Cache::compute_key(src, includes, flags2, compiler_hash, skipbuild::CompilerType::GCC);
    assert(key1.hash != key4.hash);
    std::cout << "  PASS: Different flags produce different key\n";
    
    // Test different includes produce different key
    std::vector<std::string> includes2 = {"include", "src", "extra"};
    auto key5 = skipbuild::Cache::compute_key(src, includes2, flags, compiler_hash, skipbuild::CompilerType::GCC);
    assert(key1.hash != key5.hash);
    std::cout << "  PASS: Different includes produce different key\n";
    
    // Test different compiler hash produces different key
    auto key6 = skipbuild::Cache::compute_key(src, includes, flags, "different-compiler-hash", skipbuild::CompilerType::GCC);
    assert(key1.hash != key6.hash);
    std::cout << "  PASS: Different compiler hash produces different key\n";
    
    fs::remove_all(test_dir);
}

void test_cache_key_same_across_different_build_dirs() {
    std::cout << "Testing cache key same across different build directories...\n";
    
    // Simulate two developers building from different directories
    fs::path test_dir1 = fs::temp_directory_path() / "remake-test-user1" / "project";
    fs::path test_dir2 = fs::temp_directory_path() / "remake-test-user2" / "myproject";
    
    if (fs::exists(test_dir1)) fs::remove_all(test_dir1);
    if (fs::exists(test_dir2)) fs::remove_all(test_dir2);
    
    fs::create_directories(test_dir1 / "src");
    fs::create_directories(test_dir2 / "src");
    
    std::ofstream(test_dir1 / "src" / "main.cpp") << "int main() { return 0; }";
    std::ofstream(test_dir2 / "src" / "main.cpp") << "int main() { return 0; }";
    
    std::vector<std::string> includes = {"include"};
    std::vector<std::string> flags = {"-O2"};
    std::string compiler_hash = "test-compiler-hash";
    
    auto key1 = skipbuild::Cache::compute_key(test_dir1 / "src" / "main.cpp", includes, flags, compiler_hash, skipbuild::CompilerType::GCC);
    auto key2 = skipbuild::Cache::compute_key(test_dir2 / "src" / "main.cpp", includes, flags, compiler_hash, skipbuild::CompilerType::GCC);
    
    assert(key1.hash == key2.hash);
    std::cout << "  PASS: Same source from different dirs produces same key: " << key1.hash.substr(0, 16) << "...\n";
    
    fs::remove_all(test_dir1.parent_path());
    fs::remove_all(test_dir2.parent_path());
}

void test_environment_variable_hash() {
    std::cout << "Testing environment variable hash...\n";
    
    // Set some test environment variables
    #ifdef _WIN32
    _putenv_s("CXXFLAGS", "-O2 -Wall");
    _putenv_s("INCLUDE", "C:\\include");
    _putenv_s("TEMP", "C:\\temp"); // Should be ignored
    _putenv_s("USER", "testuser"); // Should be ignored
    #else
    setenv("CXXFLAGS", "-O2 -Wall", 1);
    setenv("INCLUDE", "/usr/include", 1);
    setenv("TEMP", "/tmp", 1); // Should be ignored
    setenv("USER", "testuser", 1); // Should be ignored
    #endif
    
    std::string env_hash1 = skipbuild::get_relevant_env_hash();
    std::cout << "  Env hash 1: " << env_hash1.substr(0, 16) << "...\n";
    
    // Change relevant env var
    #ifdef _WIN32
    _putenv_s("CXXFLAGS", "-O3 -Wall");
    #else
    setenv("CXXFLAGS", "-O3 -Wall", 1);
    #endif
    
    std::string env_hash2 = skipbuild::get_relevant_env_hash();
    std::cout << "  Env hash 2: " << env_hash2.substr(0, 16) << "...\n";
    
    assert(env_hash1 != env_hash2);
    std::cout << "  PASS: Changing relevant env var changes hash\n";
    
    // Change irrelevant env var
    #ifdef _WIN32
    _putenv_s("TEMP", "C:\\different_temp");
    #else
    setenv("TEMP", "/different_tmp", 1);
    #endif
    
    std::string env_hash3 = skipbuild::get_relevant_env_hash();
    assert(env_hash2 == env_hash3);
    std::cout << "  PASS: Changing irrelevant env var doesn't change hash\n";
    
    // Clean up
    #ifdef _WIN32
    _putenv_s("CXXFLAGS", "");
    _putenv_s("INCLUDE", "");
    _putenv_s("TEMP", "");
    _putenv_s("USER", "");
    #else
    unsetenv("CXXFLAGS");
    unsetenv("INCLUDE");
    unsetenv("TEMP");
    unsetenv("USER");
    #endif
}

void test_find_build_root() {
    std::cout << "Testing find_build_root...\n";
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-build-root";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir / "src" / "subdir");
    
    // Create a CMakeLists.txt at project root
    std::ofstream(test_dir / "CMakeLists.txt") << "cmake_minimum_required(VERSION 3.10)";
    
    fs::path source = test_dir / "src" / "subdir" / "file.cpp";
    std::ofstream(source) << "int main() { return 0; }";
    
    fs::path root = skipbuild::find_build_root(source);
    assert(root == fs::weakly_canonical(test_dir));
    std::cout << "  PASS: Found build root at: " << root << "\n";
    
    // Test without CMakeLists.txt - should fall back to parent
    fs::remove(test_dir / "CMakeLists.txt");
    fs::path root2 = skipbuild::find_build_root(source);
    assert(root2 == fs::weakly_canonical(test_dir / "src" / "subdir"));
    std::cout << "  PASS: Falls back to source parent: " << root2 << "\n";
    
    fs::remove_all(test_dir);
}

void test_linker_key_normalization() {
    std::cout << "Testing linker key normalization...\n";
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-linker";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    skipbuild::LinkerCommand cmd;
    cmd.inputs = {
        test_dir / "obj1.o",
        test_dir / "obj2.o"
    };
    cmd.libraries = {"-lfoo", "-lbar"};
    cmd.library_dirs = {(test_dir / "lib").string(), "/usr/lib"};
    cmd.output = test_dir / "myapp";
    cmd.linker_hash = "test-linker-hash";
    
    // Create dummy input files
    std::ofstream(cmd.inputs[0]) << "obj1";
    std::ofstream(cmd.inputs[1]) << "obj2";
    
    auto key1 = skipbuild::compute_linker_key(cmd);
    auto key2 = skipbuild::compute_linker_key(cmd);
    
    assert(key1.hash == key2.hash);
    std::cout << "  PASS: Linker key deterministic: " << key1.hash.substr(0, 16) << "...\n";
    
    // Link order is significant (e.g. for static libraries / --start-group), so
    // reordering the inputs must change the key.
    std::swap(cmd.inputs[0], cmd.inputs[1]);
    auto key3 = skipbuild::compute_linker_key(cmd);
    assert(key1.hash != key3.hash);
    std::cout << "  PASS: Linker key is order sensitive\n";

    // Reordering the libraries must also change the key.
    std::swap(cmd.libraries[0], cmd.libraries[1]);
    auto key4 = skipbuild::compute_linker_key(cmd);
    assert(key3.hash != key4.hash);
    std::cout << "  PASS: Linker key is library-order sensitive\n";

    // Editing an input's contents must change the key.
    std::ofstream(cmd.inputs[0]) << "obj1-modified";
    auto key5 = skipbuild::compute_linker_key(cmd);
    assert(key4.hash != key5.hash);
    std::cout << "  PASS: Linker key reflects input contents\n";
    
    fs::remove_all(test_dir);
}

// Create a small fake compiler executable/script that prints a fixed version
static fs::path make_fake_compiler(const fs::path& dir, const std::string& version) {
#ifdef _WIN32
    fs::path p = dir / "fake_compiler.bat";
    std::ofstream(p, std::ios::binary) << "@echo off\r\necho FakeCompiler " << version << "\r\n";
#else
    fs::path p = dir / "fake_compiler.sh";
    std::ofstream(p, std::ios::binary) << "#!/bin/sh\necho \"FakeCompiler " << version << "\"\n";
    fs::permissions(p, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add);
#endif
    return p;
}

void test_compiler_check_modes() {
    std::cout << "Testing compiler check modes...\n";
    using skipbuild::CompilerCheckMode;
    auto saved = skipbuild::get_compiler_check_mode();
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-compiler-check";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    fs::path compiler = make_fake_compiler(test_dir, "1.0.0");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    
    // none: empty fingerprint (compiler excluded from the key)
    skipbuild::set_compiler_check_mode(CompilerCheckMode::None);
    assert(skipbuild::get_compiler_hash(compiler.string()).empty());
    std::cout << "  PASS: 'none' yields empty fingerprint\n";
    
    // content: stable across mtime changes, changes when content changes
    skipbuild::set_compiler_check_mode(CompilerCheckMode::Content);
    std::string content1 = skipbuild::get_compiler_hash(compiler.string());
    assert(content1.size() == 64);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    make_fake_compiler(test_dir, "1.0.0");  // same bytes, new mtime
    std::string content2 = skipbuild::get_compiler_hash(compiler.string());
    assert(content1 == content2);
    make_fake_compiler(test_dir, "2.0.0");  // different bytes
    std::string content3 = skipbuild::get_compiler_hash(compiler.string());
    assert(content1 != content3);
    std::cout << "  PASS: 'content' stable across mtime, changes on content\n";
    
    // version: stable across mtime changes (same version output)
    make_fake_compiler(test_dir, "1.0.0");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    skipbuild::set_compiler_check_mode(CompilerCheckMode::Version);
    std::string ver1 = skipbuild::get_compiler_hash(compiler.string());
    assert(ver1.size() == 64);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    make_fake_compiler(test_dir, "1.0.0");  // same bytes + version, new mtime
    std::string ver2 = skipbuild::get_compiler_hash(compiler.string());
    assert(ver1 == ver2);
    std::cout << "  PASS: 'version' stable across mtime changes\n";
    
    // mtime: changes when mtime changes
    skipbuild::set_compiler_check_mode(CompilerCheckMode::Mtime);
    std::string mtime1 = skipbuild::get_compiler_hash(compiler.string());
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    make_fake_compiler(test_dir, "1.0.0");  // same bytes, new mtime
    std::string mtime2 = skipbuild::get_compiler_hash(compiler.string());
    assert(mtime1 != mtime2);
    std::cout << "  PASS: 'mtime' changes when mtime changes\n";
    
    skipbuild::set_compiler_check_mode(saved);
    fs::remove_all(test_dir);
}

void test_log_file_output() {
    std::cout << "Testing SKIPBUILD_LOG_FILE capture...\n";
    fs::path log_path = fs::temp_directory_path() / "remake-test-logfile.log";
    if (fs::exists(log_path)) fs::remove(log_path);
    
#ifdef _WIN32
    _putenv_s("REMAKE_LOG", "debug");
    _putenv_s("SKIPBUILD_LOG_FILE", log_path.string().c_str());
#else
    setenv("REMAKE_LOG", "debug", 1);
    setenv("SKIPBUILD_LOG_FILE", log_path.string().c_str(), 1);
#endif
    
    skipbuild::log_debug("hello-from-skipbuild-test");
    
    {
        std::ifstream f(log_path);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        assert(content.find("hello-from-skipbuild-test") != std::string::npos);
    }
    std::cout << "  PASS: log line written to SKIPBUILD_LOG_FILE\n";
    
#ifdef _WIN32
    _putenv_s("REMAKE_LOG", "");
    _putenv_s("SKIPBUILD_LOG_FILE", "");
#else
    unsetenv("REMAKE_LOG");
    unsetenv("SKIPBUILD_LOG_FILE");
#endif
    std::error_code ec;
    fs::remove(log_path, ec);
}

void test_compiler_hash_changes_on_binary_change(const fs::path& self_exe) {
    std::cout << "Testing compiler hash changes on binary change...\n";
    
    // Use the test binary itself as a real executable
    // Copy it to two different names and modify one to change mtime
    fs::path test_dir = fs::temp_directory_path() / "remake-test-compiler-hash";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    fs::path compiler1 = test_dir / "fake_compiler.exe";
    fs::path compiler2 = test_dir / "fake_compiler2.exe";
    
    fs::copy_file(self_exe, compiler1, fs::copy_options::overwrite_existing);
    fs::copy_file(self_exe, compiler2, fs::copy_options::overwrite_existing);
    
    // Modify compiler2's mtime to be different
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::ofstream(compiler2, std::ios::app) << " ";
    
    // Get hashes - they should differ because of mtime
    std::string hash1 = skipbuild::get_compiler_hash(compiler1.string());
    std::string hash2 = skipbuild::get_compiler_hash(compiler2.string());
    
    assert(hash1 != hash2);
    assert(hash1.size() == 64); // SHA256 hex
    assert(hash2.size() == 64);
    
    std::cout << "  Hash1: " << hash1.substr(0, 16) << "...\n";
    std::cout << "  Hash2: " << hash2.substr(0, 16) << "...\n";
    std::cout << "  PASS: Different binaries (mtime) produce different hashes\n";
    
    // Test same file produces same hash
    std::string hash1_again = skipbuild::get_compiler_hash(compiler1.string());
    assert(hash1 == hash1_again);
    std::cout << "  PASS: Same binary produces same hash\n";
    
    // Test non-existent path uses path hash fallback
    std::string hash_missing = skipbuild::get_compiler_hash("/nonexistent/path/compiler.exe");
    std::string hash_missing_again = skipbuild::get_compiler_hash("/nonexistent/path/compiler.exe");
    assert(hash_missing == hash_missing_again);
    assert(hash_missing.size() == 64);
    std::cout << "  PASS: Non-existent path uses path hash fallback\n";
    
    fs::remove_all(test_dir);
}

int main(int argc, char* argv[]) {
    // When this binary is invoked as a fake compiler (see
    // test_compiler_hash_changes_on_binary_change), answer --version and exit
    // instead of recursively running the whole test suite.
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--version" || a == "-v") {
            std::cout << "fake-compiler 1.0.0\n";
            return 0;
        }
    }

    fs::path self_exe = fs::canonical(argv[0]);
    std::cout << "=== Cache Key Determinism & Invalidation Tests ===\n\n";
    
    test_compiler_hash_changes_on_binary_change(self_exe);
    std::cout << "\n";
    
    test_path_normalization();
    std::cout << "\n";
    
    test_cache_key_determinism();
    std::cout << "\n";
    
    test_cache_key_same_across_different_build_dirs();
    std::cout << "\n";
    
    test_environment_variable_hash();
    std::cout << "\n";
    
    test_find_build_root();
    std::cout << "\n";
    
    test_linker_key_normalization();
    std::cout << "\n";
    
    test_compiler_check_modes();
    std::cout << "\n";
    
    test_log_file_output();
    std::cout << "\n";
    
    std::cout << "=== All tests passed! ===\n";
    return 0;
}
