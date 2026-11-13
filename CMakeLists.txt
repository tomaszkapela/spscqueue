cmake_minimum_required(VERSION 3.20)
project(spscqueue VERSION 1.0.0 LANGUAGES CXX)

add_library(spsc_queue INTERFACE)
add_library(spsc::queue ALIAS spsc_queue)
target_include_directories(spsc_queue INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(spsc_queue INTERFACE cxx_std_20)
# GCC warns about the interference size in headers because it depends on -mtune.
# Build every user of a queue type with the same flags.
target_compile_options(spsc_queue INTERFACE $<$<CXX_COMPILER_ID:GNU>:-Wno-interference-size>)

# Development only: a parent project (FetchContent or add_subdirectory) gets just spsc::queue.
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  return()
endif()

include(CTest)
option(SPSC_WARNINGS_AS_ERRORS "Treat project test warnings as errors" ON)

# clang-tidy on the tests and the header as they compile. The extra argument
# hides GCC-only warning flags.
option(SPSC_CLANG_TIDY "Run clang-tidy on the tests while building" OFF)
if(SPSC_CLANG_TIDY)
  find_program(CLANG_TIDY NAMES clang-tidy REQUIRED)
  set(spsc_clang_tidy "${CLANG_TIDY}" --extra-arg=-Wno-unknown-warning-option)
  if(SPSC_WARNINGS_AS_ERRORS)
    list(APPEND spsc_clang_tidy --warnings-as-errors=*)
  endif()
endif()

if(BUILD_TESTING)
  find_package(GTest CONFIG QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG f8d7d77c06936315286eb55f8de22cd23c188571) # v1.14.0
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
  find_package(Threads REQUIRED)
  # spsc_test(<target> <sources>...)
  function(spsc_test target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE spsc::queue GTest::gtest_main Threads::Threads)
    set_target_properties(${target} PROPERTIES
      CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic)
    if(SPSC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(SPSC_CLANG_TIDY)
      set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${spsc_clang_tidy}")
    endif()
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 90)
  endfunction()
  spsc_test(spsc_core_tests tests/core_test.cpp tests/core_compile_time_test.cpp)
endif()

# `format` rewrites the sources; `format-check` fails on any deviation.
find_program(CLANG_FORMAT NAMES clang-format)
if(CLANG_FORMAT)
  file(GLOB_RECURSE spsc_format_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
  add_custom_target(format
    COMMAND "${CLANG_FORMAT}" -i ${spsc_format_sources}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Formatting sources with ${CLANG_FORMAT}" VERBATIM)
  add_custom_target(format-check
    COMMAND "${CLANG_FORMAT}" --dry-run -Werror ${spsc_format_sources}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Checking formatting with ${CLANG_FORMAT}" VERBATIM)
endif()
