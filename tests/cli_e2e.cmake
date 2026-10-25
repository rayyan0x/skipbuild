# End-to-end test of the `skipbuild -- <compiler>` entry point: a real compiler
# is wrapped, the first compile must MISS, an identical second compile must HIT,
# and editing an included header must invalidate the entry again.
#
# Required variables:
#   SKIPBUILD    - path to the launcher executable
#   COMPILER     - path to a C++ compiler
#   COMPILER_ID  - CMake compiler id (MSVC changes the flag spelling)
#   WORK         - scratch directory

foreach(var SKIPBUILD COMPILER COMPILER_ID WORK)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "cli_e2e.cmake: ${var} is required")
    endif()
endforeach()

set(work "${WORK}")
set(cache "${work}/cache")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

set(header "${work}/h.h")
set(source "${work}/a.cpp")
file(WRITE "${header}" "#define VAL 1\n")
file(WRITE "${source}" "#include \"h.h\"\nint f() { return VAL; }\n")

if(COMPILER_ID STREQUAL "MSVC")
    set(obj "${work}/a.obj")
    set(compile_args /nologo /c /I "${work}" "${source}" "/Fo${obj}")
else()
    set(obj "${work}/a.o")
    set(compile_args -c -I "${work}" "${source}" -o "${obj}")
endif()

function(run_launcher out_var)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                "SKIPBUILD_DIR=${cache}"
                "SKIPBUILD_CACHE_DIR=${cache}"
                "SKIPBUILD_DEBUG=1"
                "SKIPBUILD_LOG_FILE=${work}/launcher.log"
                "${SKIPBUILD}" -- "${COMPILER}" ${ARGN}
        WORKING_DIRECTORY "${work}"
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
        RESULT_VARIABLE rc
    )
    message(STATUS "run rc=${rc} out=${out} err=${err}")
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "launcher exited with ${rc}\nstdout:\n${out}\nstderr:\n${err}")
    endif()
    if(NOT EXISTS "${obj}")
        message(FATAL_ERROR "compiler produced no object file\nstdout:\n${out}\nstderr:\n${err}")
    endif()
    set(${out_var} "${out}${err}" PARENT_SCOPE)
endfunction()

if(NOT EXISTS "${SKIPBUILD}")
    message(FATAL_ERROR "launcher not found: ${SKIPBUILD}")
endif()
message(STATUS "skipbuild=${SKIPBUILD} compiler=${COMPILER} args=${compile_args}")

run_launcher(first ${compile_args})
if(first MATCHES "HIT")
    message(FATAL_ERROR "expected MISS on first compile, got:\n${first}")
endif()

run_launcher(second ${compile_args})
if(NOT second MATCHES "HIT")
    file(READ "${work}/launcher.log" launch_log OPTIONAL)
    file(GLOB_RECURSE cached_files "${cache}/*")
    list(LENGTH cached_files cached_count)
    message(FATAL_ERROR "expected HIT on second compile\ncompiler: ${COMPILER}\nfirst:\n${first}\nsecond:\n${second}\ncached_files=${cached_count}\nlog:\n${launch_log}")
endif()

file(WRITE "${header}" "#define VAL 2\n")
run_launcher(third ${compile_args})
if(third MATCHES "HIT")
    message(FATAL_ERROR "header edit did not invalidate the cache, got:\n${third}")
endif()

# A read-only cache must refuse destructive commands and keep its entries.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            "SKIPBUILD_DIR=${cache}"
            "SKIPBUILD_CACHE_DIR=${cache}"
            "${SKIPBUILD}" --read-only true --clean
    WORKING_DIRECTORY "${work}"
    OUTPUT_VARIABLE ro_out
    ERROR_VARIABLE ro_err
    RESULT_VARIABLE ro_rc
)
if(ro_rc EQUAL 0)
    message(FATAL_ERROR "read-only --clean unexpectedly succeeded\nstdout:\n${ro_out}\nstderr:\n${ro_err}")
endif()
if(NOT ro_err MATCHES "read-only")
    message(FATAL_ERROR "read-only --clean did not explain the refusal\nstderr:\n${ro_err}")
endif()
if(NOT EXISTS "${cache}")
    message(FATAL_ERROR "read-only --clean removed the cache directory")
endif()

# The same command is allowed once the cache is writable again.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            "SKIPBUILD_DIR=${cache}"
            "SKIPBUILD_CACHE_DIR=${cache}"
            "${SKIPBUILD}" --clean
    WORKING_DIRECTORY "${work}"
    OUTPUT_VARIABLE clean_out
    ERROR_VARIABLE clean_err
    RESULT_VARIABLE clean_rc
)
if(NOT clean_rc EQUAL 0)
    message(FATAL_ERROR "writable --clean failed with ${clean_rc}\nstdout:\n${clean_out}\nstderr:\n${clean_err}")
endif()
if(EXISTS "${cache}")
    message(FATAL_ERROR "--clean did not remove the cache directory")
endif()

message(STATUS "cli e2e test passed")
