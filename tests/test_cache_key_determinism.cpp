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
    
    // Test different order of inputs produces same key (due to sorting)
    std::swap(cmd.inputs[0], cmd.inputs[1]);
    auto key3 = skipbuild::compute_linker_key(cmd);
    assert(key1.hash == key3.hash);
    std::cout << "  PASS: Linker key order independent\n";
    
    fs::remove_all(test_dir);
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
    
    std::cout << "=== All tests passed! ===\n";
    return 0;
}
