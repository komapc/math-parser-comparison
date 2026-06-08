cmake_minimum_required(VERSION 3.20)
project(math_parser_bench CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# CMake 3.28 lacks the GCC 14 / Clang 17 → -std=c++26 mapping; set the flag directly.
# Remove this block once CMake gains native C++26 support.
if((CMAKE_CXX_COMPILER_ID STREQUAL "GNU"   AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "14") OR
   (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "17"))
  add_compile_options(-std=c++26)
else()
  set(CMAKE_CXX_STANDARD 23)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Shared infrastructure (tokenizer + AST) and all evaluation strategies.
# NOTE: src/parallel.cpp (single-expression parallelism) is intentionally
# excluded for now -- it is allocation-bound and disabled pending an arena
# allocator. The file is kept on disk for reference.
find_package(Threads REQUIRED)

add_library(parsers
  src/lexer.cpp
  src/ast.cpp
  src/recursive_descent.cpp
  src/shunting_yard.cpp
  src/pratt.cpp
  src/multipass.cpp
  src/multipass_arena.cpp
  src/multipass_opt.cpp
  src/multipass_lean.cpp
  src/multipass_parallel.cpp
  src/arena_ast.cpp
  src/direct_recursive_descent.cpp
  src/direct_shunting_yard.cpp
  src/bytecode.cpp
  src/evaluators.cpp
  src/reeval.cpp
)
target_include_directories(parsers PUBLIC include)
target_link_libraries(parsers PUBLIC Threads::Threads)

# Benchmark harnesses.
add_executable(bench bench/benchmark.cpp)        # one-shot: string -> value
target_link_libraries(bench PRIVATE parsers)

add_executable(reeval bench/reeval.cpp)          # compile once, evaluate many
target_link_libraries(reeval PRIVATE parsers)

add_executable(parallel_bench bench/parallel_bench.cpp)  # batch scaling 1-8T
target_link_libraries(parallel_bench PRIVATE parsers)

add_executable(single_par_bench bench/single_par_bench.cpp)  # single-expr fork-join
target_link_libraries(single_par_bench PRIVATE parsers)

# Correctness test suite (dependency-free), run via CTest.
enable_testing()
add_executable(tests tests/test_parsers.cpp)
target_link_libraries(tests PRIVATE parsers)
add_test(NAME parsers_correctness COMMAND tests)
