cmake_minimum_required(VERSION 3.22)
project(matching LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

set(Boost_NO_BOOST_CMAKE ON)
set(Boost_NO_WARN_NEW_VERSIONS ON)
find_package(Boost REQUIRED COMPONENTS thread)

find_package(PostgreSQL REQUIRED)
find_package(Eigen3 REQUIRED)

find_package(libpqxx CONFIG QUIET)
if(libpqxx_FOUND)
  if(TARGET libpqxx::pqxx)
    set(MATCHING_PQXX_TARGET libpqxx::pqxx)
  elseif(TARGET pqxx)
    set(MATCHING_PQXX_TARGET pqxx)
  else()
    message(FATAL_ERROR "libpqxx was found but no known CMake target is available")
  endif()
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBPQXX REQUIRED IMPORTED_TARGET libpqxx)
  set(MATCHING_PQXX_TARGET PkgConfig::LIBPQXX)
endif()

add_executable(matching
  src/main.cpp
  src/app/execution_intent_builder.cpp
  src/app/external_venue_filter.cpp
  src/app/hedge_execution_intents_publisher.cpp
  src/app/hedge_trigger_policy.cpp
  src/app/matching_loop.cpp
  src/app/planner_inputs_cache.cpp
  src/app/position_snapshot_calculator.cpp
  src/app/run_batch_uc.cpp
  src/app/solver_metrics.cpp
  src/domain/solver_impl.cpp
  src/domain/batch_result_to_fill_events.cpp
  src/infra/market_data/market_data_client.cpp
  src/infra/postgres/decimal_conversion.cpp
  src/infra/postgres/postgres_flow_order_repository.cpp
  src/infra/postgres/postgres_solver_config_repository.cpp
  src/infra/kafka/batch_outputs_producer.cpp
  src/infra/kafka/execution_intents_producer.cpp
  src/transport/grpc_isolation_matching_service.cpp
)

target_include_directories(matching PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/crow/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(matching PRIVATE
  cex_common
  contracts_proto
  Boost::thread
  ${MATCHING_PQXX_TARGET}
  PostgreSQL::PostgreSQL
  Eigen3::Eigen
)

target_compile_definitions(matching PRIVATE CROW_USE_BOOST)
target_compile_options(matching PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
  add_executable(matching_domain_tests
    tests/domain/fill_math_test.cpp
  )

  target_include_directories(matching_domain_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(matching_domain_tests PRIVATE
    cex_common
  )

  target_compile_options(matching_domain_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_domain_tests COMMAND matching_domain_tests)

  add_executable(matching_br_to_fe_test
    tests/domain/br_to_fe_test.cpp
    src/domain/batch_result_to_fill_events.cpp
  )
  
  target_include_directories(matching_br_to_fe_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  
  target_link_libraries(matching_br_to_fe_test PRIVATE
    cex_common
    contracts_proto
  )
  
  target_compile_options(matching_br_to_fe_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_br_to_fe_test COMMAND matching_br_to_fe_test)

#   add_executable(matching_simulated_solver_test
#     tests/domain/simulated_solver_test.cpp
#     src/domain/simulated_solver.cpp
#   )

#   target_include_directories(matching_simulated_solver_test PRIVATE
#     ${CMAKE_CURRENT_SOURCE_DIR}/src
#   )

#   target_link_libraries(matching_simulated_solver_test PRIVATE
#     cex_common
#     contracts_proto
#   )

#   target_compile_options(matching_simulated_solver_test PRIVATE -Wall -Wextra -Wpedantic)
#   add_test(NAME matching_simulated_solver_test COMMAND matching_simulated_solver_test)

  add_executable(matching_app_tests
    tests/app/run_batch_uc_test.cpp
    src/app/execution_intent_builder.cpp
    src/app/external_venue_filter.cpp
    src/app/hedge_trigger_policy.cpp
    src/app/position_snapshot_calculator.cpp
    src/app/run_batch_uc.cpp
    src/app/solver_metrics.cpp
    src/domain/solver_impl.cpp
  )

  target_include_directories(matching_app_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(matching_app_tests PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(matching_app_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_app_tests COMMAND matching_app_tests)

  add_executable(matching_hedge_trigger_policy_test
    tests/app/hedge_trigger_policy_test.cpp
    src/app/hedge_trigger_policy.cpp
  )

  target_include_directories(matching_hedge_trigger_policy_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(matching_hedge_trigger_policy_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(matching_hedge_trigger_policy_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_hedge_trigger_policy_test COMMAND matching_hedge_trigger_policy_test)

  add_executable(matching_position_snapshot_test
    tests/app/position_snapshot_calculator_test.cpp
    src/app/position_snapshot_calculator.cpp
  )

  target_include_directories(matching_position_snapshot_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(matching_position_snapshot_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(matching_position_snapshot_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_position_snapshot_test COMMAND matching_position_snapshot_test)

  add_executable(matching_venue_filter_test
    tests/app/external_venue_filter_test.cpp
    src/app/external_venue_filter.cpp
  )

  target_include_directories(matching_venue_filter_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(matching_venue_filter_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(matching_venue_filter_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_venue_filter_test COMMAND matching_venue_filter_test)

  add_executable(matching_planner_inputs_test
    tests/app/planner_inputs_cache_test.cpp
    src/app/external_venue_filter.cpp
    src/app/planner_inputs_cache.cpp
  )

  target_include_directories(matching_planner_inputs_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(matching_planner_inputs_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(matching_planner_inputs_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_planner_inputs_test COMMAND matching_planner_inputs_test)

  add_executable(matching_f04_solver_u1_u10_test
    tests/domain/f04_solver_u1_u10_test.cpp
    src/domain/solver_impl.cpp
  )

  target_include_directories(matching_f04_solver_u1_u10_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(matching_f04_solver_u1_u10_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(matching_f04_solver_u1_u10_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_f04_solver_u1_u10_test COMMAND matching_f04_solver_u1_u10_test)

  add_executable(matching_f04_full_cycle_test
    tests/domain/f04_full_cycle_test.cpp
    src/app/execution_intent_builder.cpp
    src/app/hedge_trigger_policy.cpp
    src/app/position_snapshot_calculator.cpp
    src/app/run_batch_uc.cpp
    src/domain/solver_impl.cpp
    src/domain/batch_result_to_fill_events.cpp
  )

  target_include_directories(matching_f04_full_cycle_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(matching_f04_full_cycle_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(matching_f04_full_cycle_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_f04_full_cycle_test COMMAND matching_f04_full_cycle_test)

  add_executable(matching_f04_sla_test
    tests/domain/f04_sla_test.cpp
    src/app/execution_intent_builder.cpp
    src/app/hedge_trigger_policy.cpp
    src/app/position_snapshot_calculator.cpp
    src/app/run_batch_uc.cpp
    src/domain/solver_impl.cpp
  )

  target_include_directories(matching_f04_sla_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(matching_f04_sla_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(matching_f04_sla_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_f04_sla_test COMMAND matching_f04_sla_test)

  add_executable(matching_execution_intents_test
    tests/app/execution_intent_builder_test.cpp
    src/app/execution_intent_builder.cpp
    src/app/external_venue_filter.cpp
    src/app/hedge_execution_intents_publisher.cpp
    src/app/planner_inputs_cache.cpp
    src/infra/kafka/execution_intents_producer.cpp
  )

  target_include_directories(matching_execution_intents_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(matching_execution_intents_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(matching_execution_intents_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_execution_intents_test COMMAND matching_execution_intents_test)
endif()
