cmake_minimum_required(VERSION 3.20)
project(remake LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Workaround for GCC 16.1 ranges bug
# NOTE: VERSION_LESS 17 matches all GCC < 17, not only 16.1. Narrow or remove after verifying.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
    add_compile_options(-D_GLIBCXX_USE_CXX20_ABI=0 -std=c++20)
    add_compile_options(-D_GLIBCXX_ASSERTIONS=0)
endif()

# ZSTD: use a system copy if present, otherwise build zstd 1.5.5 from source
include(FetchContent)

find_package(ZSTD QUIET)
if(ZSTD_FOUND)
    message(STATUS "System ZSTD found - using it")
    set(REMAKE_ZSTD_TARGET ZSTD::libzstd)
else()
    message(STATUS "System ZSTD not found - fetching zstd 1.5.5")

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
endif()
set(REMAKE_HAVE_ZSTD 1)

# Optional: CURL for remote cache support (disabled on GCC 16.1 due to ranges bug)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
    message(STATUS "GCC 16.x detected - disabling remote cache (CURL) due to ranges bug")
    set(REMAKE_HAVE_CURL 0)
else()
    find_package(CURL QUIET)
    if(CURL_FOUND)
        message(STATUS "CURL found - remote cache support enabled")
        set(REMAKE_HAVE_CURL 1)
    else()
        message(STATUS "CURL not found - remote cache support disabled")
        set(REMAKE_HAVE_CURL 0)
    endif()
endif()

# Core library
add_library(remake_core STATIC cache/core.cpp cache/stats.cpp cache/config.cpp)
target_include_directories(remake_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cache)

if(REMAKE_HAVE_CURL)
    target_link_libraries(remake_core PUBLIC CURL::libcurl)
    target_compile_definitions(remake_core PUBLIC REMAKE_HAVE_CURL=1)
else()
    target_compile_definitions(remake_core PUBLIC REMAKE_HAVE_CURL=0)
endif()

target_link_libraries(remake_core PUBLIC ${REMAKE_ZSTD_TARGET})
target_compile_definitions(remake_core PUBLIC REMAKE_HAVE_ZSTD=1)

# CLI tool
add_executable(remake cli/main.cpp)
target_link_libraries(remake remake_core)

# Tests
enable_testing()

foreach(t cache concurrent multiprocess eviction simple sidecar cache_key_determinism resiliency config stats)
    add_executable(test_${t} tests/test_${t}.cpp)
    target_link_libraries(test_${t} remake_core)
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

# Install targets
install(TARGETS remake
    RUNTIME DESTINATION bin
    BUNDLE DESTINATION bin
)
install(TARGETS remake_core
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES cache/core.hpp DESTINATION include/remake)

# CMake integration
install(FILES cmake/remake.cmake DESTINATION lib/cmake/remake)

include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/remake-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/remake-config.cmake
    INSTALL_DESTINATION lib/cmake/remake
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/remake-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/remake-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/remake-config-version.cmake
    DESTINATION lib/cmake/remake
)

install(FILES cmake/Findremake.cmake DESTINATION share/cmake/Modules)