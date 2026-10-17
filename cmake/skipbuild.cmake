# skipbuild CMake integration
# Usage:
#   find_package(skipbuild REQUIRED)
#   skipbuild_enable()

# Find the skipbuild executable.
# Look next to this file's install prefix first (lib/cmake/skipbuild -> bin),
# then the active install prefix, then SKIPBUILD_ROOT / PATH.
find_program(SKIPBUILD_EXECUTABLE NAMES skipbuild
    PATHS
        "${CMAKE_CURRENT_LIST_DIR}/../../../bin"
        "${CMAKE_INSTALL_PREFIX}/bin"
        "$ENV{SKIPBUILD_ROOT}/bin"
    DOC "skipbuild compiler launcher")

if(NOT SKIPBUILD_EXECUTABLE)
    message(FATAL_ERROR "skipbuild not found. Install it first or set SKIPBUILD_ROOT.")
endif()

# Get version
execute_process(
    COMMAND "${SKIPBUILD_EXECUTABLE}" --version
    OUTPUT_VARIABLE SKIPBUILD_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

message(STATUS "Found skipbuild: ${SKIPBUILD_EXECUTABLE} (${SKIPBUILD_VERSION})")

# Function to enable skipbuild for a target
function(skipbuild_enable_target TARGET)
    if(NOT TARGET ${TARGET})
        message(WARNING "skipbuild_enable_target: target '${TARGET}' not found")
        return()
    endif()
    
    # Set compiler launcher for this target
    set_target_properties(${TARGET} PROPERTIES
        CXX_COMPILER_LAUNCHER "${SKIPBUILD_EXECUTABLE}"
    )
    
    message(STATUS "skipbuild enabled for target: ${TARGET}")
endfunction()

# Function to enable skipbuild for all targets in current directory and subdirectories
function(skipbuild_enable)
    # For CMake 3.19+: use global property
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.19")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${SKIPBUILD_EXECUTABLE}" PARENT_SCOPE)
        message(STATUS "skipbuild enabled globally (CMAKE_CXX_COMPILER_LAUNCHER)")
    else()
        # For older CMake: set on all existing targets
        get_property(targets DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)
        foreach(t ${targets})
            get_property(type TARGET ${t} PROPERTY TYPE)
            if(type STREQUAL "EXECUTABLE" OR type STREQUAL "STATIC_LIBRARY" OR type STREQUAL "SHARED_LIBRARY" OR type STREQUAL "MODULE_LIBRARY")
                set_target_properties(${t} PROPERTIES CXX_COMPILER_LAUNCHER "${SKIPBUILD_EXECUTABLE}")
                message(STATUS "skipbuild enabled for existing target: ${t}")
            endif()
        endforeach()
        
        # Hook into add_executable/add_library for future targets
        macro(add_executable)
            _add_executable_with_skipbuild(${ARGV})
        endmacro()
        
        macro(add_library)
            _add_library_with_skipbuild(${ARGV})
        endmacro()
    endif()
    
    message(STATUS "skipbuild integration enabled")
endfunction()

# Helper macros for older CMake
macro(_add_executable_with_skipbuild)
    _add_executable_with_skipbuild_impl(${ARGV})
    if(CMAKE_VERSION VERSION_LESS "3.19")
        set_target_properties(${ARGV0} PROPERTIES CXX_COMPILER_LAUNCHER "${SKIPBUILD_EXECUTABLE}")
    endif()
endmacro()

macro(_add_library_with_skipbuild)
    _add_library_with_skipbuild_impl(${ARGV})
    if(CMAKE_VERSION VERSION_LESS "3.19")
        set_target_properties(${ARGV0} PROPERTIES CXX_COMPILER_LAUNCHER "${SKIPBUILD_EXECUTABLE}")
    endif()
endmacro()

# Provide a function to disable for specific targets
function(skipbuild_disable_target TARGET)
    if(TARGET ${TARGET})
        set_target_properties(${TARGET} PROPERTIES CXX_COMPILER_LAUNCHER "")
        message(STATUS "skipbuild disabled for target: ${TARGET}")
    endif()
endfunction()

# Provide cache directory configuration
function(skipbuild_set_cache_dir DIR)
    set(ENV{SKIPBUILD_CACHE_DIR} "${DIR}" PARENT_SCOPE)
    message(STATUS "skipbuild cache directory set to: ${DIR}")
endfunction()