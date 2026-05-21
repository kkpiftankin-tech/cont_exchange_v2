cmake_minimum_required(VERSION 3.24)
project(venue_health LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(venue_health
        src/main.cpp
        src/app/service.cpp
        src/domain/entities/circuit_breaker.cpp
        src/domain/entities/venue_state.cpp
        src/infra/kafka/kafka_consumer.cpp
        src/infra/kafka/kafka_producer.cpp
        src/infra/mappers/raw_report.cpp
        src/infra/mappers/venue_state.cpp
)

target_include_directories(venue_health PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(venue_health PRIVATE
        cex_common
        contracts_proto
)

target_compile_options(venue_health PRIVATE -Wall -Wextra -Wpedantic)

add_subdirectory(tests)
