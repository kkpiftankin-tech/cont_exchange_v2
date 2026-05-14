cmake_minimum_required(VERSION 3.24)
project(continuous_exchange_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Put all binaries into build/bin for easy Docker COPY.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Custom CMake modules (e.g. FindgRPC.cmake for pkg-config-based gRPC lookup).
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

add_subdirectory(contracts)
add_subdirectory(cpp/common)

add_subdirectory(cpp/gateway)
add_subdirectory(cpp/order_flow)
add_subdirectory(cpp/risk)
add_subdirectory(cpp/ledger)
add_subdirectory(cpp/matching)
add_subdirectory(cpp/market_data)
add_subdirectory(cpp/venues)
add_subdirectory(cpp/observability)
