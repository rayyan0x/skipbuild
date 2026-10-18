cmake_minimum_required(VERSION 3.20)
project(skipbuild LANGUAGES CXX VERSION 0.3.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

cmake_policy(SET CMP0135 NEW)
set(CMAKE_POLICY_VERSION_MINIMUM "3.5" CACHE STRING "" FORCE)

# ZSTD: always fetch and build zstd 1.5.5 from source (avoids target name differences across platforms)
include(FetchContent)

message(STATUS "Fetching zstd 1.5.5")

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC   ON  CACHE BOOL "" FORCE)

FetchContent_Declare(
    zstd
    URL https://github.com/facebook/zstd/releases/download/v1.5.5/zstd-1.5.5.tar.gz
    URL_HASH SHA256=9c4396cc829cfae319a6e2615202e82aad41372073482fce286fac78646d3ee4
    SOURCE_SUBDIR build/cmake
)
FetchContent_MakeAvailable(zstd)

set(REMAKE_ZSTD_TARGET libzstd_static)
set(ZSTD_INCLUDE_DIRS ${zstd_SOURCE_DIR}/lib ${zstd_SOURCE_DIR}/lib/common)
set(REMAKE_HAVE_ZSTD 1)

# Core library
add_library(remake_core STATIC cache/core.cpp cache/stats.cpp cache/config.cpp)
target_include_directories(remake_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cache)

if(ZSTD_INCLUDE_DIRS)
    target_include_directories(remake_core PUBLIC ${ZSTD_INCLUDE_DIRS})
endif()

target_link_libraries(remake_core PUBLIC ${REMAKE_ZSTD_TARGET})
target_compile_definitions(remake_core PUBLIC
    REMAKE_HAVE_ZSTD=1
    SKIPBUILD_VERSION="${PROJECT_VERSION}"
    SKIPBUILD_WVERSION=L"${PROJECT_VERSION}"
)

# CLI tool
add_executable(skipbuild cli/main.cpp)
target_link_libraries(skipbuild remake_core)

# Tests
enable_testing()

# Optional strict warnings / sanitizers.
# - Warnings-as-errors applies to the shipped targets only.
# - Sanitizer flags must reach every executable that links the instrumented
#   core library (including tests), so both compile and link options are set on
#   each target; a static library is not linked, so link options must be on the
#   executables.
option(SKIPBUILD_WERROR "Treat compiler warnings as errors (/W4 /WX or -Wall -Wextra -Werror)" OFF)
option(SKIPBUILD_SANITIZE "Enable AddressSanitizer + UBSan (GCC/Clang only)" OFF)

function(skipbuild_werror target)
    if(SKIPBUILD_WERROR)
        if(MSVC)
            target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
            target_compile_options(${target} PRIVATE /W4 /WX /permissive- /external:anglebrackets /external:W0)
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
        endif()
    endif()
endfunction()

function(skipbuild_sanitize target)
    if(SKIPBUILD_SANITIZE AND NOT MSVC)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

foreach(t cache concurrent multiprocess eviction simple sidecar cache_key_determinism resiliency config stats pch response_file key_soundness)
    add_executable(test_${t} tests/test_${t}.cpp)
    target_link_libraries(test_${t} remake_core)
    skipbuild_sanitize(test_${t})
endforeach()

add_executable(test_init tests/test_init.cpp)
target_link_libraries(test_init remake_core)
target_include_directories(test_init PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cli)
skipbuild_sanitize(test_init)

foreach(tgt remake_core skipbuild)
    skipbuild_werror(${tgt})
    skipbuild_sanitize(${tgt})
endforeach()

add_test(NAME cache_test COMMAND test_cache)
add_test(NAME concurrent_test COMMAND test_concurrent)
add_test(NAME multiprocess_test COMMAND test_multiprocess 0)
add_test(NAME eviction_test COMMAND test_eviction)
add_test(NAME simple_test COMMAND test_simple)
add_test(NAME sidecar_test COMMAND test_sidecar)
add_test(NAME cache_key_determinism_test COMMAND test_cache_key_determinism)
add_test(NAME resiliency_test COMMAND test_resiliency)
add_test(NAME config_test COMMAND test_config)
add_test(NAME stats_test COMMAND test_stats)
add_test(NAME pch_test COMMAND test_pch)
add_test(NAME response_file_test COMMAND test_response_file)
add_test(NAME init_test COMMAND test_init)
add_test(NAME key_soundness_test COMMAND test_key_soundness)

# Install targets
install(TARGETS skipbuild
    RUNTIME DESTINATION bin
    BUNDLE DESTINATION bin
)
install(TARGETS remake_core
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES cache/core.hpp DESTINATION include/skipbuild)

# CMake integration
install(FILES cmake/skipbuild.cmake DESTINATION lib/cmake/skipbuild)

include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/skipbuild-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/skipbuild-config.cmake
    INSTALL_DESTINATION lib/cmake/skipbuild
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/skipbuild-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/skipbuild-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/skipbuild-config-version.cmake
    DESTINATION lib/cmake/skipbuild
)

install(FILES cmake/Findskipbuild.cmake DESTINATION share/cmake/Modules)