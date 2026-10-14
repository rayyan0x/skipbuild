#include "core.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

void test_response_file_basic() {
    std::cout << "Testing basic response file parsing...\n";
    std::cout.flush();
    
    fs::path test_dir = fs::temp_directory_path() / "rsp_b";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    fs::path rsp_file = test_dir / "c.rsp";
    std::ofstream(rsp_file) << "-c -o main.o main.cpp -Iinclude -O2 -Wall";
    
    std::string rsp_arg = "@" + rsp_file.string();
    const char* argv[] = {
        "cl.exe", rsp_arg.c_str(), nullptr
    };
    
    auto cmd = skipbuild::parse_compile_command(2, const_cast<char**>(argv));
    assert(cmd.has_value());
    assert(cmd->source.string().find("main.cpp") != std::string::npos);
    assert(cmd->output.string().find("main.o") != std::string::npos);
    assert(cmd->flags.size() >= 2);
    assert(cmd->includes.size() == 1);
    assert(cmd->includes[0] == "include");
    
    std::cout << "  PASS: Basic response file parsed\n";
    std::cout << "    source: " << cmd->source << "\n";
    std::cout << "    output: " << cmd->output << "\n";
    std::cout << "    flags: " << cmd->flags.size() << "\n";
    std::cout << "    includes: " << cmd->includes.size() << "\n";
    std::cout.flush();
    
    fs::remove_all(test_dir);
}

void test_response_file_quoted_args() {
    std::cout << "Testing response file with quoted arguments...\n";
    std::cout.flush();
    
    // Use shorter path to avoid Windows MAX_PATH issues on CI
    fs::path test_dir = fs::temp_directory_path() / "rsp_q";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    // Create response file with quoted paths (spaces) - simpler version
    fs::path rsp_file = test_dir / "c.rsp";
    std::ofstream(rsp_file) << "-c -o \"main.o\" \"main.cpp\" -Iinclude -O2";
    
    std::string rsp_arg = "@" + rsp_file.string();
    const char* argv[] = {
        "cl.exe", rsp_arg.c_str(), nullptr
    };
    
    auto cmd = skipbuild::parse_compile_command(2, const_cast<char**>(argv));
    assert(cmd.has_value());
    assert(cmd->source.string().find("main.cpp") != std::string::npos);
    assert(cmd->output.string().find("main.o") != std::string::npos);
    
    std::cout << "  PASS: Quoted arguments parsed correctly\n";
    std::cout << "    source: " << cmd->source << "\n";
    std::cout << "    output: " << cmd->output << "\n";
    std::cout.flush();
    
    fs::remove_all(test_dir);
}

void test_response_file_escaped_quotes() {
    std::cout << "Testing response file with escaped quotes...\n";
    std::cout.flush();
    
    fs::path test_dir = fs::temp_directory_path() / "rsp_e";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    fs::path rsp_file = test_dir / "c.rsp";
    std::ofstream(rsp_file) << "-c -o main.o main.cpp -D\"QUOTED=\\\"hello\\\"\"";
    
    std::string rsp_arg = "@" + rsp_file.string();
    const char* argv[] = {
        "cl.exe", rsp_arg.c_str(), nullptr
    };
    
    auto cmd = skipbuild::parse_compile_command(2, const_cast<char**>(argv));
    assert(cmd.has_value());
    
    bool found_escaped = false;
    for (const auto& flag : cmd->flags) {
        if (flag.find("QUOTED=\"hello\"") != std::string::npos) {
            found_escaped = true;
            break;
        }
    }
    assert(found_escaped);
    
    std::cout << "  PASS: Escaped quotes parsed correctly\n";
    std::cout.flush();
    
    fs::remove_all(test_dir);
}

void test_response_file_multiline() {
    std::cout << "Testing response file with multiple lines...\n";
    std::cout.flush();
    
    fs::path test_dir = fs::temp_directory_path() / "rsp_m";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    fs::path rsp_file = test_dir / "c.rsp";
    std::ofstream(rsp_file) << "-c\n-o main.o\nmain.cpp\n-Iinclude\n-O2\n-Wall\n";
    
    std::string rsp_arg = "@" + rsp_file.string();
    const char* argv[] = {
        "cl.exe", rsp_arg.c_str(), nullptr
    };
    
    auto cmd = skipbuild::parse_compile_command(2, const_cast<char**>(argv));
    assert(cmd.has_value());
    assert(cmd->source.string().find("main.cpp") != std::string::npos);
    assert(cmd->output.string().find("main.o") != std::string::npos);
    assert(cmd->includes.size() == 1);
    
    std::cout << "  PASS: Multi-line response file parsed\n";
    std::cout << "    source: " << cmd->source << "\n";
    std::cout << "    output: " << cmd->output << "\n";
    std::cout << "    includes: " << cmd->includes.size() << "\n";
    std::cout.flush();
    
    fs::remove_all(test_dir);
}

void test_response_file_nested() {
    std::cout << "Testing nested response files...\n";
    std::cout.flush();
    
    fs::path test_dir = fs::temp_directory_path() / "rsp_n";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    fs::path rsp1 = test_dir / "f.rsp";
    std::ofstream(rsp1) << "-O2 -Wall -Wextra";
    
    fs::path rsp2 = test_dir / "c.rsp";
    std::ofstream(rsp2) << "-c -o main.o main.cpp -Iinclude @" + rsp1.string();
    
    std::string rsp_arg = "@" + rsp2.string();
    const char* argv[] = {
        "cl.exe", rsp_arg.c_str(), nullptr
    };
    
    auto cmd = skipbuild::parse_compile_command(2, const_cast<char**>(argv));
    assert(cmd.has_value());
    assert(cmd->source.string().find("main.cpp") != std::string::npos);
    
    bool has_O2 = false, has_Wall = false, has_Wextra = false;
    for (const auto& flag : cmd->flags) {
        if (flag == "-O2") has_O2 = true;
        if (flag == "-Wall") has_Wall = true;
        if (flag == "-Wextra") has_Wextra = true;
    }
    assert(has_O2);
    assert(has_Wall);
    assert(has_Wextra);
    
    std::cout << "  PASS: Nested response files expanded\n";
    std::cout << "    flags: " << cmd->flags.size() << "\n";
    std::cout.flush();
    
    fs::remove_all(test_dir);
}

void test_response_file_msvc_style() {
    std::cout << "Testing MSVC-style response file...\n";
    std::cout.flush();
    
    fs::path test_dir = fs::temp_directory_path() / "rsp_v";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    fs::path rsp_file = test_dir / "c.rsp";
    std::ofstream(rsp_file) << "/c /Fomain.obj main.cpp /Iinclude /O2 /Zi /DWIN32";
    
    std::string rsp_arg = "@" + rsp_file.string();
    const char* argv[] = {
        "cl.exe", rsp_arg.c_str(), nullptr
    };
    
    auto cmd = skipbuild::parse_compile_command(2, const_cast<char**>(argv));
    assert(cmd.has_value());
    assert(cmd->source.string().find("main.cpp") != std::string::npos);
    assert(cmd->output.string().find("main.obj") != std::string::npos);
    assert(cmd->has_debug_info == true);
    
    bool has_win32 = false;
    for (const auto& flag : cmd->flags) {
        if (flag == "/DWIN32") has_win32 = true;
    }
    assert(has_win32);
    
    std::cout << "  PASS: MSVC-style response file parsed\n";
    std::cout << "    source: " << cmd->source << "\n";
    std::cout << "    output: " << cmd->output << "\n";
    std::cout << "    has_debug_info: " << cmd->has_debug_info << "\n";
    std::cout.flush();
    
    fs::remove_all(test_dir);
}

void test_response_file_mixed_with_direct_args() {
    std::cout << "Testing response file mixed with direct arguments...\n";
    std::cout.flush();
    
    fs::path test_dir = fs::temp_directory_path() / "rsp_x";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    // Response file + direct args
    fs::path rsp_file = test_dir / "c.rsp";
    std::ofstream(rsp_file) << "-Icommon_include -O2 -Wall";
    
    // cl.exe @common.rsp -c -o main.o main.cpp -Ispecific_include
    std::string rsp_arg = "@" + rsp_file.string();
    const char* argv[] = {
        "cl.exe", rsp_arg.c_str(), "-c", "-o", "main.o", "main.cpp", "-Ispecific_include", nullptr
    };
    
    auto cmd = skipbuild::parse_compile_command(7, const_cast<char**>(argv));
    assert(cmd.has_value());
    assert(cmd->source.string().find("main.cpp") != std::string::npos);
    assert(cmd->output.string().find("main.o") != std::string::npos);
    
    bool has_common = false, has_specific = false;
    for (const auto& inc : cmd->includes) {
        if (inc == "common_include") has_common = true;
        if (inc == "specific_include") has_specific = true;
    }
    assert(has_common);
    assert(has_specific);
    
    std::cout << "  PASS: Mixed response file + direct args parsed\n";
    std::cout << "    includes: common=" << has_common << ", specific=" << has_specific << "\n";
    std::cout.flush();
    
    fs::remove_all(test_dir);
}

void test_response_file_crlf_line_endings() {
    std::cout << "Testing CRLF line endings in response file...\n";
    std::cout.flush();
    
    fs::path test_dir = fs::temp_directory_path() / "rsp_c";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    // Create response file with CRLF line endings (Windows style)
    fs::path rsp_file = test_dir / "c.rsp";
    std::ofstream(rsp_file, std::ios::binary) << "-c\r\n-o main.o\r\nmain.cpp\r\n";
    
    std::string rsp_arg = "@" + rsp_file.string();
    const char* argv[] = {
        "cl.exe", rsp_arg.c_str(), nullptr
    };
    
    auto cmd = skipbuild::parse_compile_command(2, const_cast<char**>(argv));
    assert(cmd.has_value());
    assert(cmd->source.string().find("main.cpp") != std::string::npos);
    assert(cmd->output.string().find("main.o") != std::string::npos);
    
    std::cout << "  PASS: CRLF line endings handled\n";
    std::cout.flush();
    
    fs::remove_all(test_dir);
}

int main() {
    std::cout << "=== Response File Parsing Tests ===\n\n";
    std::cout << "Starting test_response_file_basic...\n";
    std::cout.flush();
    
    test_response_file_basic();
    std::cout << "\n";
    std::cout.flush();
    
    test_response_file_quoted_args();
    std::cout << "\n";
    std::cout.flush();
    
    test_response_file_escaped_quotes();
    std::cout << "\n";
    std::cout.flush();
    
    test_response_file_multiline();
    std::cout << "\n";
    std::cout.flush();
    
    test_response_file_nested();
    std::cout << "\n";
    std::cout.flush();
    
    test_response_file_msvc_style();
    std::cout << "\n";
    std::cout.flush();
    
    test_response_file_mixed_with_direct_args();
    std::cout << "\n";
    std::cout.flush();
    
    test_response_file_crlf_line_endings();
    std::cout << "\n";
    std::cout.flush();
    
    std::cout << "=== All response file tests passed! ===\n";
    return 0;
}