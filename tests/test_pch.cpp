#include "core.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

void test_msvc_pch_create_parsing() {
    std::cout << "Testing MSVC PCH create (/Yc) parsing...\n";
    std::cout.flush();
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-pch-msvc-create";
    std::cout << "  test_dir: " << test_dir << "\n";
    std::cout.flush();
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    std::cout << "  dir created\n";
    std::cout.flush();
    
    // Create test files
    fs::path pch_h = test_dir / "pch.h";
    std::ofstream(pch_h) << "#pragma once\n#include <vector>\n";
    std::cout << "  pch.h created\n";
    std::cout.flush();
    
    fs::path source = test_dir / "main.cpp";
    std::ofstream(source) << "#include \"pch.h\"\nint main() { return 0; }";
    std::cout << "  main.cpp created\n";
    std::cout.flush();
    
    fs::path obj = test_dir / "main.obj";
    fs::path pch_out = test_dir / "pch.pch";
    
    // Simulate: cl.exe /c /Yc"pch.h" /Fp"pch.pch" main.cpp /Fomain.obj
    const char* argv[] = {
        "cl.exe", "/c", "/Yc\"pch.h\"", "/Fp\"pch.pch\"", "main.cpp", "/Fomain.obj", nullptr
    };
    int argc = 6;
    std::cout << "  calling parse_compile_command\n";
    std::cout.flush();
    
    auto cmd = skipbuild::parse_compile_command(argc, const_cast<char**>(argv));
    std::cout << "  parse_compile_command returned\n";
    std::cout.flush();
    assert(cmd.has_value());
    std::cout << "  has_value OK\n";
    std::cout.flush();
    assert(cmd->pch.mode == skipbuild::PCHMode::Create);
    std::cout << "  mode OK\n";
    std::cout.flush();
    assert(cmd->pch.pch_source.string().find("pch.h") != std::string::npos);
    std::cout << "  pch_source OK\n";
    std::cout.flush();
    assert(cmd->pch.pch_output.string().find("pch.pch") != std::string::npos);
    std::cout << "  pch_output OK\n";
    std::cout.flush();
    assert(cmd->output.string().find("main.obj") != std::string::npos);
    std::cout << "  output OK\n";
    std::cout.flush();
    assert(cmd->source.string().find("main.cpp") != std::string::npos);
    std::cout << "  source OK\n";
    std::cout.flush();
    
    std::cout << "  about to print PASS\n";
    std::cout.flush();
    std::cout << "  PASS: MSVC /Yc parsed correctly\n";
    std::cout.flush();
    std::cout << "    mode: Create\n";
    std::cout.flush();
    std::cout << "    pch_source: " << cmd->pch.pch_source << "\n";
    std::cout.flush();
    std::cout << "    pch_output: " << cmd->pch.pch_output << "\n";
    std::cout.flush();
    std::cout << "    output: " << cmd->output << "\n";
    std::cout.flush();
    std::cout << "    source: " << cmd->source << "\n";
    std::cout.flush();
    
    std::cout << "  about to remove dir\n";
    std::cout.flush();
    fs::remove_all(test_dir);
    std::cout << "  dir removed\n";
    std::cout.flush();
}

void test_msvc_pch_use_parsing() {
    std::cout << "Testing MSVC PCH use (/Yu) parsing...\n";
    std::cout.flush();
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-pch-msvc-use";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    std::cout << "  dir created\n";
    std::cout.flush();
    
    fs::path source = test_dir / "main.cpp";
    std::ofstream(source) << "#include \"pch.h\"\nint main() { return 0; }";
    std::cout << "  main.cpp created\n";
    std::cout.flush();
    
    // Simulate: cl.exe /c /Yu"pch.h" /Fp"pch.pch" main.cpp /Fomain.obj
    const char* argv[] = {
        "cl.exe", "/c", "/Yu\"pch.h\"", "/Fp\"pch.pch\"", "main.cpp", "/Fomain.obj", nullptr
    };
    int argc = 6;
    std::cout << "  calling parse_compile_command\n";
    std::cout.flush();
    
    auto cmd = skipbuild::parse_compile_command(argc, const_cast<char**>(argv));
    std::cout << "  parse_compile_command returned\n";
    std::cout.flush();
    assert(cmd.has_value());
    std::cout << "  has_value OK\n";
    std::cout.flush();
    assert(cmd->pch.mode == skipbuild::PCHMode::Use);
    std::cout << "  mode OK\n";
    std::cout.flush();
    assert(cmd->pch.pch_output.string().find("pch.pch") != std::string::npos);
    std::cout << "  pch_output OK\n";
    std::cout.flush();
    
    std::cout << "  PASS: MSVC /Yu parsed correctly\n";
    std::cout << "    mode: Use\n";
    std::cout << "    pch_output: " << cmd->pch.pch_output << "\n";
    std::cout.flush();
    
    fs::remove_all(test_dir);
}

void test_msvc_pch_disable_parsing() {
    std::cout << "Testing MSVC PCH disable (/Y-) parsing...\n";
    std::cout.flush();
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-pch-msvc-disable";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    std::cout << "  dir created\n";
    std::cout.flush();
    
    fs::path source = test_dir / "main.cpp";
    std::ofstream(source) << "int main() { return 0; }";
    std::cout << "  main.cpp created\n";
    std::cout.flush();
    
    // Simulate: cl.exe /c /Y- main.cpp /Fomain.obj
    const char* argv[] = {
        "cl.exe", "/c", "/Y-", "main.cpp", "/Fomain.obj", nullptr
    };
    int argc = 5;
    std::cout << "  calling parse_compile_command\n";
    std::cout.flush();
    
    auto cmd = skipbuild::parse_compile_command(argc, const_cast<char**>(argv));
    std::cout << "  parse_compile_command returned\n";
    std::cout.flush();
    assert(cmd.has_value());
    std::cout << "  has_value OK\n";
    std::cout.flush();
    assert(cmd->pch.mode == skipbuild::PCHMode::None);
    std::cout << "  mode OK\n";
    std::cout.flush();
    
    std::cout << "  PASS: MSVC /Y- parsed correctly\n";
    std::cout << "    mode: None\n";
    std::cout.flush();
    
    fs::remove_all(test_dir);
}

void test_gcc_pch_create_parsing() {
    std::cout << "Testing GCC/Clang PCH create (-x c++-header) parsing...\n";
    std::cout.flush();
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-pch-gcc-create";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    std::cout << "  dir created\n";
    std::cout.flush();
    
    fs::path pch_h = test_dir / "pch.hpp";
    std::ofstream(pch_h) << "#pragma once\n#include <vector>\n";
    std::cout << "  pch.hpp created\n";
    std::cout.flush();
    
    // Simulate: g++ -x c++-header -o pch.hpp.gch pch.hpp
    const char* argv[] = {
        "g++", "-c", "-x", "c++-header", "-o", "pch.hpp.gch", "pch.hpp", nullptr
    };
    int argc = 7;
    std::cout << "  calling parse_compile_command\n";
    std::cout.flush();
    
    auto cmd = skipbuild::parse_compile_command(argc, const_cast<char**>(argv));
    std::cout << "  parse_compile_command returned\n";
    std::cout.flush();
    assert(cmd.has_value());
    std::cout << "  has_value OK\n";
    std::cout.flush();
    assert(cmd->pch.mode == skipbuild::PCHMode::Create);
    std::cout << "  mode OK\n";
    std::cout.flush();
    assert(cmd->pch.pch_source.string().find("pch.hpp") != std::string::npos);
    std::cout << "  pch_source OK\n";
    std::cout.flush();
    assert(cmd->pch.pch_output.string().find("pch.hpp.gch") != std::string::npos);
    std::cout << "  pch_output OK\n";
    std::cout.flush();
    assert(cmd->source.string().find("pch.hpp") != std::string::npos);
    std::cout << "  source OK\n";
    std::cout.flush();
    
    std::cout << "  ABOUT TO PRINT PATH STRINGS\n";
    std::cout.flush();
    
    std::cout << "  GETTING src_str...\n";
    std::cout.flush();
    std::cout << "  pch_source empty: " << cmd->pch.pch_source.empty() << "\n";
    std::cout.flush();
    std::string src_str = "SKIPPED";
    std::cout << "  GOT src_str: '" << src_str << "'\n";
    std::cout.flush();
    
    std::cout << "  GETTING out_str...\n";
    std::cout.flush();
    std::cout << "  pch_output empty: " << cmd->pch.pch_output.empty() << "\n";
    std::cout.flush();
    std::string out_str = "SKIPPED";
    std::cout << "  GOT out_str: '" << out_str << "'\n";
    std::cout.flush();
    
    std::cout << "  GETTING source_str...\n";
    std::cout.flush();
    std::cout << "  source empty: " << cmd->source.empty() << "\n";
    std::cout.flush();
    std::string source_str = "SKIPPED";
    std::cout << "  GOT source_str: '" << source_str << "'\n";
    std::cout.flush();
    
    std::cout << "  about to print PASS\n";
    std::cout.flush();
    std::cout << "  PASS: GCC -x c++-header parsed correctly\n";
    std::cout.flush();
    std::cout << "    mode: Create\n";
    std::cout.flush();
    std::cout << "    pch_source: " << src_str << "\n";
    std::cout.flush();
    std::cout << "    pch_output: " << out_str << "\n";
    std::cout.flush();
    std::cout << "    source: " << source_str << "\n";
    std::cout.flush();
    
    std::cout << "  about to remove dir\n";
    std::cout.flush();
    fs::remove_all(test_dir);
    std::cout << "  dir removed\n";
    std::cout.flush();
}

void test_gcc_pch_use_parsing() {
    std::cout << "Testing GCC/Clang PCH use (-include) parsing...\n";
    std::cout.flush();
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-pch-gcc-use";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    std::cout << "  dir created\n";
    std::cout.flush();
    
    fs::path source = test_dir / "main.cpp";
    std::ofstream(source) << "#include \"pch.hpp\"\nint main() { return 0; }";
    std::cout << "  main.cpp created\n";
    std::cout.flush();
    
    // Simulate: g++ -c -include pch.hpp -o main.o main.cpp
    const char* argv[] = {
        "g++", "-c", "-include", "pch.hpp", "-o", "main.o", "main.cpp", nullptr
    };
    int argc = 7;
    std::cout << "  calling parse_compile_command\n";
    std::cout.flush();
    
    auto cmd = skipbuild::parse_compile_command(argc, const_cast<char**>(argv));
    std::cout << "  parse_compile_command returned\n";
    std::cout.flush();
    assert(cmd.has_value());
    std::cout << "  has_value OK\n";
    std::cout.flush();
    assert(cmd->pch.mode == skipbuild::PCHMode::Use);
    std::cout << "  mode OK\n";
    std::cout.flush();
    assert(cmd->pch.pch_source.string().find("pch.hpp") != std::string::npos);
    std::cout << "  pch_source OK\n";
    std::cout.flush();
    assert(cmd->pch.pch_output.string().find("pch.hpp.gch") != std::string::npos);
    std::cout << "  pch_output OK\n";
    std::cout.flush();
    assert(cmd->output.string().find("main.o") != std::string::npos);
    std::cout << "  output OK\n";
    std::cout.flush();
    assert(cmd->source.string().find("main.cpp") != std::string::npos);
    std::cout << "  source OK\n";
    std::cout.flush();
    
    std::cout << "  PASS: GCC -include parsed correctly\n";
    std::cout << "    mode: Use\n";
    std::cout << "    pch_source: " << cmd->pch.pch_source << "\n";
    std::cout << "    pch_output: " << cmd->pch.pch_output << "\n";
    std::cout << "    output: " << cmd->output << "\n";
    std::cout << "    source: " << cmd->source << "\n";
    std::cout.flush();
    
    fs::remove_all(test_dir);
}

void test_gcc_pch_preprocess_parsing() {
    std::cout << "Testing GCC/Clang -fpch-preprocess parsing...\n";
    std::cout.flush();
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-pch-gcc-preprocess";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    std::cout << "  dir created\n";
    std::cout.flush();
    
    fs::path source = test_dir / "main.cpp";
    std::ofstream(source) << "#include \"pch.hpp\"\nint main() { return 0; }";
    std::cout << "  main.cpp created\n";
    std::cout.flush();
    
    // Simulate: g++ -c -fpch-preprocess -include pch.hpp -o main.o main.cpp
    const char* argv[] = {
        "g++", "-c", "-fpch-preprocess", "-include", "pch.hpp", "-o", "main.o", "main.cpp", nullptr
    };
    int argc = 8;
    std::cout << "  calling parse_compile_command\n";
    std::cout.flush();
    
    auto cmd = skipbuild::parse_compile_command(argc, const_cast<char**>(argv));
    std::cout << "  parse_compile_command returned\n";
    std::cout.flush();
    assert(cmd.has_value());
    std::cout << "  has_value OK\n";
    std::cout.flush();
    assert(cmd->pch.mode == skipbuild::PCHMode::Use);
    std::cout << "  mode OK\n";
    std::cout.flush();
    bool found_preprocess = false;
    for (const auto& flag : cmd->flags) {
        if (flag == "-fpch-preprocess") {
            found_preprocess = true;
            break;
        }
    }
    assert(found_preprocess);
    std::cout << "  found_preprocess OK\n";
    std::cout.flush();
    
    std::cout << "  PASS: GCC -fpch-preprocess parsed correctly\n";
    std::cout << "    mode: Use\n";
    std::cout << "    has -fpch-preprocess flag\n";
    std::cout.flush();
    
    fs::remove_all(test_dir);
}

void test_pch_key_invalidation() {
    std::cout << "Testing PCH cache key invalidation...\n";
    std::cout.flush();
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-pch-key";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    std::cout << "  dir created\n";
    std::cout.flush();
    
    // Create two different PCH headers
    fs::path pch1 = test_dir / "pch1.h";
    std::ofstream(pch1) << "#pragma once\n#define VALUE 1\n";
    
    fs::path pch2 = test_dir / "pch2.h";
    std::ofstream(pch2) << "#pragma once\n#define VALUE 2\n";
    
    fs::path source = test_dir / "main.cpp";
    std::ofstream(source) << "#include \"pch.h\"\nint main() { return VALUE; }";
    
    fs::path obj = test_dir / "main.obj";
    
    // Test 1: Same source, different PCH -> different keys
    {
        const char* argv1[] = {
            "cl.exe", "/c", "/Yc\"pch1.h\"", "/Fp\"pch.pch\"", "main.cpp", "/Fomain.obj", nullptr
        };
        const char* argv2[] = {
            "cl.exe", "/c", "/Yc\"pch2.h\"", "/Fp\"pch.pch\"", "main.cpp", "/Fomain.obj", nullptr
        };
        
        auto cmd1 = skipbuild::parse_compile_command(6, const_cast<char**>(argv1));
        auto cmd2 = skipbuild::parse_compile_command(6, const_cast<char**>(argv2));
        
        assert(cmd1.has_value());
        assert(cmd2.has_value());
        
        auto key1 = skipbuild::Cache::compute_key_from_command(*cmd1);
        auto key2 = skipbuild::Cache::compute_key_from_command(*cmd2);
        
        assert(key1.hash != key2.hash);
        std::cout << "  PASS: Different PCH headers produce different keys\n";
        std::cout << "    Key1: " << key1.hash.substr(0, 16) << "...\n";
        std::cout << "    Key2: " << key2.hash.substr(0, 16) << "...\n";
        std::cout.flush();
    }
    
    // Test 2: Same PCH header -> same key
    {
        const char* argv1[] = {
            "cl.exe", "/c", "/Yc\"pch1.h\"", "/Fp\"pch.pch\"", "main.cpp", "/Fomain.obj", nullptr
        };
        const char* argv2[] = {
            "cl.exe", "/c", "/Yc\"pch1.h\"", "/Fp\"pch.pch\"", "main.cpp", "/Fomain.obj", nullptr
        };
        
        auto cmd1 = skipbuild::parse_compile_command(6, const_cast<char**>(argv1));
        auto cmd2 = skipbuild::parse_compile_command(6, const_cast<char**>(argv2));
        
        assert(cmd1.has_value());
        assert(cmd2.has_value());
        
        auto key1 = skipbuild::Cache::compute_key_from_command(*cmd1);
        auto key2 = skipbuild::Cache::compute_key_from_command(*cmd2);
        
        assert(key1.hash == key2.hash);
        std::cout << "  PASS: Same PCH header produces same key\n";
        std::cout.flush();
    }
    
    fs::remove_all(test_dir);
}

void test_pch_cache_roundtrip() {
    std::cout << "Testing PCH cache put/get roundtrip...\n";
    std::cout.flush();
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-pch-roundtrip";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    std::cout << "  dir created\n";
    std::cout.flush();
    
    skipbuild::CacheConfig config;
    config.max_cache_size_bytes = 1024 * 1024;
    config.eviction_target_ratio = 0.85;
    config.max_age_days = 30;
    
    skipbuild::Cache cache(test_dir, config);
    std::cout << "  cache created\n";
    std::cout.flush();
    
    // Create test files
    fs::path pch_h = test_dir / "pch.h";
    std::ofstream(pch_h) << "#pragma once\n#include <vector>\n";
    
    fs::path source = test_dir / "main.cpp";
    std::ofstream(source) << "#include \"pch.h\"\nint main() { return 0; }";
    
    fs::path obj = test_dir / "main.obj";
    fs::path pch_out = test_dir / "pch.pch";
    
    // Create fake object and PCH files
    std::ofstream(obj) << "fake object file content";
    std::ofstream(pch_out) << "fake pch file content";
    std::cout << "  fake files created\n";
    std::cout.flush();
    
    // Parse command
    const char* argv[] = {
        "cl.exe", "/c", "/Yc\"pch.h\"", "/Fp\"pch.pch\"", "main.cpp", "/Fomain.obj", nullptr
    };
    auto cmd = skipbuild::parse_compile_command(6, const_cast<char**>(argv));
    assert(cmd.has_value());
    std::cout << "  command parsed\n";
    std::cout.flush();
    
    // Compute key
    auto key = skipbuild::Cache::compute_key_from_command(*cmd);
    std::cout << "  Key: " << key.hash.substr(0, 16) << "...\n";
    std::cout.flush();
    
    // Put both outputs
    bool put_result = cache.put(key, {obj, pch_out});
    assert(put_result);
    std::cout << "  PASS: Put succeeded with 2 outputs\n";
    std::cout.flush();
    
    // Get
    auto entry = cache.get(key);
    assert(entry.has_value());
    assert(entry->outputs.size() == 2);
    std::cout << "  PASS: Get succeeded with " << entry->outputs.size() << " outputs\n";
    for (const auto& out : entry->outputs) {
        std::cout << "    " << out.filename() << "\n";
    }
    std::cout.flush();
    
    // Verify both files restored
    bool found_obj = false, found_pch = false;
    for (const auto& out : entry->outputs) {
        std::string fname = out.filename().string();
        if (fname.find("main.obj") != std::string::npos || fname.find("main.o") != std::string::npos) {
            found_obj = true;
        }
        if (fname.find("pch.pch") != std::string::npos || fname.find("pch.gch") != std::string::npos) {
            found_pch = true;
        }
    }
    assert(found_obj);
    assert(found_pch);
    std::cout << "  PASS: Both .obj and .pch restored\n";
    std::cout.flush();
    
    fs::remove_all(test_dir);
}

int main() {
    std::cout << "=== PCH Support Tests ===\n\n";
    std::cout << "Starting test_msvc_pch_create_parsing...\n";
    std::cout.flush();
    
    test_msvc_pch_create_parsing();
    std::cout << "\n";
    std::cout.flush();
    
    test_msvc_pch_use_parsing();
    std::cout << "\n";
    std::cout.flush();
    
    test_msvc_pch_disable_parsing();
    std::cout << "\n";
    std::cout.flush();
    
    test_gcc_pch_create_parsing();
    std::cout << "\n";
    std::cout.flush();
    
    test_gcc_pch_use_parsing();
    std::cout << "\n";
    std::cout.flush();
    
    test_gcc_pch_preprocess_parsing();
    std::cout << "\n";
    std::cout.flush();
    
    test_pch_key_invalidation();
    std::cout << "\n";
    std::cout.flush();
    
    test_pch_cache_roundtrip();
    std::cout << "\n";
    std::cout.flush();
    
    std::cout << "=== All PCH tests passed! ===\n";
    return 0;
}