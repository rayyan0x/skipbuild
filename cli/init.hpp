#pragma once
//
// skipbuild --init: idempotent CMake integration insertion.
//
// Header-only so it can be shared by the CLI and unit tests without pulling
// in cli/main.cpp (which owns main()).
//
#include <cctype>
#include <cstddef>
#include <string>

namespace skipbuild {

enum class InitStatus {
    Inserted,     // snippet was added
    AlreadyDone,  // the file already contains a launcher/integration
    Error         // no safe insertion point found
};

struct InitResult {
    InitStatus status = InitStatus::Error;
    std::string content;  // resulting file content when status == Inserted
    std::size_t offset = 0;
};

// The exact block that gets inserted (after project(...)).
inline std::string integration_snippet(const std::string& eol) {
    std::string s;
    s += eol;
    s += "# ---- skipbuild: cache compiles automatically ----" + eol;
    s += "find_program(SKIPBUILD_EXE skipbuild)" + eol;
    s += "if(SKIPBUILD_EXE)" + eol;
    s += "  set(CMAKE_C_COMPILER_LAUNCHER   \"${SKIPBUILD_EXE}\")" + eol;
    s += "  set(CMAKE_CXX_COMPILER_LAUNCHER \"${SKIPBUILD_EXE}\")" + eol;
    s += "endif()" + eol;
    s += "# -------------------------------------------------" + eol;
    return s;
}

// True if the file already looks integrated (so --init is a no-op).
inline bool has_integration(const std::string& content) {
    return content.find("SKIPBUILD_EXE") != std::string::npos ||
           content.find("CMAKE_C_COMPILER_LAUNCHER") != std::string::npos ||
           content.find("CMAKE_CXX_COMPILER_LAUNCHER") != std::string::npos;
}

namespace detail {

inline bool ci_equal(char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
}

// Case-insensitive search for `kw` at an identifier boundary (i.e. not part of
// a longer word such as `myproject`).
inline std::size_t find_keyword(const std::string& content, const std::string& kw, std::size_t from) {
    if (kw.size() > content.size()) return std::string::npos;
    for (std::size_t i = from; i + kw.size() <= content.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < kw.size(); ++j) {
            if (!ci_equal(content[i + j], kw[j])) { match = false; break; }
        }
        if (!match) continue;
        bool left_ok = (i == 0) ||
            !(std::isalnum(static_cast<unsigned char>(content[i - 1])) || content[i - 1] == '_');
        if (left_ok) return i;
    }
    return std::string::npos;
}

// Offset just past the end of the first `kw(...)` call (whole line consumed).
inline std::size_t end_of_call(const std::string& content, const std::string& kw) {
    std::size_t pos = find_keyword(content, kw, 0);
    while (pos != std::string::npos) {
        std::size_t p = pos + kw.size();
        while (p < content.size() && (content[p] == ' ' || content[p] == '\t')) ++p;
        if (p < content.size() && content[p] == '(') {
            int depth = 0;
            std::size_t i = p;
            bool in_str = false;
            char quote = 0;
            for (; i < content.size(); ++i) {
                char c = content[i];
                if (in_str) { if (c == quote) in_str = false; continue; }
                if (c == '"' || c == '\'') { in_str = true; quote = c; continue; }
                if (c == '#') { while (i < content.size() && content[i] != '\n') ++i; continue; }
                if (c == '(') ++depth;
                else if (c == ')') { --depth; if (depth == 0) { ++i; break; } }
            }
            if (i > content.size()) i = content.size();
            while (i < content.size() && content[i] != '\n') ++i;
            if (i < content.size()) ++i;  // consume the newline
            return i;
        }
        pos = find_keyword(content, kw, pos + kw.size());
    }
    return std::string::npos;
}

}  // namespace detail

// Where the snippet should be inserted: right after `project(...)`, falling
// back to `cmake_minimum_required(...)`. Returns npos if neither is found.
inline std::size_t find_integration_offset(const std::string& content) {
    std::size_t off = detail::end_of_call(content, "project");
    if (off != std::string::npos) return off;
    return detail::end_of_call(content, "cmake_minimum_required");
}

inline InitResult apply_integration(const std::string& content) {
    InitResult r;
    if (has_integration(content)) {
        r.status = InitStatus::AlreadyDone;
        return r;
    }
    std::size_t off = find_integration_offset(content);
    if (off == std::string::npos) {
        r.status = InitStatus::Error;
        return r;
    }
    const std::string eol = (content.find("\r\n") != std::string::npos) ? "\r\n" : "\n";
    r.content = content.substr(0, off) + integration_snippet(eol) + content.substr(off);
    r.offset = off;
    r.status = InitStatus::Inserted;
    return r;
}

}  // namespace skipbuild
