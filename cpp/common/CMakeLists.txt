cmake_minimum_required(VERSION 3.24)
project(cex_common LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(RDKAFKA REQUIRED IMPORTED_TARGET rdkafka++)

add_library(cex_common STATIC
  src/env.cpp
  src/uuid.cpp
  src/time.cpp
  src/log.cpp
  src/proto.cpp
  src/decimal.cpp
  src/kafka.cpp
)

target_include_directories(cex_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(cex_common PUBLIC
  contracts_proto
  protobuf::libprotobuf
  gRPC::grpc++
  PkgConfig::RDKAFKA
)

target_compile_options(cex_common PRIVATE -Wall -Wextra -Wpedantic)
