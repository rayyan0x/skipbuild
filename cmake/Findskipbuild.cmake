# Findskipbuild.cmake - Find skipbuild build cache
# 
# Usage:
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
)

set(SKIPBUILD_FOUND TRUE)
set(SKIPBUILD_VERSION "${SKIPBUILD_VERSION}" PARENT_SCOPE)
set(SKIPBUILD_EXECUTABLE "${SKIPBUILD_EXECUTABLE}" PARENT_SCOPE)

# Include the integration module
include("${CMAKE_CURRENT_LIST_DIR}/skipbuild.cmake")

message(STATUS "Found skipbuild: ${SKIPBUILD_EXECUTABLE} (${SKIPBUILD_VERSION})")