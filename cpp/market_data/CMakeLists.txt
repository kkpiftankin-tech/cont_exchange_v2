cmake_minimum_required(VERSION 3.24)
project(market_data LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif ()

set(Boost_NO_BOOST_CMAKE ON)

set(ABSL_PROPAGATE_CXX_STD ON)
find_package(absl REQUIRED)

find_package(Boost REQUIRED COMPONENTS thread)

include(FetchContent)

set(WITH_OPENSSL YES CACHE BOOL "Enable OpenSSL in clickhouse-cpp" FORCE)
FetchContent_Declare(
        clickhouse-cpp
        GIT_REPOSITORY https://github.com/ClickHouse/clickhouse-cpp.git
        GIT_TAG v2.6.0
)
FetchContent_MakeAvailable(clickhouse-cpp)

FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
FetchContent_MakeAvailable(json)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)

add_executable(market_data
        src/main.cpp
        src/app/market_data_uc.cpp
        src/app/update_order_book_uc.cpp
        src/transport/grpc_market_data_service.cpp
        src/infra/kafka_consumer.cpp
        src/domain/entities/order_book.cpp
        src/transport/mappers/order_book.cpp
        src/infra/clickhouse/order_book_storage.cpp
        src/infra/clickhouse/clickhouse_liquidity_curve_storage.cpp
        src/infra/clickhouse_storage.cpp
        src/infra/liquidity_curve_memory_storage.hpp
        src/domain/ports/i_liquidity_curve_storage.hpp
)

target_include_directories(market_data PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(market_data PRIVATE
        cex_common
        contracts_proto
        clickhouse-cpp-lib
        nlohmann_json::nlohmann_json
        PkgConfig::CURL
        Boost::thread
)

target_compile_options(market_data PRIVATE -Wall -Wextra -Wpedantic)

if (BUILD_TESTING)
    add_executable(market_data_uc_test
            tests/market_data_uc_test.cpp
            src/app/market_data_uc.cpp
            src/app/update_order_book_uc.cpp
            src/transport/mappers/order_book.cpp
            src/domain/entities/order_book.cpp
            src/infra/clickhouse/clickhouse_liquidity_curve_storage.cpp
    )

    target_include_directories(market_data_uc_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_link_libraries(market_data_uc_test PRIVATE
            cex_common
            contracts_proto
            Boost::thread
            clickhouse-cpp-lib
    )

    target_compile_options(market_data_uc_test PRIVATE -Wall -Wextra -Wpedantic)

    add_test(NAME market_data_uc_test COMMAND market_data_uc_test)

    add_executable(market_data_clickhouse_storage_test
            tests/clickhouse_storage_test.cpp
            src/infra/clickhouse_storage.cpp
    )

    target_include_directories(market_data_clickhouse_storage_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_link_libraries(market_data_clickhouse_storage_test PRIVATE
            cex_common
            contracts_proto
            PkgConfig::CURL
            clickhouse-cpp-lib
    )

    target_compile_options(market_data_clickhouse_storage_test PRIVATE -Wall -Wextra -Wpedantic)

    add_test(NAME market_data_clickhouse_storage_test COMMAND market_data_clickhouse_storage_test)
endif ()
