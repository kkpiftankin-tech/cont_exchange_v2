cmake_minimum_required(VERSION 3.24)
project(matching LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(matching
  src/main.cpp
  src/app/matching_loop.cpp
)

target_include_directories(matching PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(matching PRIVATE
  cex_common
  contracts_proto
)

target_compile_options(matching PRIVATE -Wall -Wextra -Wpedantic)
