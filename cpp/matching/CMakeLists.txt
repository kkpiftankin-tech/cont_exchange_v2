cmake_minimum_required(VERSION 3.24)
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

# F-05A (T-F05A-301 / ADR-048): OSQP QP solver backend via FetchContent (pinned).
# ADR-048 §7: OSQP добавляется синхронно в matching CMake + docker/Dockerfile.service
# + .github/workflows/cpp-build.yml (урок F-09/libabsl). Eigen (выше) собирает W/D;
# OSQP решает QP. Статическая линковка → бинарь самодостаточен для runtime-стадии.
include(FetchContent)
set(OSQP_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
FetchContent_Declare(osqp
  GIT_REPOSITORY https://github.com/osqp/osqp.git
  GIT_TAG        v0.6.3
)
FetchContent_MakeAvailable(osqp)
# 0.6.x экспортирует статическую цель osqpstatic (namespaced или нет — берём что есть).
if(TARGET osqp::osqpstatic)
  set(MATCHING_OSQP_TARGET osqp::osqpstatic)
elseif(TARGET osqpstatic)
  set(MATCHING_OSQP_TARGET osqpstatic)
elseif(TARGET osqp::osqp)
  set(MATCHING_OSQP_TARGET osqp::osqp)
else()
  set(MATCHING_OSQP_TARGET osqp)
endif()

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
  src/app/execution_planner.cpp
  src/app/external_venue_filter.cpp
  src/app/hedge_execution_intents_publisher.cpp
  src/app/hedge_trigger_policy.cpp
  src/app/matching_loop.cpp
  src/app/planner_inputs_cache.cpp
  src/app/position_snapshot_calculator.cpp
  src/app/run_batch_uc.cpp
  src/app/solver_metrics.cpp
  src/app/solve_grouped_batch_use_case.cpp
  src/app/combo_external_routing.cpp
  src/domain/solver_impl.cpp
  src/domain/batch_result_to_fill_events.cpp
  src/domain/multileg_feasible_caps.cpp
  src/domain/grouped_solver_bisection.cpp
  src/domain/child_graph_transitions.cpp
  src/domain/constraint_evaluator.cpp
  src/domain/trigger_condition.cpp
  src/domain/vector_qp_solver.cpp
  src/infra/osqp_backend.cpp
  src/infra/market_data/market_data_client.cpp
  src/infra/postgres/decimal_conversion.cpp
  src/infra/postgres/postgres_flow_order_repository.cpp
  src/infra/postgres/postgres_solver_config_repository.cpp
  src/infra/postgres_execution_groups_repository.cpp
  src/infra/active_grouped_orders_loader.cpp
  src/infra/postgres_child_graph_repository.cpp
  src/infra/postgres_combo_compensation_repository.cpp
  src/infra/kafka/batch_outputs_producer.cpp
  src/infra/kafka/execution_intents_producer.cpp
  src/infra/execution_groups_producer.cpp
  src/transport/grpc_isolation_matching_service.cpp
  src/transport/grpc_compensation_service.cpp
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
  ${MATCHING_OSQP_TARGET}
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

  # F-09 (T-F09-043): grouped solver feasible caps (pure domain, Decimal only).
  add_executable(matching_multileg_feasible_caps_test
    tests/domain/multileg_feasible_caps_test.cpp
    src/domain/multileg_feasible_caps.cpp
  )
  target_include_directories(matching_multileg_feasible_caps_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(matching_multileg_feasible_caps_test PRIVATE
    cex_common
  )
  target_compile_options(matching_multileg_feasible_caps_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_multileg_feasible_caps_test COMMAND matching_multileg_feasible_caps_test)

  # F-09 (T-F09-044): grouped vector solver (strict/scalable/best_effort).
  add_executable(matching_grouped_solver_test
    tests/domain/grouped_solver_test.cpp
    src/domain/grouped_solver_bisection.cpp
    src/domain/multileg_feasible_caps.cpp
  )
  target_include_directories(matching_grouped_solver_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(matching_grouped_solver_test PRIVATE
    cex_common
  )
  target_compile_options(matching_grouped_solver_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_grouped_solver_test COMMAND matching_grouped_solver_test)

  # F-05A (T-F05A-302/303): QP vector-clearing solver domain core (Eigen-only,
  # fake QP backend). Реальный OSQP-backend — T-F05A-301 (следующий PR).
  add_executable(matching_vector_qp_solver_test
    tests/domain/vector_qp_solver_test.cpp
    src/domain/vector_qp_solver.cpp
  )
  target_include_directories(matching_vector_qp_solver_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(matching_vector_qp_solver_test PRIVATE
    cex_common
    Eigen3::Eigen
  )
  target_compile_options(matching_vector_qp_solver_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_vector_qp_solver_test COMMAND matching_vector_qp_solver_test)

  # F-05A (T-F05A-301): OSQP-backend — реальный QP на known-solution задачах +
  # end-to-end через VectorQpSolver. Линкует OSQP-таргет (для osqp.h) + Eigen.
  add_executable(matching_osqp_backend_test
    tests/infra/osqp_backend_test.cpp
    src/infra/osqp_backend.cpp
    src/domain/vector_qp_solver.cpp
  )
  target_include_directories(matching_osqp_backend_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(matching_osqp_backend_test PRIVATE
    cex_common
    Eigen3::Eigen
    ${MATCHING_OSQP_TARGET}
  )
  target_compile_options(matching_osqp_backend_test PRIVATE -Wall -Wextra)
  add_test(NAME matching_osqp_backend_test COMMAND matching_osqp_backend_test)

  # F-09 (T-F09-045): child graph transitions (OCO cancel / bracket resize).
  add_executable(matching_child_graph_transitions_test
    tests/domain/child_graph_transitions_test.cpp
    src/domain/child_graph_transitions.cpp
    src/domain/trigger_condition.cpp
  )
  target_include_directories(matching_child_graph_transitions_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(matching_child_graph_transitions_test PRIVATE
    cex_common
  )
  target_compile_options(matching_child_graph_transitions_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_child_graph_transitions_test COMMAND matching_child_graph_transitions_test)

  # F-09 (T-F09-094, MVP-3): group constraint evaluator (spread/factor/notional).
  add_executable(matching_constraint_evaluator_test
    tests/domain/constraint_evaluator_test.cpp
    src/domain/constraint_evaluator.cpp
  )
  target_include_directories(matching_constraint_evaluator_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(matching_constraint_evaluator_test PRIVATE cex_common)
  target_compile_options(matching_constraint_evaluator_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_constraint_evaluator_test COMMAND matching_constraint_evaluator_test)

  # F-09 (MVP-4.1): trigger condition evaluator (conditional links).
  add_executable(matching_trigger_condition_test
    tests/domain/trigger_condition_test.cpp
    src/domain/trigger_condition.cpp
  )
  target_include_directories(matching_trigger_condition_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(matching_trigger_condition_test PRIVATE cex_common)
  target_compile_options(matching_trigger_condition_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_trigger_condition_test COMMAND matching_trigger_condition_test)

  # F-09 MVP-6: ComputeReversals перенесён в cpp/common (ADR-040 §4).
  # Тест → cex_common_compensation_reversal_test (cpp/common/CMakeLists.txt).

  # F-09 (T-F09-045): SolveGroupedBatch use case (solve loop over active groups).
  add_executable(matching_solve_grouped_batch_test
    tests/app/solve_grouped_batch_use_case_test.cpp
    src/app/solve_grouped_batch_use_case.cpp
    src/domain/grouped_solver_bisection.cpp
    src/domain/multileg_feasible_caps.cpp
    src/domain/constraint_evaluator.cpp
  )
  target_include_directories(matching_solve_grouped_batch_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(matching_solve_grouped_batch_test PRIVATE
    cex_common
  )
  target_compile_options(matching_solve_grouped_batch_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_solve_grouped_batch_test COMMAND matching_solve_grouped_batch_test)

  # F-09 (T-F09-046): ExecutionGroup producer (pure BuildExecutionGroup mapping).
  add_executable(matching_execution_groups_producer_test
    tests/infra/execution_groups_producer_test.cpp
    src/infra/execution_groups_producer.cpp
    src/domain/grouped_solver_bisection.cpp
    src/domain/multileg_feasible_caps.cpp
  )
  target_include_directories(matching_execution_groups_producer_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(matching_execution_groups_producer_test PRIVATE
    cex_common
    contracts_proto
  )
  target_compile_options(matching_execution_groups_producer_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_execution_groups_producer_test COMMAND matching_execution_groups_producer_test)

  # F-09 (T-F09-047): execution_groups repository (PG integration; skips w/o TEST_PG_DSN).
  add_executable(matching_execution_groups_repo_test
    tests/infra/postgres_execution_groups_repository_test.cpp
    src/infra/postgres_execution_groups_repository.cpp
  )
  target_include_directories(matching_execution_groups_repo_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(matching_execution_groups_repo_test PRIVATE
    cex_common
    contracts_proto
    ${MATCHING_PQXX_TARGET}
    PostgreSQL::PostgreSQL
  )
  target_compile_options(matching_execution_groups_repo_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_execution_groups_repo_test COMMAND matching_execution_groups_repo_test)

  # F-09 (T-F09-048): active grouped orders loader (PG → MultiLegVectorOrder).
  add_executable(matching_active_groups_loader_test
    tests/infra/active_grouped_orders_loader_test.cpp
    src/infra/active_grouped_orders_loader.cpp
    src/infra/postgres/decimal_conversion.cpp
  )
  target_include_directories(matching_active_groups_loader_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(matching_active_groups_loader_test PRIVATE
    cex_common
    ${MATCHING_PQXX_TARGET}
    PostgreSQL::PostgreSQL
  )
  target_compile_options(matching_active_groups_loader_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_active_groups_loader_test COMMAND matching_active_groups_loader_test)

  # F-09 (MVP-4, ADR-038): child-graph repository — OCO/bracket leg transitions (PG).
  add_executable(matching_child_graph_repo_test
    tests/infra/postgres_child_graph_repository_test.cpp
    src/infra/postgres_child_graph_repository.cpp
    src/domain/child_graph_transitions.cpp
    src/domain/trigger_condition.cpp
    src/infra/postgres/decimal_conversion.cpp
  )
  target_include_directories(matching_child_graph_repo_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(matching_child_graph_repo_test PRIVATE
    cex_common
    ${MATCHING_PQXX_TARGET}
    PostgreSQL::PostgreSQL
  )
  target_compile_options(matching_child_graph_repo_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_child_graph_repo_test COMMAND matching_child_graph_repo_test)

  # F-09 MVP-5 (ADR-037): combo compensation repository (PG integration).
  add_executable(matching_combo_compensation_repo_test
    tests/infra/postgres_combo_compensation_repository_test.cpp
    src/infra/postgres_combo_compensation_repository.cpp
    src/infra/postgres/decimal_conversion.cpp
  )
  target_include_directories(matching_combo_compensation_repo_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(matching_combo_compensation_repo_test PRIVATE
    cex_common
    ${MATCHING_PQXX_TARGET}
    PostgreSQL::PostgreSQL
  )
  target_compile_options(matching_combo_compensation_repo_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_combo_compensation_repo_test COMMAND matching_combo_compensation_repo_test)

  # F-09 MVP-6 slice 3b (T-F09-065, ADR-040): CompensationService gRPC (PG integration).
  add_executable(matching_grpc_compensation_service_test
    tests/transport/grpc_compensation_service_test.cpp
    src/transport/grpc_compensation_service.cpp
    src/infra/postgres_combo_compensation_repository.cpp
    src/infra/postgres/decimal_conversion.cpp
  )
  target_include_directories(matching_grpc_compensation_service_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(matching_grpc_compensation_service_test PRIVATE
    cex_common
    contracts_proto
    ${MATCHING_PQXX_TARGET}
    PostgreSQL::PostgreSQL
  )
  target_compile_options(matching_grpc_compensation_service_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_grpc_compensation_service_test COMMAND matching_grpc_compensation_service_test)

  # F-09 MVP-5 (ADR-037): combo external-leg routing (split + build ExecutionIntent).
  add_executable(matching_combo_external_routing_test
    tests/app/combo_external_routing_test.cpp
    src/app/combo_external_routing.cpp
  )
  target_include_directories(matching_combo_external_routing_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(matching_combo_external_routing_test PRIVATE cex_common contracts_proto)
  target_compile_options(matching_combo_external_routing_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_combo_external_routing_test COMMAND matching_combo_external_routing_test)

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

  # F-12 DoD-2 (PR-F12-15): multi-venue routing planner unit tests.
  add_executable(matching_execution_planner_test
    tests/app/execution_planner_test.cpp
    src/app/execution_planner.cpp
  )

  target_include_directories(matching_execution_planner_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(matching_execution_planner_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(matching_execution_planner_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_execution_planner_test COMMAND matching_execution_planner_test)

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

  # IN-012 closed-form replication tests (ADR-035 / solver-foundation.md).
  add_executable(matching_continuous_market_replica_test
    tests/domain/continuous_market_replica_test.cpp
    src/domain/solver_impl.cpp
  )
  target_include_directories(matching_continuous_market_replica_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(matching_continuous_market_replica_test PRIVATE
    cex_common
    contracts_proto
  )
  target_compile_options(matching_continuous_market_replica_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME matching_continuous_market_replica_test COMMAND matching_continuous_market_replica_test)

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
