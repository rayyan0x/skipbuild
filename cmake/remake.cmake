# remake CMake integration
# Usage:
#   find_package(remake REQUIRED)
#   remake_enable()

# Find the remake executable
find_program(REMAKE_EXECUTABLE NAMES remake PATHS "${CMAKE_INSTALL_PREFIX}/bin" "$ENV{REMAKE_ROOT}/bin" DOC "remake compiler launcher")

if(NOT REMAKE_EXECUTABLE)
    message(FATAL_ERROR "remake not found. Install it first or set REMAKE_ROOT.")
endif()

# Get version
execute_process(
    COMMAND "${REMAKE_EXECUTABLE}" --version
    OUTPUT_VARIABLE REMAKE_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

message(STATUS "Found remake: ${REMAKE_EXECUTABLE} (${REMAKE_VERSION})")

# Function to enable remake for a target
function(remake_enable_target TARGET)
    if(NOT TARGET ${TARGET})
        message(WARNING "remake_enable_target: target '${TARGET}' not found")
        return()
    endif()
    
    # Set compiler launcher for this target
    set_target_properties(${TARGET} PROPERTIES
        CXX_COMPILER_LAUNCHER "${REMAKE_EXECUTABLE}"
    )
    
    message(STATUS "remake enabled for target: ${TARGET}")
endfunction()

# Function to enable remake for all targets in current directory and subdirectories
function(remake_enable)
    # For CMake 3.19+: use global property
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.19")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${REMAKE_EXECUTABLE}" PARENT_SCOPE)
        message(STATUS "remake enabled globally (CMAKE_CXX_COMPILER_LAUNCHER)")
    else()
        # For older CMake: set on all existing targets
        get_property(targets DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)
        foreach(t ${targets})
            get_property(type TARGET ${t} PROPERTY TYPE)
            if(type STREQUAL "EXECUTABLE" OR type STREQUAL "STATIC_LIBRARY" OR type STREQUAL "SHARED_LIBRARY" OR type STREQUAL "MODULE_LIBRARY")
                set_target_properties(${t} PROPERTIES CXX_COMPILER_LAUNCHER "${REMAKE_EXECUTABLE}")
                message(STATUS "remake enabled for existing target: ${t}")
            endif()
        endforeach()
        
        # Hook into add_executable/add_library for future targets
        macro(add_executable)
            _add_executable_with_remake(${ARGV})
        endmacro()
        
        macro(add_library)
            _add_library_with_remake(${ARGV})
        endmacro()
    endif()
    
    message(STATUS "remake integration enabled")
endfunction()

# Helper macros for older CMake
macro(_add_executable_with_remake)
    _add_executable_with_remake_impl(${ARGV})
    if(CMAKE_VERSION VERSION_LESS "3.19")
        set_target_properties(${ARGV0} PROPERTIES CXX_COMPILER_LAUNCHER "${REMAKE_EXECUTABLE}")
    endif()
endmacro()

macro(_add_library_with_remake)
    _add_library_with_remake_impl(${ARGV})
    if(CMAKE_VERSION VERSION_LESS "3.19")
        set_target_properties(${ARGV0} PROPERTIES CXX_COMPILER_LAUNCHER "${REMAKE_EXECUTABLE}")
    endif()
endmacro()

# Provide a function to disable for specific targets
function(remake_disable_target TARGET)
    if(TARGET ${TARGET})
        set_target_properties(${TARGET} PROPERTIES CXX_COMPILER_LAUNCHER "")
        message(STATUS "remake disabled for target: ${TARGET}")
    endif()
endfunction()

# Provide cache directory configuration
function(remake_set_cache_dir DIR)
    set(ENV{REMAKE_CACHE_DIR} "${DIR}" PARENT_SCOPE)
    message(STATUS "remake cache directory set to: ${DIR}")
endfunction()