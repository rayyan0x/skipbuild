#include "init.hpp"

#include <cassert>
#include <iostream>
#include <string>

using namespace skipbuild;

int main() {
    // Inserts after project()
    {
        std::string in =
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(foo CXX)\n"
            "add_executable(a m.cpp)\n";
        auto r = apply_integration(in);
        assert(r.status == InitStatus::Inserted);
        auto snip = r.content.find("SKIPBUILD_EXE");
        assert(snip != std::string::npos);
        assert(snip > r.content.find("project(foo CXX)"));
        assert(snip < r.content.find("add_executable"));
        assert(r.content.find("find_program(SKIPBUILD_EXE skipbuild)") != std::string::npos);
    }

    // Idempotent: second run is a no-op
    {
        auto r1 = apply_integration("project(foo)\n");
        assert(r1.status == InitStatus::Inserted);
        auto r2 = apply_integration(r1.content);
        assert(r2.status == InitStatus::AlreadyDone);
    }

    // CRLF is preserved
    {
        std::string in = "project(foo)\r\nadd_executable(a m.cpp)\r\n";
        auto r = apply_integration(in);
        assert(r.status == InitStatus::Inserted);
        auto p = r.content.find("# ---- skipbuild");
        assert(p != std::string::npos);
        auto nl = r.content.find('\n', p);
        assert(nl != std::string::npos);
        assert(r.content[nl - 1] == '\r');
    }

    // Multi-line project() call: snippet goes after the closing paren
    {
        std::string in =
            "project(\n  foo\n  VERSION 1.0\n)\n"
            "add_executable(a m.cpp)\n";
        auto r = apply_integration(in);
        assert(r.status == InitStatus::Inserted);
        auto snip = r.content.find("SKIPBUILD_EXE");
        assert(snip > r.content.find("VERSION 1.0"));
        assert(snip < r.content.find("add_executable"));
    }

    // No project() -> falls back to cmake_minimum_required()
    {
        std::string in =
            "cmake_minimum_required(VERSION 3.20)\n"
            "add_executable(a m.cpp)\n";
        auto r = apply_integration(in);
        assert(r.status == InitStatus::Inserted);
        assert(r.content.find("SKIPBUILD_EXE") > r.content.find("cmake_minimum_required"));
    }

    // Word-boundary: a stray "myproject(" must not be mistaken for project()
    {
        std::string in =
            "cmake_minimum_required(VERSION 3.20)\n"
            "add_library(myproject STATIC x.cpp)\n";
        auto r = apply_integration(in);
        assert(r.status == InitStatus::Inserted);
        assert(r.content.find("SKIPBUILD_EXE") > r.content.find("cmake_minimum_required"));
        assert(r.content.find("SKIPBUILD_EXE") < r.content.find("add_library"));
    }

    // Nothing to hook onto -> Error
    {
        auto r = apply_integration("# just a comment\n");
        assert(r.status == InitStatus::Error);
    }

    std::cout << "test_init passed\n";
    return 0;
}
