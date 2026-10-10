# Findremake.cmake - Find remake build cache
# 
# Usage:
#   find_package(remake REQUIRED)
#   remake_enable()

find_program(REMAKE_EXECUTABLE NAMES remake
    PATHS 
        "${REMAKE_ROOT}/bin"
        "$ENV{REMAKE_ROOT}/bin"
        "/usr/local/bin"
        "/opt/homebrew/bin"
        "C:/Program Files/remake/bin"
    DOC "remake compiler launcher"
)

if(NOT REMAKE_EXECUTABLE)
    if(REMAKE_FIND_REQUIRED)
        message(FATAL_ERROR "remake not found. Install it or set REMAKE_ROOT.")
    else()
        message(STATUS "remake not found")
        set(REMAKE_FOUND FALSE)
        return()
    endif()
endif()

execute_process(
    COMMAND "${REMAKE_EXECUTABLE}" --version
    OUTPUT_VARIABLE REMAKE_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

set(REMAKE_FOUND TRUE)
set(REMAKE_VERSION "${REMAKE_VERSION}" PARENT_SCOPE)
set(REMAKE_EXECUTABLE "${REMAKE_EXECUTABLE}" PARENT_SCOPE)

# Include the integration module
include("${CMAKE_CURRENT_LIST_DIR}/remake.cmake")

message(STATUS "Found remake: ${REMAKE_EXECUTABLE} (${REMAKE_VERSION})")