# Run by the coverage targets: cmake -DTEST_EXE=... -DLLVM_COV=... -DLLVM_PROFDATA=... -DWORK_DIR=... -DHEADER_A=... -DHEADER_B=... [-DHTML=ON] -P coverage.cmake

file(MAKE_DIRECTORY "${WORK_DIR}")
set(raw "${WORK_DIR}/test.profraw")
set(data "${WORK_DIR}/test.profdata")
file(REMOVE "${raw}")

execute_process(COMMAND "${CMAKE_COMMAND}" -E env "LLVM_PROFILE_FILE=${raw}" "${TEST_EXE}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "tests failed")
endif()

execute_process(COMMAND "${LLVM_PROFDATA}" merge -sparse "${raw}" -o "${data}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "llvm-profdata could not read ${raw}: is it from the same LLVM as the compiler?")
endif()

execute_process(
    COMMAND "${LLVM_COV}" report "${TEST_EXE}" "-instr-profile=${data}" -show-branch-summary -use-color=false "${HEADER_A}" "${HEADER_B}"
    OUTPUT_VARIABLE report
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "llvm-cov report failed")
endif()
message("${report}")

# TOTAL columns: regions, missed, cover, functions, missed, cover, lines, missed, cover, branches, missed, cover
string(REGEX MATCH "TOTAL[^\n]*" total "${report}")
string(REGEX REPLACE "[ \t]+" ";" fields "${total}")
list(GET fields 2 missed_regions)
list(GET fields 8 missed_lines)
list(GET fields 11 missed_branches)
if(NOT missed_regions EQUAL 0 OR NOT missed_lines EQUAL 0 OR NOT missed_branches EQUAL 0)
    message(FATAL_ERROR "coverage below 100%: build the coverage-html target to see what is missed")
endif()

if(HTML)
    execute_process(
        COMMAND "${LLVM_COV}" show "${TEST_EXE}" "-instr-profile=${data}" -format=html -show-branches=count "-output-dir=${WORK_DIR}/html" "${HEADER_A}" "${HEADER_B}"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "llvm-cov show failed")
    endif()
    message("open ${WORK_DIR}/html/index.html")
endif()
