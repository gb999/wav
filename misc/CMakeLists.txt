cmake_minimum_required(VERSION 3.21)
project(wav C)

# cmake -S misc -B build && cmake --build build && ctest --test-dir build
# options: -DWAV_SANITIZE=ON  -DWAV_COVERAGE=ON (clang, adds the coverage targets)  -DWAV_WERROR=ON
# coverage needs llvm-cov and llvm-profdata from the same LLVM as the compiler: found next to it, or set -DWAV_LLVM_COV= and -DWAV_LLVM_PROFDATA=

option(WAV_SANITIZE "AddressSanitizer and UBSan for the tests" OFF)
option(WAV_COVERAGE "clang source coverage for the tests, adds the coverage and coverage-html targets" OFF)
option(WAV_WERROR "treat warnings as errors" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(WAV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

include(CheckLanguage)
check_language(CXX)
if(CMAKE_CXX_COMPILER)
    enable_language(CXX)
endif()

function(wav_configure target)
    target_include_directories(${target} PRIVATE "${WAV_ROOT}")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(WAV_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(WAV_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

enable_testing()

add_library(wav_lint_c OBJECT lint_impl.c)
wav_configure(wav_lint_c)
if(CMAKE_CXX_COMPILER)
    add_library(wav_lint_cpp OBJECT lint_decl.cpp)
    wav_configure(wav_lint_cpp)
endif()

add_executable(wav_test test.c)
wav_configure(wav_test)
add_test(NAME wav_test COMMAND wav_test)

if(WAV_SANITIZE)
    if(MSVC)
        target_compile_options(wav_test PRIVATE /fsanitize=address /Zi)
    else()
        target_compile_options(wav_test PRIVATE -g -fsanitize=address,undefined -fno-sanitize-recover=undefined)
        target_link_options(wav_test PRIVATE -fsanitize=address,undefined)
    endif()
endif()

find_package(Threads REQUIRED)
add_executable(wav_example example.c example_platform.h)
wav_configure(wav_example)
target_link_libraries(wav_example PRIVATE Threads::Threads $<$<BOOL:${UNIX}>:m>)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/example_out")
add_test(NAME wav_example COMMAND wav_example WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/example_out")

add_executable(wav_bench EXCLUDE_FROM_ALL bench.c)
wav_configure(wav_bench)
target_compile_options(wav_bench PRIVATE $<IF:$<BOOL:${MSVC}>,/O2,-O2>)
add_custom_target(bench COMMAND wav_bench DEPENDS wav_bench USES_TERMINAL)

if(WAV_COVERAGE)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "WAV_COVERAGE needs clang")
    endif()
    target_compile_options(wav_test PRIVATE -O0 -fprofile-instr-generate -fcoverage-mapping)
    target_link_options(wav_test PRIVATE -fprofile-instr-generate)

    get_filename_component(compiler_dir "${CMAKE_C_COMPILER}" DIRECTORY)
    string(REGEX MATCH "^[0-9]+" compiler_major "${CMAKE_C_COMPILER_VERSION}")
    find_program(WAV_LLVM_COV NAMES llvm-cov-${compiler_major} llvm-cov HINTS "${compiler_dir}")
    find_program(WAV_LLVM_PROFDATA NAMES llvm-profdata-${compiler_major} llvm-profdata HINTS "${compiler_dir}")
    if(APPLE AND CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
        execute_process(COMMAND xcrun -f llvm-cov OUTPUT_VARIABLE xcrun_cov OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        execute_process(COMMAND xcrun -f llvm-profdata OUTPUT_VARIABLE xcrun_profdata OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT WAV_LLVM_COV AND xcrun_cov)
            set(WAV_LLVM_COV "${xcrun_cov}" CACHE FILEPATH "llvm-cov" FORCE)
        endif()
        if(NOT WAV_LLVM_PROFDATA AND xcrun_profdata)
            set(WAV_LLVM_PROFDATA "${xcrun_profdata}" CACHE FILEPATH "llvm-profdata" FORCE)
        endif()
    endif()
    if(NOT WAV_LLVM_COV OR NOT WAV_LLVM_PROFDATA)
        message(FATAL_ERROR "llvm-cov and llvm-profdata not found next to ${CMAKE_C_COMPILER}, set WAV_LLVM_COV and WAV_LLVM_PROFDATA")
    endif()

    set(wav_coverage_args
        -DTEST_EXE=$<TARGET_FILE:wav_test>
        -DLLVM_COV=${WAV_LLVM_COV}
        -DLLVM_PROFDATA=${WAV_LLVM_PROFDATA}
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/coverage
        -DHEADER_A=${WAV_ROOT}/wav.h
        -DHEADER_B=${WAV_ROOT}/wav_convert.h)
    add_custom_target(coverage
        COMMAND "${CMAKE_COMMAND}" ${wav_coverage_args} -P "${CMAKE_CURRENT_SOURCE_DIR}/coverage.cmake"
        DEPENDS wav_test USES_TERMINAL)
    add_custom_target(coverage-html
        COMMAND "${CMAKE_COMMAND}" ${wav_coverage_args} -DHTML=ON -P "${CMAKE_CURRENT_SOURCE_DIR}/coverage.cmake"
        DEPENDS wav_test USES_TERMINAL)
endif()
