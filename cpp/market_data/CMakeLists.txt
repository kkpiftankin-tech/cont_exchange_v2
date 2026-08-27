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

# PostgreSQL (для kill-switch / marketdata_config)
find_package(PostgreSQL REQUIRED)
find_package(libpqxx CONFIG QUIET)
if(libpqxx_FOUND)
  if(TARGET libpqxx::pqxx)
    set(MARKET_DATA_PQXX_TARGET libpqxx::pqxx)
  elseif(TARGET pqxx)
    set(MARKET_DATA_PQXX_TARGET pqxx)
  else()
    message(FATAL_ERROR "libpqxx found but no CMake target available")
  endif()
else()
  pkg_check_modules(LIBPQXX REQUIRED IMPORTED_TARGET libpqxx)
  set(MARKET_DATA_PQXX_TARGET PkgConfig::LIBPQXX)
endif()

add_executable(market_data
        src/main.cpp
        # app
        src/app/market_data_uc.cpp
        src/app/update_order_book_uc.cpp
        src/app/compute_market_data.cpp
        src/app/curve_to_levels.cpp
        # infra/postgres
        src/infra/postgres/pg_market_data_config.cpp
        # transport
        src/transport/grpc_market_data_service.cpp
        src/transport/mappers/order_book.cpp
        src/transport/mappers/vectorized_liquidity.cpp
        # domain
        src/domain/entities/order_book.cpp
        src/domain/vectorize.cpp
        # infra
        src/infra/kafka_consumer.cpp
        src/infra/kafka_snapshots_producer.cpp
        src/infra/kafka_vectorized_producer.cpp
        src/infra/clickhouse_storage.cpp
        src/infra/clickhouse/order_book_storage.cpp
        src/infra/clickhouse/clickhouse_liquidity_curve_storage.cpp
        src/infra/clickhouse/clickhouse_vector_segment_storage.cpp
        src/infra/clickhouse/clickhouse_vector_clearing_storage.cpp
        src/infra/clickhouse/ch_snapshot_storage.cpp
        # headers-only (IDE visibility)
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
        ${MARKET_DATA_PQXX_TARGET}
)

target_compile_options(market_data PRIVATE -Wall -Wextra -Wpedantic)

if (BUILD_TESTING)
    add_executable(market_data_uc_test
            tests/market_data_uc_test.cpp
            src/app/market_data_uc.cpp
            src/app/update_order_book_uc.cpp
            src/app/compute_market_data.cpp
            src/app/curve_to_levels.cpp                       # F-05A (T-F05A-205)
            src/domain/vectorize.cpp                          # F-05A
            src/transport/mappers/order_book.cpp
            src/transport/mappers/vectorized_liquidity.cpp    # F-05A
            src/domain/entities/order_book.cpp
            src/infra/clickhouse/clickhouse_liquidity_curve_storage.cpp
            src/infra/postgres/pg_market_data_config.cpp
    )

    target_include_directories(market_data_uc_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_link_libraries(market_data_uc_test PRIVATE
            cex_common
            contracts_proto
            Boost::thread
            clickhouse-cpp-lib
            ${MARKET_DATA_PQXX_TARGET}
    )

    target_compile_options(market_data_uc_test PRIVATE -Wall -Wextra -Wpedantic)

    add_test(NAME market_data_uc_test COMMAND market_data_uc_test)

    # F-05A (T-F05A-201/202/203): vectorization domain core (Decimal-only, Eigen-free).
    add_executable(market_data_vectorize_test
            tests/vectorize_test.cpp
            src/domain/vectorize.cpp
    )
    target_include_directories(market_data_vectorize_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(market_data_vectorize_test PRIVATE
            cex_common
    )
    target_compile_options(market_data_vectorize_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME market_data_vectorize_test COMMAND market_data_vectorize_test)

    # F-05A (T-F05A-205): curve→levels + proto mapping (needs contracts_proto).
    add_executable(market_data_vectorize_wiring_test
            tests/vectorize_wiring_test.cpp
            src/app/curve_to_levels.cpp
            src/domain/vectorize.cpp
            src/transport/mappers/vectorized_liquidity.cpp
    )
    target_include_directories(market_data_vectorize_wiring_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(market_data_vectorize_wiring_test PRIVATE
            cex_common
            contracts_proto
    )
    target_compile_options(market_data_vectorize_wiring_test PRIVATE -Wall -Wextra)
    add_test(NAME market_data_vectorize_wiring_test COMMAND market_data_vectorize_wiring_test)

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

    # F-05 integration tests I1-I8
    if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/market_data_integration_test.cpp)
        add_executable(market_data_integration_test
                tests/market_data_integration_test.cpp
                src/app/market_data_uc.cpp
                src/app/update_order_book_uc.cpp
                src/app/compute_market_data.cpp
                src/app/curve_to_levels.cpp                       # F-05A (T-F05A-205)
                src/domain/vectorize.cpp                          # F-05A
                src/domain/entities/order_book.cpp
                src/transport/mappers/order_book.cpp
                src/transport/mappers/vectorized_liquidity.cpp    # F-05A
                src/infra/clickhouse/clickhouse_liquidity_curve_storage.cpp
                src/infra/postgres/pg_market_data_config.cpp
        )
        target_include_directories(market_data_integration_test PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src
        )
        target_link_libraries(market_data_integration_test PRIVATE
                cex_common
                contracts_proto
                Boost::thread
                clickhouse-cpp-lib
                ${MARKET_DATA_PQXX_TARGET}
        )
        target_compile_options(market_data_integration_test PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME market_data_integration_test COMMAND market_data_integration_test)
    endif ()

    # F-05 unit tests: ComputeMarketData + ComputeEffectiveSpread (U1-U10)
    if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/compute_market_data_test.cpp)
        add_executable(compute_market_data_test
                tests/compute_market_data_test.cpp
                src/app/compute_market_data.cpp
                src/domain/entities/order_book.cpp
        )
        target_include_directories(compute_market_data_test PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src
        )
        target_link_libraries(compute_market_data_test PRIVATE
                cex_common
                contracts_proto
                Boost::thread
        )
        target_compile_options(compute_market_data_test PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME compute_market_data_test COMMAND compute_market_data_test)
    endif ()
endif ()
