cmake_minimum_required(VERSION 3.20)
project(remake LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer MSVC on Windows
if(WIN32)
    set(CMAKE_CXX_COMPILER cl CACHE STRING "C++ compiler" FORCE)
endif()

# Workaround for GCC 16.1 ranges bug
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
    add_compile_options(-D_GLIBCXX_USE_CXX20_ABI=0 -std=c++20)
    add_compile_options(-D_GLIBCXX_ASSERTIONS=0)
endif()

# Optional: ZSTD for artifact compression
find_package(ZSTD QUIET)
if(ZSTD_FOUND)
    message(STATUS "ZSTD found - artifact compression enabled")
    set(REMAKE_HAVE_ZSTD 1)
else()
    message(STATUS "ZSTD not found - artifact compression disabled")
    set(REMAKE_HAVE_ZSTD 0)
endif()

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

if(REMAKE_HAVE_ZSTD)
    target_link_libraries(remake_core PUBLIC ZSTD::libzstd)
    target_compile_definitions(remake_core PUBLIC REMAKE_HAVE_ZSTD=1)
else()
    target_compile_definitions(remake_core PUBLIC REMAKE_HAVE_ZSTD=0)
endif()

# CLI tool
add_executable(remake cli/main.cpp)
target_link_libraries(remake remake_core)

# Tests
enable_testing()
add_executable(test_cache tests/test_cache.cpp)
target_link_libraries(test_cache remake_core)
add_test(NAME cache_test COMMAND test_cache)

add_executable(test_concurrent tests/test_concurrent.cpp)
target_link_libraries(test_concurrent remake_core)
add_test(NAME concurrent_test COMMAND test_concurrent)

add_executable(test_multiprocess tests/test_multiprocess.cpp)
target_link_libraries(test_multiprocess remake_core)
add_test(NAME multiprocess_test COMMAND test_multiprocess 0)

add_executable(test_eviction tests/test_eviction.cpp)
target_link_libraries(test_eviction remake_core)
add_test(NAME eviction_test COMMAND test_eviction)

add_executable(test_simple tests/test_simple.cpp)
target_link_libraries(test_simple remake_core)
add_test(NAME simple_test COMMAND test_simple)

add_executable(test_sidecar tests/test_sidecar.cpp)
target_link_libraries(test_sidecar remake_core)
add_test(NAME sidecar_test COMMAND test_sidecar)

add_executable(test_cache_key_determinism tests/test_cache_key_determinism.cpp)
target_link_libraries(test_cache_key_determinism remake_core)
add_test(NAME cache_key_determinism_test COMMAND test_cache_key_determinism)

add_executable(test_resiliency tests/test_resiliency.cpp)
target_link_libraries(test_resiliency remake_core)
add_test(NAME resiliency_test COMMAND test_resiliency)

add_executable(test_config tests/test_config.cpp)
target_link_libraries(test_config remake_core)
add_test(NAME config_test COMMAND test_config)

add_executable(test_stats tests/test_stats.cpp)
target_link_libraries(test_stats remake_core)
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

# Install CMake integration module
install(FILES cmake/remake.cmake DESTINATION lib/cmake/remake)

# Generate and install config file for find_package()
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

# Also install a simple find module for older CMake
install(FILES cmake/Findremake.cmake DESTINATION share/cmake/Modules)