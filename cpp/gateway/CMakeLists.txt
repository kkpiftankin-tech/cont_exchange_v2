# =============================================================================
# Сборка сервиса `gateway` — внешний REST-edge биржи.
#
# Принимает HTTP/JSON от UI или внешних клиентов, конвертирует в proto и
# проксирует в gRPC к order_flow. Никакой бизнес-логики (только трансляция
# протоколов, в проде сюда же — auth, rate-limiting, audit-log).
# Использует Crow (header-only HTTP-сервер из third_party/) и Boost.Asio.
# =============================================================================

cmake_minimum_required(VERSION 3.24)
project(gateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Boost.Asio + thread нужны для Crow (HTTP-сервер).
find_package(Boost REQUIRED COMPONENTS system thread)

add_executable(gateway
  src/main.cpp                                # entrypoint
  src/transport/http_gateway.cpp              # HTTP-маршруты (Crow)
  src/infra/order_flow_client.cpp             # gRPC-клиент к order_flow
)

target_include_directories(gateway PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/crow/include   # header-only Crow
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(gateway PRIVATE
  cex_common
  contracts_proto
  Boost::system
  Boost::thread
)

target_compile_options(gateway PRIVATE -Wall -Wextra -Wpedantic)
