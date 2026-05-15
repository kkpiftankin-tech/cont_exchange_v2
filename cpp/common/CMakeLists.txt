cmake_minimum_required(VERSION 3.22)
project(cex_common LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Protobuf CONFIG QUIET)
if (NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
endif ()
find_package(gRPC CONFIG QUIET)

find_package(PkgConfig REQUIRED)
pkg_check_modules(RDKAFKA REQUIRED IMPORTED_TARGET rdkafka++)

if (TARGET protobuf::libprotobuf)
    set(PROTOBUF_LIB_TARGET protobuf::libprotobuf)
elseif (TARGET protobuf::protobuf)
    set(PROTOBUF_LIB_TARGET protobuf::protobuf)
else ()
    message(FATAL_ERROR "Protobuf CMake target was not found")
endif ()

if (TARGET gRPC::grpc++)
    set(GRPC_LIB_TARGET gRPC::grpc++)
else ()
    pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
    set(GRPC_LIB_TARGET PkgConfig::GRPCPP)
endif ()

add_library(cex_common STATIC
        src/env.cpp
        src/uuid.cpp
        src/time.cpp
        src/log.cpp
        src/proto.cpp
        src/decimal.cpp
        src/kafka.cpp
        src/replay_kafka.cpp
        include/cex/common/defer.hpp
)

target_include_directories(cex_common PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(cex_common PUBLIC
        contracts_proto
        ${PROTOBUF_LIB_TARGET}
        ${GRPC_LIB_TARGET}
        PkgConfig::RDKAFKA
)

target_compile_options(cex_common PRIVATE -Wall -Wextra -Wpedantic)
