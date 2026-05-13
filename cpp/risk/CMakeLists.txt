# =============================================================================
# Сборка сервиса `risk` — pre-trade и post-trade рисковая логика.
#
# Ответственности:
#   * проверять новые ордера (CheckNewOrder);
#   * управлять "kill switch" (глобальная или по инструменту остановка торгов);
#   * принимать post-trade обновления (для будущих метрик/алертов);
#   * публиковать risk.alerts в Kafka, чтобы их видели observability и оператор.
# =============================================================================

cmake_minimum_required(VERSION 3.24)
project(risk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(risk
  src/main.cpp                                  # entrypoint: gRPC server
  src/transport/grpc_risk_service.cpp           # gRPC API
  src/app/risk_uc.cpp                           # use-cases: проверки и kill-switch
  src/infra/risk_alerts_publisher.cpp           # публикатор risk.alerts в Kafka
)

target_include_directories(risk PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(risk PRIVATE
  cex_common
  contracts_proto
)

target_compile_options(risk PRIVATE -Wall -Wextra -Wpedantic)
