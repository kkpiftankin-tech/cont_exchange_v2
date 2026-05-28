cmake_minimum_required(VERSION 3.24)
project(venues LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

set(Boost_NO_BOOST_CMAKE ON)
set(Boost_NO_WARN_NEW_VERSIONS ON)
find_package(Boost REQUIRED COMPONENTS thread)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(LIBPQXX QUIET IMPORTED_TARGET libpqxx)

set(VENUES_JSON_TARGET "")
if (TARGET nlohmann_json::nlohmann_json)
  set(VENUES_JSON_TARGET nlohmann_json::nlohmann_json)
else()
  find_package(nlohmann_json CONFIG QUIET)
  if (TARGET nlohmann_json::nlohmann_json)
    set(VENUES_JSON_TARGET nlohmann_json::nlohmann_json)
  else()
    pkg_check_modules(NLOHMANN_JSON REQUIRED IMPORTED_TARGET nlohmann_json)
    set(VENUES_JSON_TARGET PkgConfig::NLOHMANN_JSON)
  endif()
endif()

add_executable(venues
  src/main.cpp
  src/app/execute_on_venue.cpp
  src/app/backtest_parity_check.cpp
  src/app/backtest_synthetic_scenarios.cpp
  src/app/venues_loop.cpp
  src/app/liquidity_curve_producer.cpp
  src/infra/simulated_venue_adapter.cpp
  src/infra/venue_sim_adapter.cpp
  src/infra/cex_local_lob_assembler.cpp
  src/infra/execution_report_producer.cpp
  src/infra/venue_observability_producer.cpp
  src/infra/cex_ws_rest_adapter.cpp
  src/infra/dex_amm_rpc_adapter.cpp
  src/infra/postgres_synthetic_order_repository.cpp
  src/infra/execution_intents_consumer.cpp
  src/infra/postgres_venue_config_repository.cpp
  src/infra/postgres_hedgeflow_repository.cpp
  src/infra/postgres_child_order_repository.cpp
  src/domain/depth_canonicalizer.cpp
  src/domain/depth_curve_builder.cpp
  src/domain/normalize_snapshot.cpp
  src/domain/snapshot_status.cpp
  src/domain/amm_pool_extractor.cpp
  src/domain/amm_virtual_lob.cpp
  src/domain/amm_direct_fob.cpp
  src/app/snapshot_producer.cpp
  src/infra/snapshot_clickhouse_writer.cpp
)

target_include_directories(venues PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/crow/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(venues PRIVATE
  cex_common
  contracts_proto
  Boost::thread
  PkgConfig::CURL
  ${VENUES_JSON_TARGET}
)

target_compile_definitions(venues PRIVATE CROW_USE_BOOST)

if (LIBPQXX_FOUND)
  target_link_libraries(venues PRIVATE PkgConfig::LIBPQXX)
  target_compile_definitions(venues PRIVATE CEX_VENUES_HAS_LIBPQXX=1)
endif()

target_compile_options(venues PRIVATE -Wall -Wextra -Wpedantic)

if (BUILD_TESTING)
  add_executable(venues_depth_canonicalizer_test
    tests/depth_canonicalizer_test.cpp
    src/domain/depth_canonicalizer.cpp
  )

  target_include_directories(venues_depth_canonicalizer_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(venues_depth_canonicalizer_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(venues_depth_canonicalizer_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME venues_depth_canonicalizer_test COMMAND venues_depth_canonicalizer_test)

  add_executable(venues_depth_curve_builder_test
    tests/depth_curve_builder_test.cpp
    src/domain/depth_curve_builder.cpp
  )

  target_include_directories(venues_depth_curve_builder_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(venues_depth_curve_builder_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(venues_depth_curve_builder_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME venues_depth_curve_builder_test COMMAND venues_depth_curve_builder_test)

  add_executable(venues_depth_curve_regularization_test
    tests/depth_curve_regularization_test.cpp
    src/domain/depth_curve_builder.cpp
  )

  target_include_directories(venues_depth_curve_regularization_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(venues_depth_curve_regularization_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(venues_depth_curve_regularization_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME venues_depth_curve_regularization_test COMMAND venues_depth_curve_regularization_test)

  add_executable(venues_depth_curve_dual_test
    tests/depth_curve_dual_test.cpp
    src/domain/depth_curve_builder.cpp
  )

  target_include_directories(venues_depth_curve_dual_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(venues_depth_curve_dual_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(venues_depth_curve_dual_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME venues_depth_curve_dual_test COMMAND venues_depth_curve_dual_test)

  add_executable(venues_simulated_venue_adapter_test
    tests/simulated_venue_adapter_test.cpp
    src/infra/simulated_venue_adapter.cpp
  )

  target_include_directories(venues_simulated_venue_adapter_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(venues_simulated_venue_adapter_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(venues_simulated_venue_adapter_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME venues_simulated_venue_adapter_test COMMAND venues_simulated_venue_adapter_test)

  add_executable(venues_cex_ws_rest_adapter_test
    tests/cex_ws_rest_adapter_test.cpp
    src/infra/cex_local_lob_assembler.cpp
    src/infra/cex_ws_rest_adapter.cpp
  )

  target_include_directories(venues_cex_ws_rest_adapter_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(venues_cex_ws_rest_adapter_test PRIVATE
    cex_common
    contracts_proto
    PkgConfig::CURL
    ${VENUES_JSON_TARGET}
  )

  target_compile_options(venues_cex_ws_rest_adapter_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME venues_cex_ws_rest_adapter_test COMMAND venues_cex_ws_rest_adapter_test)

  add_executable(venues_dex_amm_rpc_adapter_test
    tests/dex_amm_rpc_adapter_test.cpp
    src/infra/dex_amm_rpc_adapter.cpp
  )

  target_include_directories(venues_dex_amm_rpc_adapter_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(venues_dex_amm_rpc_adapter_test PRIVATE
    cex_common
    contracts_proto
    PkgConfig::CURL
    ${VENUES_JSON_TARGET}
  )

  target_compile_options(venues_dex_amm_rpc_adapter_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME venues_dex_amm_rpc_adapter_test COMMAND venues_dex_amm_rpc_adapter_test)

  add_executable(venues_venue_adapter_contract_test
    tests/venue_adapter_contract_test.cpp
    src/infra/simulated_venue_adapter.cpp
    src/infra/cex_local_lob_assembler.cpp
    src/infra/cex_ws_rest_adapter.cpp
    src/infra/dex_amm_rpc_adapter.cpp
  )

  target_include_directories(venues_venue_adapter_contract_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(venues_venue_adapter_contract_test PRIVATE
    cex_common
    contracts_proto
    PkgConfig::CURL
    ${VENUES_JSON_TARGET}
  )

  target_compile_options(venues_venue_adapter_contract_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME venues_venue_adapter_contract_test COMMAND venues_venue_adapter_contract_test)

  add_executable(venues_cex_local_lob_assembler_test
    tests/cex_local_lob_assembler_test.cpp
    src/infra/cex_local_lob_assembler.cpp
  )

  target_include_directories(venues_cex_local_lob_assembler_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(venues_cex_local_lob_assembler_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(venues_cex_local_lob_assembler_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME venues_cex_local_lob_assembler_test COMMAND venues_cex_local_lob_assembler_test)

  add_executable(venues_venue_observability_producer_test
    tests/venue_observability_producer_test.cpp
    src/infra/venue_observability_producer.cpp
  )

  target_include_directories(venues_venue_observability_producer_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(venues_venue_observability_producer_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(venues_venue_observability_producer_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME venues_venue_observability_producer_test COMMAND venues_venue_observability_producer_test)

  add_executable(venues_execution_report_producer_test
    tests/execution_report_producer_test.cpp
    src/infra/execution_report_producer.cpp
  )

  target_include_directories(venues_execution_report_producer_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(venues_execution_report_producer_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(venues_execution_report_producer_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  add_test(NAME venues_execution_report_producer_test COMMAND venues_execution_report_producer_test)

  add_executable(venues_normalize_snapshot_test
    tests/normalize_snapshot_test.cpp
    src/domain/normalize_snapshot.cpp
    src/domain/depth_canonicalizer.cpp
  )

  target_include_directories(venues_normalize_snapshot_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(venues_normalize_snapshot_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(venues_normalize_snapshot_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  add_test(NAME venues_normalize_snapshot_test COMMAND venues_normalize_snapshot_test)

  add_executable(venues_snapshot_status_test
    tests/snapshot_status_test.cpp
    src/domain/snapshot_status.cpp
  )

  target_include_directories(venues_snapshot_status_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(venues_snapshot_status_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(venues_snapshot_status_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  add_test(NAME venues_snapshot_status_test COMMAND venues_snapshot_status_test)

  add_executable(venues_snapshot_producer_test
    tests/snapshot_producer_test.cpp
    src/app/snapshot_producer.cpp
    src/domain/normalize_snapshot.cpp
    src/domain/depth_canonicalizer.cpp
    src/domain/snapshot_status.cpp
  )

  target_include_directories(venues_snapshot_producer_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(venues_snapshot_producer_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(venues_snapshot_producer_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  add_test(NAME venues_snapshot_producer_test COMMAND venues_snapshot_producer_test)

  add_executable(venues_liquidity_curve_producer_test
    tests/liquidity_curve_producer_test.cpp
    src/app/liquidity_curve_producer.cpp
    src/domain/depth_curve_builder.cpp
    src/domain/depth_canonicalizer.cpp
    src/domain/amm_direct_fob.cpp
  )

  target_include_directories(venues_liquidity_curve_producer_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(venues_liquidity_curve_producer_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(venues_liquidity_curve_producer_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  add_test(NAME venues_liquidity_curve_producer_test COMMAND venues_liquidity_curve_producer_test)

  add_executable(venues_execute_on_venue_test
    tests/execute_on_venue_test.cpp
    src/app/execute_on_venue.cpp
  )

  target_include_directories(venues_execute_on_venue_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(venues_execute_on_venue_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(venues_execute_on_venue_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  add_test(NAME venues_execute_on_venue_test COMMAND venues_execute_on_venue_test)

  # F-20 VenueSimulator core (T-F20-701 subset).
  add_executable(venues_venue_simulator_test
    tests/venue_simulator_test.cpp
    src/app/venue_simulator.cpp
  )

  target_include_directories(venues_venue_simulator_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(venues_venue_simulator_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(venues_venue_simulator_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  add_test(NAME venues_venue_simulator_test COMMAND venues_venue_simulator_test)

  add_executable(venues_amm_pool_extractor_test
    tests/amm_pool_extractor_test.cpp
    src/domain/amm_pool_extractor.cpp
  )

  target_include_directories(venues_amm_pool_extractor_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(venues_amm_pool_extractor_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(venues_amm_pool_extractor_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  add_test(NAME venues_amm_pool_extractor_test COMMAND venues_amm_pool_extractor_test)

  add_executable(venues_backtest_parity_check_test
    tests/backtest_parity_check_test.cpp
    src/app/backtest_parity_check.cpp
    src/app/execute_on_venue.cpp
    src/infra/venue_sim_adapter.cpp
    src/infra/simulated_venue_adapter.cpp
  )

  target_include_directories(venues_backtest_parity_check_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(venues_backtest_parity_check_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(venues_backtest_parity_check_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  add_test(NAME venues_backtest_parity_check_test COMMAND venues_backtest_parity_check_test)

  add_executable(venues_venue_sim_adapter_test
    tests/venue_sim_adapter_test.cpp
    src/infra/venue_sim_adapter.cpp
    src/infra/simulated_venue_adapter.cpp
    src/app/execute_on_venue.cpp
  )

  target_include_directories(venues_venue_sim_adapter_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(venues_venue_sim_adapter_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(venues_venue_sim_adapter_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  add_test(NAME venues_venue_sim_adapter_test COMMAND venues_venue_sim_adapter_test)

  add_executable(venues_backtest_synthetic_scenarios_test
    tests/backtest_synthetic_scenarios_test.cpp
    src/app/backtest_synthetic_scenarios.cpp
    src/app/execute_on_venue.cpp
    src/infra/venue_sim_adapter.cpp
    src/infra/simulated_venue_adapter.cpp
  )

  target_include_directories(venues_backtest_synthetic_scenarios_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(venues_backtest_synthetic_scenarios_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(venues_backtest_synthetic_scenarios_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  add_test(NAME venues_backtest_synthetic_scenarios_test COMMAND venues_backtest_synthetic_scenarios_test)

  add_executable(venues_amm_virtual_lob_test
    tests/amm_virtual_lob_test.cpp
    src/domain/amm_virtual_lob.cpp
    src/domain/amm_pool_extractor.cpp
  )

  target_include_directories(venues_amm_virtual_lob_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(venues_amm_virtual_lob_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(venues_amm_virtual_lob_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  add_test(NAME venues_amm_virtual_lob_test COMMAND venues_amm_virtual_lob_test)
endif()
