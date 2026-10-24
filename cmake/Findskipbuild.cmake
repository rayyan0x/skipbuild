# Findskipbuild.cmake - Find skipbuild build cache
#
# Usage:
#   list(APPEND CMAKE_MODULE_PATH "<prefix>/share/cmake/Modules")
#   find_package(skipbuild REQUIRED)
#   skipbuild_enable()

find_program(SKIPBUILD_EXECUTABLE NAMES skipbuild
    PATHS
        "${SKIPBUILD_ROOT}/bin"
        "$ENV{SKIPBUILD_ROOT}/bin"
        "/usr/local/bin"
        "/opt/homebrew/bin"
        "C:/Program Files/skipbuild/bin"
    DOC "skipbuild compiler launcher"
)

if(NOT SKIPBUILD_EXECUTABLE)
    if(SKIPBUILD_FIND_REQUIRED)
        message(FATAL_ERROR "skipbuild not found. Install it or set SKIPBUILD_ROOT.")
    else()
        message(STATUS "skipbuild not found")
        set(SKIPBUILD_FOUND FALSE)
        return()
    endif()
endif()

execute_process(
    COMMAND "${SKIPBUILD_EXECUTABLE}" --version
    OUTPUT_VARIABLE SKIPBUILD_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _skipbuild_version_result
)

set(SKIPBUILD_FOUND TRUE)
# A find module is included into the caller's scope, so plain set() is correct;
# PARENT_SCOPE would leak into the parent directory instead.
set(SKIPBUILD_EXECUTABLE "${SKIPBUILD_EXECUTABLE}")
if(_skipbuild_version_result EQUAL 0)
    set(SKIPBUILD_VERSION "${SKIPBUILD_VERSION}")
else()
    message(WARNING "Found skipbuild at ${SKIPBUILD_EXECUTABLE}, but '--version' failed")
endif()

# Include the integration module. It is installed next to this file in both
# supported locations (lib/cmake/skipbuild and share/cmake/Modules).
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/skipbuild.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/skipbuild.cmake")
else()
    message(WARNING "Findskipbuild.cmake: skipbuild.cmake not found next to this module")
endif()

message(STATUS "Found skipbuild: ${SKIPBUILD_EXECUTABLE} (${SKIPBUILD_VERSION})")
