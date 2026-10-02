# Deterministic test suites. None of them needs a model, a network, or an
# installed Google Test: model responses come from the scripted fake adapter
# and generated tests are built against the minigtest stand-in below.
#
#   cli.*          smoke tests of the command line
#   unit           lexer, parser, validator, planner, backend policy, project
#   runtime.*      the C runtime library, tested directly in C
#   security       denied-access tests of the sandbox (Linux)
#   integration.*  the whole pipeline, driven through seqc (Linux)

string(REPLACE "." "\\." seq_version_regex "${PROJECT_VERSION}")

add_test(NAME cli.version COMMAND seqc --version)
set_tests_properties(cli.version PROPERTIES
  PASS_REGULAR_EXPRESSION "seqc ${seq_version_regex}")

add_test(NAME cli.help COMMAND seqc --help)
set_tests_properties(cli.help PROPERTIES
  PASS_REGULAR_EXPRESSION "Usage:")

add_test(NAME cli.unknown_command COMMAND seqc frobnicate)
set_tests_properties(cli.unknown_command PROPERTIES WILL_FAIL TRUE)

# The shipped examples must stay valid workflows.
foreach(example hello top3Company)
  add_test(NAME examples.${example}
    COMMAND seqc check "${PROJECT_SOURCE_DIR}/examples/${example}/src/main.seq")
  set_tests_properties(examples.${example} PROPERTIES
    PASS_REGULAR_EXPRESSION "name:    ${example}")
endforeach()

# --- Unit tests ---------------------------------------------------------------

add_executable(seq_unit_tests
  unit/test_main.cpp
  unit/test_backend.cpp
  unit/test_language.cpp
  unit/test_pipeline.cpp
  unit/test_support.cpp)
target_link_libraries(seq_unit_tests PRIVATE seqcore)
target_compile_definitions(seq_unit_tests PRIVATE
  SEQ_TEST_HOME_DIR="${SEQ_HOME_DIR}"
  SEQ_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME unit COMMAND seq_unit_tests)

if(NOT SEQ_TARGET_LINUX)
  return()
endif()

find_package(Python3 COMPONENTS Interpreter)

# --- Runtime library ----------------------------------------------------------

add_executable(seq_runtime_test runtime/runtime_test.c)
target_link_libraries(seq_runtime_test PRIVATE seqrt m)
set_target_properties(seq_runtime_test PROPERTIES
  C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(seq_runtime_test PRIVATE -Wall -Wextra -Wpedantic)

set(seq_runtime_work "${CMAKE_CURRENT_BINARY_DIR}/runtime-work")
add_test(NAME runtime.prepare
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${seq_runtime_work}")
add_test(NAME runtime.supervised
  COMMAND seq_runtime_test supervised "${seq_runtime_work}")
add_test(NAME runtime.manual
  COMMAND seq_runtime_test manual "${seq_runtime_work}-manual")
set_tests_properties(runtime.prepare PROPERTIES FIXTURES_SETUP seq_runtime_dir)
set_tests_properties(runtime.supervised PROPERTIES
  FIXTURES_REQUIRED seq_runtime_dir
  FIXTURES_SETUP seq_runtime_charts)
if(Python3_FOUND)
  # The PNG encoder is written for this project, so its output is decoded
  # here by an independent implementation.
  add_test(NAME runtime.png_decodes
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/runtime/check_png.py"
            "${seq_runtime_work}/out/charts/top.png" 800 480 3)
  add_test(NAME runtime.png_decodes_many
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/runtime/check_png.py"
            "${seq_runtime_work}/out/many.png" 800 480 6)
  set_tests_properties(runtime.png_decodes runtime.png_decodes_many PROPERTIES
    FIXTURES_REQUIRED seq_runtime_charts)
endif()

# --- Sandbox ------------------------------------------------------------------

add_executable(seq_sandbox_probe security/sandbox_probe.c)
target_link_options(seq_sandbox_probe PRIVATE -static)
set_target_properties(seq_sandbox_probe PROPERTIES
  C_STANDARD 17 C_STANDARD_REQUIRED ON)

add_executable(seq_sandbox_tests
  unit/test_main.cpp
  security/sandbox_test.cpp)
target_include_directories(seq_sandbox_tests PRIVATE unit)
target_link_libraries(seq_sandbox_tests PRIVATE seqcore)
target_compile_definitions(seq_sandbox_tests PRIVATE
  SEQ_PROBE_PATH="$<TARGET_FILE:seq_sandbox_probe>"
  SEQ_TEST_HOME_DIR="${SEQ_HOME_DIR}")
add_dependencies(seq_sandbox_tests seq_sandbox_probe seqrt)
add_test(NAME security COMMAND seq_sandbox_tests)

# --- minigtest ----------------------------------------------------------------
# A stand-in for Google Test with the same libraries and layout, selected by
# the integration tests through toolchain.gtest_root. It is static and free of
# sanitizers because seqc links it into generated test binaries.

set(SEQ_MINIGTEST_ROOT "${CMAKE_CURRENT_BINARY_DIR}/minigtest")
file(COPY fixtures/minigtest/include DESTINATION "${SEQ_MINIGTEST_ROOT}")
add_library(minigtest STATIC fixtures/minigtest/src/minigtest.cc)
add_library(minigtest_main STATIC fixtures/minigtest/src/minigtest_main.cc)
foreach(target minigtest minigtest_main)
  target_include_directories(${target} PRIVATE fixtures/minigtest/include)
  set_target_properties(${target} PROPERTIES
    CXX_STANDARD 17
    ARCHIVE_OUTPUT_DIRECTORY "${SEQ_MINIGTEST_ROOT}/lib")
endforeach()
set_target_properties(minigtest PROPERTIES OUTPUT_NAME gtest)
set_target_properties(minigtest_main PROPERTIES OUTPUT_NAME gtest_main)
add_dependencies(seqc minigtest minigtest_main)

# --- Integration --------------------------------------------------------------

if(NOT Python3_FOUND)
  message(STATUS "python3 not found: integration tests are not registered.")
  return()
endif()

set(seq_integration_tests
  check
  hello
  cache
  repair
  generated_tests
  outputs
  failures
  model_errors
  prerequisites
  housekeeping
  model_pull
  llama_server
  system_gtest)

foreach(name ${seq_integration_tests})
  add_test(NAME integration.${name}
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/integration/${name}.sh")
  # Generous: a build tree on a slow filesystem, such as a Windows drive
  # under WSL, takes several minutes for the larger tests.
  set_tests_properties(integration.${name} PROPERTIES
    TIMEOUT 1200
    SKIP_RETURN_CODE 77
    ENVIRONMENT "SEQC=$<TARGET_FILE:seqc>;SEQ_FIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/fixtures;SEQ_INTEGRATION=${CMAKE_CURRENT_SOURCE_DIR}/integration;SEQ_MINIGTEST_ROOT=${SEQ_MINIGTEST_ROOT};SEQ_SOURCE_DIR=${PROJECT_SOURCE_DIR};SEQ_WORK=${CMAKE_CURRENT_BINARY_DIR}/integration-work/${name};PYTHON=${Python3_EXECUTABLE}")
endforeach()
