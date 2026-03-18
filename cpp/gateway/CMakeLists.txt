cmake_minimum_required(VERSION 3.24)
project(gateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS system thread)

add_executable(gateway
  src/main.cpp
  src/transport/http_gateway.cpp
  src/infra/order_flow_client.cpp
)

target_include_directories(gateway PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/crow/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(gateway PRIVATE
  cex_common
  contracts_proto
  Boost::system
  Boost::thread
)

target_compile_options(gateway PRIVATE -Wall -Wextra -Wpedantic)
