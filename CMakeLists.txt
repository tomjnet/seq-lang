cmake_minimum_required(VERSION 3.28)

project(seq-lang
  VERSION 0.1.0
  DESCRIPTION "Seq workflow language and its compiler, seqc"
  LANGUAGES C CXX)

# C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# The project does not use C++20 modules; skip the per-file dependency scan.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(SEQ_SANITIZE "Build seqc with address and undefined-behavior sanitizers" OFF)

# Versions reported by `seqc --version` and recorded in seq.lock (docs/).
set(SEQ_GRAMMAR_VERSION 1)
set(SEQ_PROMPT_VERSION 1)

set(SEQ_GIT_COMMIT "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE seq_git_commit
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE seq_git_result)
  if(seq_git_result EQUAL 0 AND seq_git_commit)
    set(SEQ_GIT_COMMIT "${seq_git_commit}")
  endif()
endif()

configure_file(
  cmake/version.hpp.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/seq/version.hpp"
  @ONLY)

# The build tree mirrors the install layout, so seqc runs from either:
#   bin/seqc
#   lib/seqc/include/seq_runtime.h
#   lib/seqc/lib/libseqrt.a
#   lib/seqc/templates/{gitignore,Makefile}.template
# seqc finds lib/seqc relative to its own location (or through SEQC_HOME).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(SEQ_HOME_DIR "${CMAKE_BINARY_DIR}/lib/seqc")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(SEQ_TARGET_LINUX ON)
else()
  set(SEQ_TARGET_LINUX OFF)
  message(STATUS
    "Seq targets Linux x86_64. On this platform seqc can scaffold, check, "
    "and clean projects, but cannot build or run them.")
endif()

# Compile settings shared by the compiler's own C++ targets.
add_library(seq_options INTERFACE)
target_include_directories(seq_options INTERFACE
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(MSVC)
  target_compile_options(seq_options INTERFACE /W4 /permissive- /utf-8)
  target_compile_definitions(seq_options INTERFACE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  target_compile_options(seq_options INTERFACE -Wall -Wextra -Wpedantic)
endif()

# Sanitizers apply to seqc and its tests only. The runtime library is linked
# into generated programs and must stay free of sanitizer dependencies.
if(SEQ_SANITIZE)
  if(MSVC)
    message(WARNING "SEQ_SANITIZE is only supported with GCC and Clang; ignoring.")
  else()
    target_compile_options(seq_options INTERFACE
      -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(seq_options INTERFACE -fsanitize=address,undefined)
  endif()
endif()

set(seq_core_sources
  src/backends/c/c_backend.cpp
  src/cache/cache.cpp
  src/cli/build.cpp
  src/cli/commands.cpp
  src/execution/execution.cpp
  src/lexer/lexer.cpp
  src/model/fake_adapter.cpp
  src/model/llama_server_adapter.cpp
  src/model/model.cpp
  src/model/pull.cpp
  src/parser/parser.cpp
  src/planner/planner.cpp
  src/project/project.cpp
  src/support/config.cpp
  src/support/diagnostics.cpp
  src/support/json.cpp
  src/support/sha256.cpp
  src/support/util.cpp
  src/toolchain/toolchain.cpp
  src/validation/validator.cpp)
if(SEQ_TARGET_LINUX)
  list(APPEND seq_core_sources src/support/process_linux.cpp)
else()
  list(APPEND seq_core_sources src/support/process_stub.cpp)
endif()

add_library(seqcore STATIC ${seq_core_sources})
target_link_libraries(seqcore PUBLIC seq_options)

add_executable(seqc src/cli/main.cpp)
target_link_libraries(seqc PRIVATE seqcore)

# The two project templates have a single source in docs/. They are copied
# byte for byte; `seqc new` and every build read them from lib/seqc/templates.
configure_file(docs/gitignore.template
  "${SEQ_HOME_DIR}/templates/gitignore.template" COPYONLY)
configure_file(docs/Makefile.template
  "${SEQ_HOME_DIR}/templates/Makefile.template" COPYONLY)

# Runtime library linked into every generated program. Plain C17, always
# optimized, independent of the build type of seqc itself.
if(SEQ_TARGET_LINUX)
  add_library(seqrt STATIC
    runtime/src/seq_chart.c
    runtime/src/seq_font.c
    runtime/src/seq_png.c
    runtime/src/seq_runtime.c)
  target_include_directories(seqrt PUBLIC runtime/include)
  set_target_properties(seqrt PROPERTIES
    C_STANDARD 17
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    ARCHIVE_OUTPUT_DIRECTORY "${SEQ_HOME_DIR}/lib")
  target_compile_options(seqrt PRIVATE -O2 -Wall -Wextra -Wpedantic)
  configure_file(runtime/include/seq_runtime.h
    "${SEQ_HOME_DIR}/include/seq_runtime.h" COPYONLY)
  add_dependencies(seqc seqrt)
endif()

include(GNUInstallDirs)
install(TARGETS seqc RUNTIME DESTINATION bin)
install(FILES docs/gitignore.template docs/Makefile.template
  DESTINATION lib/seqc/templates)
if(SEQ_TARGET_LINUX)
  install(TARGETS seqrt ARCHIVE DESTINATION lib/seqc/lib)
  install(FILES runtime/include/seq_runtime.h DESTINATION lib/seqc/include)
endif()
install(FILES LICENSE THIRD_PARTY_NOTICES.md README.md
  DESTINATION share/doc/seqc)

# Release tarball: `cpack` in the build directory writes
# seqc-<version>-Linux-x86_64.tar.gz and its SHA-256 checksum file.
set(CPACK_GENERATOR TGZ)
set(CPACK_PACKAGE_NAME seqc)
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
  "seqc-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
include(CPack)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
