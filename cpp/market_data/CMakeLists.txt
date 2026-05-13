# =============================================================================
# Сборка сервиса `market_data`.
#
# Назначение: агрегировать рыночные данные с внешних venue, кэшировать
# последний тикер по паре (venue, symbol) и отдавать его по gRPC.
# Источники: топик marketdata.raw в Kafka (туда пишет сервис venues).
# =============================================================================

cmake_minimum_required(VERSION 3.24)
project(market_data LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(market_data
  src/main.cpp                                      # entrypoint
  src/app/market_data_uc.cpp                        # use-cases / in-memory кэш
  src/transport/grpc_market_data_service.cpp        # gRPC API
  src/infra/kafka_consumer.cpp                      # фоновый консьюмер marketdata.raw
)

target_include_directories(market_data PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(market_data PRIVATE
  cex_common
  contracts_proto
)

target_compile_options(market_data PRIVATE -Wall -Wextra -Wpedantic)
