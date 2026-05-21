cmake_minimum_required(VERSION 3.24)
project(backtest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

find_package(GTest CONFIG QUIET)

if (NOT TARGET GTest::gtest)
    FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG release-1.12.1
            GIT_SHALLOW TRUE
    )

    # Отключаем установку gtest вместе с проектом
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(googletest)

endif ()

# Включаем тесты для gtest discovery
include(GoogleTest)

if (POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif ()

set(Boost_NO_BOOST_CMAKE ON)
set(Boost_NO_WARN_NEW_VERSIONS ON)
find_package(Boost REQUIRED COMPONENTS thread)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(RDKAFKA REQUIRED IMPORTED_TARGET rdkafka++)
find_package(PostgreSQL REQUIRED)
find_package(Protobuf REQUIRED)

set(BACKTEST_JSON_TARGET "")
if (TARGET nlohmann_json::nlohmann_json)
    set(BACKTEST_JSON_TARGET nlohmann_json::nlohmann_json)
else ()
    find_package(nlohmann_json CONFIG QUIET)
    if (TARGET nlohmann_json::nlohmann_json)
        set(BACKTEST_JSON_TARGET nlohmann_json::nlohmann_json)
    else ()
        pkg_check_modules(NLOHMANN_JSON QUIET IMPORTED_TARGET nlohmann_json)
        if (TARGET PkgConfig::NLOHMANN_JSON)
            set(BACKTEST_JSON_TARGET PkgConfig::NLOHMANN_JSON)
        else ()
            message(WARNING "nlohmann_json not found; targets that include create_replay_session_uc.cpp may fail to build in this environment")
        endif ()
    endif ()
endif ()

find_package(libpqxx CONFIG QUIET)
if (libpqxx_FOUND)
    if (TARGET libpqxx::pqxx)
        set(BACKTEST_PQXX_TARGET libpqxx::pqxx)
    elseif (TARGET pqxx)
        set(BACKTEST_PQXX_TARGET pqxx)
    else ()
        message(FATAL_ERROR "libpqxx was found but no known CMake target is available")
    endif ()
else ()
    pkg_check_modules(LIBPQXX REQUIRED IMPORTED_TARGET libpqxx)
    set(BACKTEST_PQXX_TARGET PkgConfig::LIBPQXX)
endif ()

set(BACKTEST_LIB_SOURCES
        src/app/backtest_uc.cpp
        src/app/metrics.cpp
        src/app/venue_replay_uc.cpp
        src/app/lob_fob_replay_uc.cpp
        src/app/replay_metrics.cpp
        src/app/quality_metrics.cpp
        src/app/quality_metrics_uc.cpp
        src/app/historical_batch_loader_uc.cpp
        src/app/replay_config_snapshot.cpp
        src/app/read_replay_sessions_uc.cpp
        src/app/replay_runtime_metrics.cpp
        src/app/replay_step_journal.cpp
        src/app/restore_state_uc.cpp
        src/app/check_replay_determinism_uc.cpp
        src/app/compare_replay_sessions_uc.cpp
        src/app/run_replay_audit_batch_uc.cpp
        src/app/cancel_replay_session_uc.cpp
        src/app/run_replay_session_uc.cpp
        src/app/retry_replay_session_uc.cpp
        src/app/shadow_namespace_uc.cpp
        src/app/replay_batch_input.cpp
        src/infra/clickhouse_historical_queries.cpp
        src/infra/kafka_consumer.cpp
        src/infra/kafka_replay_event_publisher.cpp
        src/infra/replay_commands_kafka_consumer.cpp
        src/infra/venue_kafka_consumer.cpp
        src/infra/clickhouse_storage.cpp
        src/infra/grpc_replay_batch_executor.cpp
        src/infra/in_memory_shadow_ledger.cpp
        src/infra/postgres/postgres_replay_session_repository.cpp
        src/infra/postgres/postgres_replay_config_repository.cpp
        src/infra/postgres/postgres_rbac_repository.cpp
        ${CMAKE_SOURCE_DIR}/cpp/ledger/src/app/ledger_uc.cpp
        # Venues domain sources needed by F11-BACKTEST-2 replay pipeline.
        ${CMAKE_SOURCE_DIR}/cpp/venues/src/domain/depth_canonicalizer.cpp
        ${CMAKE_SOURCE_DIR}/cpp/venues/src/domain/depth_curve_builder.cpp
)

if (BACKTEST_JSON_TARGET)
    list(APPEND BACKTEST_LIB_SOURCES
            src/app/create_replay_session_uc.cpp
            src/infra/http/retry_session_handler.cpp
    )
endif ()

add_library(backtest_lib STATIC
        ${BACKTEST_LIB_SOURCES}
)

target_include_directories(backtest_lib PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/cpp/ledger/src
        ${CMAKE_SOURCE_DIR}/cpp/venues/src
        ${CMAKE_SOURCE_DIR}/third_party/crow/include
        ${CMAKE_BINARY_DIR}
        ${CMAKE_BINARY_DIR}/contracts/gen
        ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(backtest_lib PRIVATE
        cex_common
        contracts_proto
        Boost::thread
        PkgConfig::CURL
        ${BACKTEST_JSON_TARGET}
        ${BACKTEST_PQXX_TARGET}
        PostgreSQL::PostgreSQL
)

target_compile_definitions(backtest_lib PRIVATE CROW_USE_BOOST)
target_compile_options(backtest_lib PRIVATE -Wall -Wextra -Wpedantic)

add_executable(backtest
        src/main.cpp
)

target_include_directories(backtest PRIVATE
        ${CMAKE_SOURCE_DIR}/cpp/common/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}
        ${CMAKE_BINARY_DIR}/contracts/gen
)

target_compile_definitions(backtest PRIVATE CROW_USE_BOOST)
target_link_libraries(backtest PRIVATE
        backtest_lib
)

add_executable(backtest_f15_load_harness
        tests/f15_load_harness.cpp
)

target_include_directories(backtest_f15_load_harness PRIVATE
        ${CMAKE_SOURCE_DIR}/cpp/common/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}
        ${CMAKE_BINARY_DIR}/contracts/gen
)

target_compile_definitions(backtest_f15_load_harness PRIVATE CROW_USE_BOOST)
target_link_libraries(backtest_f15_load_harness PRIVATE
        backtest_lib
        contracts_proto
        PkgConfig::RDKAFKA
        ${BACKTEST_PQXX_TARGET}
        PostgreSQL::PostgreSQL
)

if (BUILD_TESTING)
    add_executable(backtest_uc_test
            tests/backtest_uc_test.cpp
    )

    target_include_directories(backtest_uc_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_uc_test PRIVATE
            backtest_lib
    )

    target_compile_options(backtest_uc_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_uc_test COMMAND backtest_uc_test)

    add_executable(backtest_metrics_test
            tests/metrics_test.cpp
    )

    target_include_directories(backtest_metrics_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_metrics_test PRIVATE
            backtest_lib
    )

    target_compile_options(backtest_metrics_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_metrics_test COMMAND backtest_metrics_test)

    add_executable(backtest_venue_replay_uc_test
            tests/venue_replay_uc_test.cpp
    )

    target_include_directories(backtest_venue_replay_uc_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_venue_replay_uc_test PRIVATE
            backtest_lib
    )

    target_compile_options(backtest_venue_replay_uc_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_venue_replay_uc_test COMMAND backtest_venue_replay_uc_test)

    add_executable(backtest_clickhouse_storage_test
            tests/clickhouse_storage_test.cpp
    )

    target_include_directories(backtest_clickhouse_storage_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_clickhouse_storage_test PRIVATE
            backtest_lib
    )

    target_compile_options(backtest_clickhouse_storage_test PRIVATE -Wall -Wextra -Wpedantic)

    add_test(NAME backtest_clickhouse_storage_test COMMAND backtest_clickhouse_storage_test)

    add_executable(backtest_clickhouse_historical_queries_test
            tests/clickhouse_historical_queries_test.cpp
    )

    target_include_directories(backtest_clickhouse_historical_queries_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_clickhouse_historical_queries_test PRIVATE
            backtest_lib
    )

    target_compile_options(backtest_clickhouse_historical_queries_test PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_clickhouse_historical_queries_test
            COMMAND backtest_clickhouse_historical_queries_test)

    add_executable(backtest_run_replay_audit_batch_test
            tests/run_replay_audit_batch_test.cpp
    )

    target_include_directories(backtest_run_replay_audit_batch_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_run_replay_audit_batch_test PRIVATE
            backtest_lib
    )

    target_compile_options(backtest_run_replay_audit_batch_test PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_run_replay_audit_batch_test
            COMMAND backtest_run_replay_audit_batch_test)

    add_executable(backtest_lob_fob_replay_test
            tests/lob_fob_replay_test.cpp
    )

    target_include_directories(backtest_lob_fob_replay_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/venues/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_lob_fob_replay_test PRIVATE
            backtest_lib
    )

    target_compile_options(backtest_lob_fob_replay_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_lob_fob_replay_test COMMAND backtest_lob_fob_replay_test)

    add_executable(backtest_quality_metrics_test
            tests/quality_metrics_test.cpp
    )

    target_include_directories(backtest_quality_metrics_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_quality_metrics_test PRIVATE
            backtest_lib
    )

    target_compile_options(backtest_quality_metrics_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_quality_metrics_test COMMAND backtest_quality_metrics_test)

    add_executable(backtest_historical_batch_loader_test
            tests/historical_batch_loader_test.cpp
    )

    target_include_directories(backtest_historical_batch_loader_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_historical_batch_loader_test PRIVATE
            backtest_lib
    )

    target_compile_options(backtest_historical_batch_loader_test PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_historical_batch_loader_test
            COMMAND backtest_historical_batch_loader_test)

    add_executable(backtest_replay_config_snapshot_test
            tests/replay_config_snapshot_test.cpp
    )

    target_include_directories(backtest_replay_config_snapshot_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_replay_config_snapshot_test PRIVATE
            backtest_lib
    )

    target_compile_options(backtest_replay_config_snapshot_test PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_replay_config_snapshot_test
            COMMAND backtest_replay_config_snapshot_test)

    add_executable(backtest_create_replay_session_test
            tests/create_replay_session_test.cpp
    )

    target_include_directories(backtest_create_replay_session_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_create_replay_session_test PRIVATE
            backtest_lib
            ${BACKTEST_JSON_TARGET}
    )

    target_compile_options(backtest_create_replay_session_test PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_create_replay_session_test
            COMMAND backtest_create_replay_session_test)

    add_executable(backtest_read_replay_sessions_test
            tests/read_replay_sessions_test.cpp
    )

    target_include_directories(backtest_read_replay_sessions_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_read_replay_sessions_test PRIVATE
            backtest_lib
    )

    target_compile_options(backtest_read_replay_sessions_test PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_read_replay_sessions_test
            COMMAND backtest_read_replay_sessions_test)

    add_executable(backtest_shadow_namespace_test
            tests/shadow_namespace_test.cpp
    )

    target_include_directories(backtest_shadow_namespace_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/ledger/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_shadow_namespace_test PRIVATE
            backtest_lib
    )

    target_compile_options(backtest_shadow_namespace_test PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_shadow_namespace_test
            COMMAND backtest_shadow_namespace_test)

    add_executable(backtest_run_replay_session_test
            tests/run_replay_session_test.cpp
    )

    target_include_directories(backtest_run_replay_session_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/ledger/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_run_replay_session_test PRIVATE
            backtest_lib
    )

    target_compile_options(backtest_run_replay_session_test PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_run_replay_session_test
            COMMAND backtest_run_replay_session_test)

    add_executable(backtest_grpc_replay_batch_executor_test
            tests/grpc_replay_batch_executor_test.cpp
    )

    target_include_directories(backtest_grpc_replay_batch_executor_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/ledger/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_grpc_replay_batch_executor_test PRIVATE
            backtest_lib
    )

    target_compile_options(backtest_grpc_replay_batch_executor_test PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_grpc_replay_batch_executor_test
            COMMAND backtest_grpc_replay_batch_executor_test)

    add_executable(backtest_shadow_ledger_apply_test
            tests/shadow_ledger_apply_test.cpp
    )

    target_include_directories(backtest_shadow_ledger_apply_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/ledger/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_shadow_ledger_apply_test PRIVATE
            backtest_lib
    )

    target_compile_options(backtest_shadow_ledger_apply_test PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_shadow_ledger_apply_test
            COMMAND backtest_shadow_ledger_apply_test)

    add_executable(backtest_replay_step_journal_test
            tests/replay_step_journal_test.cpp
            src/app/replay_step_journal.cpp
    )

    target_include_directories(backtest_replay_step_journal_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_link_libraries(backtest_replay_step_journal_test PRIVATE
            cex_common
            contracts_proto
    )

    target_compile_options(backtest_replay_step_journal_test PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_replay_step_journal_test
            COMMAND backtest_replay_step_journal_test)

    # F15-BACKTEST-7: ReplaySummary aggregation tests (avg_vwap, sharpe,
    # drawdown, processed/failed counts, idempotent SaveSummary contract).
    add_executable(backtest_replay_summary_aggregation_test
            tests/replay_summary_aggregation_test.cpp
            src/app/replay_step_journal.cpp
    )

    target_include_directories(backtest_replay_summary_aggregation_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_link_libraries(backtest_replay_summary_aggregation_test PRIVATE
            cex_common
            contracts_proto
    )

    target_compile_options(backtest_replay_summary_aggregation_test PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_replay_summary_aggregation_test
            COMMAND backtest_replay_summary_aggregation_test)

    add_executable(backtest_restore_state_test
            tests/restore_state_test.cpp
    )

    target_include_directories(backtest_restore_state_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_restore_state_test PRIVATE
            backtest_lib
    )

    target_compile_options(backtest_restore_state_test PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_restore_state_test
            COMMAND backtest_restore_state_test)

    # F15-BACKTEST-6: AgentLog writer (JSONEachRow builder) tests.
    add_executable(backtest_agent_log_writer_test
            tests/agent_log_writer_test.cpp
    )

    target_include_directories(backtest_agent_log_writer_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_agent_log_writer_test PRIVATE
            backtest_lib
    )

    target_compile_options(backtest_agent_log_writer_test PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_agent_log_writer_test
            COMMAND backtest_agent_log_writer_test)

    add_executable(backtest_compare_replay_sessions_test
            tests/compare_replay_sessions_test.cpp
    )

    target_include_directories(backtest_compare_replay_sessions_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_compare_replay_sessions_test PRIVATE
            backtest_lib
    )

    target_compile_options(backtest_compare_replay_sessions_test PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_compare_replay_sessions_test
            COMMAND backtest_compare_replay_sessions_test)

    add_executable(backtest_check_replay_determinism_test
            tests/check_replay_determinism_test.cpp
    )

    target_include_directories(backtest_check_replay_determinism_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_check_replay_determinism_test PRIVATE
            backtest_lib
    )

    target_compile_options(backtest_check_replay_determinism_test PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_check_replay_determinism_test
            COMMAND backtest_check_replay_determinism_test)

    add_executable(backtest_replay_runtime_metrics_test
            tests/replay_runtime_metrics_test.cpp
    )

    target_include_directories(backtest_replay_runtime_metrics_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_replay_runtime_metrics_test PRIVATE
            backtest_lib
    )

    target_compile_options(backtest_replay_runtime_metrics_test PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_replay_runtime_metrics_test
            COMMAND backtest_replay_runtime_metrics_test)

    # ============================================
    # ТЕСТЫ, КОТОРЫЕ ТРЕБУЮТ GTEST (ДОБАВЛЕНА ЛИНКОВКА)
    # ============================================

    add_executable(backtest_kafka_replay_event_publisher_test
            tests/kafka_replay_event_publisher_test.cpp
    )

    target_include_directories(backtest_kafka_replay_event_publisher_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_kafka_replay_event_publisher_test PRIVATE
            backtest_lib
            GTest::gtest_main
            GTest::gtest
    )

    target_compile_options(backtest_kafka_replay_event_publisher_test PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)

    gtest_discover_tests(backtest_kafka_replay_event_publisher_test)
    add_test(NAME backtest_kafka_replay_event_publisher_test
            COMMAND backtest_kafka_replay_event_publisher_test)

    # F15-COBE-160: RetryReplaySession use case unit tests
    add_executable(backtest_retry_replay_session_uc_test
            tests/app/retry_replay_session_uc_test.cpp
    )

    target_include_directories(backtest_retry_replay_session_uc_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_retry_replay_session_uc_test PRIVATE
            backtest_lib
            GTest::gtest_main
            GTest::gtest
    )

    target_compile_options(backtest_retry_replay_session_uc_test PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_retry_replay_session_uc_test
            COMMAND backtest_retry_replay_session_uc_test)

    # F15-COBE-160: RetryReplaySession integration tests with PostgreSQL
    add_executable(backtest_retry_replay_session_integration_test
            tests/infra/postgres_replay_session_repository_test.cpp
    )

    target_include_directories(backtest_retry_replay_session_integration_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_retry_replay_session_integration_test PRIVATE
            backtest_lib
            GTest::gtest_main
            GTest::gtest
    )

    target_compile_options(backtest_retry_replay_session_integration_test PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)

    gtest_discover_tests(backtest_retry_replay_session_integration_test)

    add_executable(backtest_cancel_replay_session_test
            tests/cancel_replay_session_test.cpp
    )

    target_include_directories(backtest_cancel_replay_session_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/cpp/common/include
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/contracts/gen
    )

    target_link_libraries(backtest_cancel_replay_session_test PRIVATE
            backtest_lib
    )

    target_compile_options(backtest_cancel_replay_session_test PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)

    add_test(NAME backtest_cancel_replay_session_test
            COMMAND backtest_cancel_replay_session_test)

endif ()
