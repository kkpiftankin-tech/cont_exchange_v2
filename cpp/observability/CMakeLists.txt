# =============================================================================
# Сборка сервиса `observability` — простой агрегатор интересных событий.
#
# В MVP подписывается на ключевые топики Kafka (risk.alerts, batch.outputs,
# execution.reports) и пишет краткие JSON-сводки в stdout. В прод-стенде
# здесь должны быть OpenTelemetry/Prometheus экспортёры и заливка в
# ClickHouse/Loki, чтобы строить дашборды и алерты.
# =============================================================================

cmake_minimum_required(VERSION 3.24)
project(observability LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(observability
  src/main.cpp
  src/app/obs_loop.cpp
)

target_include_directories(observability PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(observability PRIVATE
  cex_common
  contracts_proto
)

target_compile_options(observability PRIVATE -Wall -Wextra -Wpedantic)
