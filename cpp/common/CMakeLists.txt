cmake_minimum_required(VERSION 3.24)
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
        src/compensation_reversal.cpp
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

# F-09 (T-F09-044 prereq): unit-тест Decimal::div (деньги — §9, покрываем тестом).
include(CTest)
if(BUILD_TESTING)
  add_executable(cex_common_decimal_test tests/decimal_test.cpp)
  target_link_libraries(cex_common_decimal_test PRIVATE cex_common)
  target_compile_options(cex_common_decimal_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME cex_common_decimal_test COMMAND cex_common_decimal_test)

  # F-09 MVP-6 (ADR-039 / ADR-040 §4): pure ComputeReversals (перенесён из matching).
  add_executable(cex_common_compensation_reversal_test tests/compensation_reversal_test.cpp)
  target_link_libraries(cex_common_compensation_reversal_test PRIVATE cex_common)
  target_compile_options(cex_common_compensation_reversal_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME cex_common_compensation_reversal_test COMMAND cex_common_compensation_reversal_test)
endif()
