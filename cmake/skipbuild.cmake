# skipbuild CMake integration
# Usage:
#   find_package(skipbuild REQUIRED)
#   skipbuild_enable()

# Find the skipbuild executable.
# Look next to this file's install prefix first (lib/cmake/skipbuild -> bin),
# then the active install prefix, then SKIPBUILD_ROOT / PATH.
# Prefer the imported launcher target exported by skipbuild-config.cmake.
if(NOT SKIPBUILD_EXECUTABLE AND TARGET skipbuild::skipbuild)
    get_target_property(SKIPBUILD_EXECUTABLE skipbuild::skipbuild IMPORTED_LOCATION)
    if(NOT SKIPBUILD_EXECUTABLE)
        get_target_property(_skipbuild_noconfig skipbuild::skipbuild IMPORTED_LOCATION_NOCONFIG)
        if(_skipbuild_noconfig)
            set(SKIPBUILD_EXECUTABLE "${_skipbuild_noconfig}")
        endif()
    endif()
    if(NOT SKIPBUILD_EXECUTABLE)
        get_target_property(_skipbuild_configs skipbuild::skipbuild IMPORTED_CONFIGURATIONS)
        if(_skipbuild_configs)
            list(GET _skipbuild_configs 0 _skipbuild_first_config)
            get_target_property(SKIPBUILD_EXECUTABLE skipbuild::skipbuild IMPORTED_LOCATION_${_skipbuild_first_config})
        endif()
    endif()
endif()

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
    RESULT_VARIABLE _skipbuild_version_result
)

if(_skipbuild_version_result EQUAL 0)
    message(STATUS "Found skipbuild: ${SKIPBUILD_EXECUTABLE} (${SKIPBUILD_VERSION})")
else()
    message(WARNING "Found skipbuild at ${SKIPBUILD_EXECUTABLE}, but '--version' failed")
endif()

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
        # No directory-level CXX_COMPILER_LAUNCHER property before 3.19: apply
        # it to targets that already exist and warn about the limitation.
        get_property(targets DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)
        foreach(t ${targets})
            get_property(type TARGET ${t} PROPERTY TYPE)
            if(type STREQUAL "EXECUTABLE" OR type STREQUAL "STATIC_LIBRARY" OR type STREQUAL "SHARED_LIBRARY" OR type STREQUAL "MODULE_LIBRARY" OR type STREQUAL "OBJECT_LIBRARY")
                set_target_properties(${t} PROPERTIES CXX_COMPILER_LAUNCHER "${SKIPBUILD_EXECUTABLE}")
                message(STATUS "skipbuild enabled for existing target: ${t}")
            endif()
        endforeach()
        message(WARNING
            "CMake ${CMAKE_VERSION} has no global CXX_COMPILER_LAUNCHER; skipbuild was "
            "enabled only for targets that already exist. Use CMake 3.19+ or call "
            "skipbuild_enable_target() for each target.")
    endif()
    
    message(STATUS "skipbuild integration enabled")
endfunction()

# Provide a function to disable for specific targets
function(skipbuild_disable_target TARGET)
    if(TARGET ${TARGET})
        set_target_properties(${TARGET} PROPERTIES CXX_COMPILER_LAUNCHER "")
        message(STATUS "skipbuild disabled for target: ${TARGET}")
    endif()
endfunction()

# Provide cache directory configuration
function(skipbuild_set_cache_dir DIR)
    # ENV is process-global, so PARENT_SCOPE does not apply here.
    set(ENV{SKIPBUILD_CACHE_DIR} "${DIR}")
    message(STATUS "skipbuild cache directory set to: ${DIR}")
endfunction()