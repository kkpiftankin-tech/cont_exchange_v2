cmake_minimum_required(VERSION 3.24)
project(continuous_exchange_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(CTest)
option(CEX_BUILD_OBSERVABILITY "Build observability service" ON)
option(CEX_BUILD_MARKET_DATA "Build market data service" ON)
option(CEX_BUILD_VENUES "Build venues service" ON)

# Keep FindBoost module behavior on newer CMake (CMP0167).
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

# Homebrew Boost 1.90+ may expose incomplete CMake package components
# (e.g. missing boost_systemConfig.cmake), which breaks configure.
# Force CMake's FindBoost module for portable behavior across dev/CI/docker.
set(Boost_NO_BOOST_CMAKE ON)

# Put all binaries into build/bin for easy Docker COPY.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_subdirectory(contracts)
add_subdirectory(cpp/common)

add_subdirectory(cpp/gateway)
add_subdirectory(cpp/order_flow)
add_subdirectory(cpp/risk)
add_subdirectory(cpp/ledger)
add_subdirectory(cpp/matching)
if (CEX_BUILD_MARKET_DATA)
  add_subdirectory(cpp/market_data)
endif()
if (CEX_BUILD_VENUES)
  add_subdirectory(cpp/venues)
endif()
add_subdirectory(cpp/backtest)
if (CEX_BUILD_OBSERVABILITY)
  add_subdirectory(cpp/observability)
endif()
add_subdirectory(cpp/venue_health)

# Force C++20 for all targets
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20 -O2")
