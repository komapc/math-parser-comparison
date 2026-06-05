cmake_minimum_required(VERSION 3.20)
project(math_parser_bench CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Shared infrastructure (tokenizer + AST) and all evaluation strategies.
# NOTE: src/parallel.cpp (single-expression parallelism) is intentionally
# excluded for now -- it is allocation-bound and disabled pending an arena
# allocator. The file is kept on disk for reference.
add_library(parsers
  src/lexer.cpp
  src/ast.cpp
  src/recursive_descent.cpp
  src/shunting_yard.cpp
  src/pratt.cpp
  src/arena_ast.cpp
  src/direct_recursive_descent.cpp
  src/direct_shunting_yard.cpp
  src/rpn.cpp
  src/bytecode.cpp
  src/evaluators.cpp
  src/reeval.cpp
)
target_include_directories(parsers PUBLIC include)

# Benchmark harnesses.
add_executable(bench bench/benchmark.cpp)        # one-shot: string -> value
target_link_libraries(bench PRIVATE parsers)

add_executable(reeval bench/reeval.cpp)          # compile once, evaluate many
target_link_libraries(reeval PRIVATE parsers)

# Correctness test suite (dependency-free), run via CTest.
enable_testing()
add_executable(tests tests/test_parsers.cpp)
target_link_libraries(tests PRIVATE parsers)
add_test(NAME parsers_correctness COMMAND tests)
