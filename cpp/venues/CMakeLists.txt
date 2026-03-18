cmake_minimum_required(VERSION 3.24)
project(venues LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(venues
  src/main.cpp
  src/app/venues_loop.cpp
)

target_include_directories(venues PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(venues PRIVATE
  cex_common
  contracts_proto
)

target_compile_options(venues PRIVATE -Wall -Wextra -Wpedantic)
