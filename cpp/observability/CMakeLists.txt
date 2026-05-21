cmake_minimum_required(VERSION 3.24)
project(observability LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif ()

set(Boost_NO_BOOST_CMAKE ON)

find_package(Boost REQUIRED COMPONENTS thread)

include(FetchContent)

FetchContent_Declare(
        httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG master
)
FetchContent_MakeAvailable(httplib)

add_executable(observability
        src/main.cpp
        src/app/service.cpp
        src/infra/kafka/kafka_consumer.cpp
        src/infra/mappers/venue_health.cpp
        src/infra/mappers/venue_liquidity_curve.cpp
        src/transport/grpc/observability_service.cpp
        src/transport/mappers/alert.cpp
)

target_include_directories(observability PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(observability PRIVATE
        cex_common
        contracts_proto
        httplib::httplib
        Boost::thread
)

target_compile_options(observability PRIVATE -Wall -Wextra -Wpedantic)
