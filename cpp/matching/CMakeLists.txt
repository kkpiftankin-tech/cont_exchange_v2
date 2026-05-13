# =============================================================================
# Сборка сервиса `matching` — движка непрерывного матчинга (continuous clearing).
#
# Сервис читает поток нормализованных команд по ордерам из Kafka
# (orders.normalized), периодически запускает "решатель" — в MVP это простой
# имитатор, в продакшене сюда встанет полноценная LP/QP-задача — и публикует
# результат батча в топик batch.outputs.
# =============================================================================

cmake_minimum_required(VERSION 3.24)
project(matching LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Исполняемый файл сервиса (отдельный контейнер в Docker/k8s).
add_executable(matching
  src/main.cpp                  # entrypoint: читает env, запускает loop
  src/app/matching_loop.cpp     # потоки потребления и батч-таймера
)

# include-папка для удобных путей вида "app/matching_loop.hpp"
target_include_directories(matching PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Зависимости:
#   cex_common      — общий рантайм (Kafka, лог, протобуф-утилиты, decimal)
#   contracts_proto — сгенерированный код из contracts/*.proto
target_link_libraries(matching PRIVATE
  cex_common
  contracts_proto
)

target_compile_options(matching PRIVATE -Wall -Wextra -Wpedantic)
