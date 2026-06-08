cmake_minimum_required(VERSION 3.24)
project(order_flow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# PR-F02-001: order_flow now persists accepted FlowOrders to flow_orders +
# flow_order_legs so the F-04 matching loop's PG reader picks them up.
# libpqxx integration mirrors cpp/matching/CMakeLists.txt — first try the
# CMake config-installed target, then fall back to pkg-config.
find_package(PostgreSQL REQUIRED)
find_package(libpqxx CONFIG QUIET)
if(libpqxx_FOUND)
  if(TARGET libpqxx::pqxx)
    set(ORDER_FLOW_PQXX_TARGET libpqxx::pqxx)
  elseif(TARGET pqxx)
    set(ORDER_FLOW_PQXX_TARGET pqxx)
  else()
    message(FATAL_ERROR "libpqxx was found but no known CMake target is available")
  endif()
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBPQXX REQUIRED IMPORTED_TARGET libpqxx)
  set(ORDER_FLOW_PQXX_TARGET PkgConfig::LIBPQXX)
endif()

add_executable(order_flow
  src/main.cpp
  src/transport/grpc_order_flow_service.cpp
  src/app/order_flow_uc.cpp
  src/infra/risk_client.cpp
  src/infra/ledger_client.cpp
  src/infra/orders_kafka_publisher.cpp
  src/infra/batch_results_consumer.cpp
  src/infra/postgres/postgres_flow_order_repository.cpp
  src/domain/combo_order.cpp
  src/infra/postgres_combo_order_repository.cpp
  src/infra/orders_normalized_grouped_producer.cpp
  src/app/create_combo_order_use_case.cpp
  src/app/cancel_combo_order_use_case.cpp
)

target_include_directories(order_flow PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(order_flow PRIVATE
  cex_common
  contracts_proto
  ${ORDER_FLOW_PQXX_TARGET}
  PostgreSQL::PostgreSQL
)

target_compile_options(order_flow PRIVATE -Wall -Wextra -Wpedantic)

# F-09 (T-F09-030): domain unit tests. Hand-rolled harness (no GTest),
# mirrors matching_domain_tests; links only cex_common (brings Decimal +
# generated common proto transitively).
include(CTest)
if(BUILD_TESTING)
  add_executable(order_flow_domain_tests
    tests/domain/combo_order_domain_test.cpp
    src/domain/combo_order.cpp
  )
  target_include_directories(order_flow_domain_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(order_flow_domain_tests PRIVATE
    cex_common
  )
  target_compile_options(order_flow_domain_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME order_flow_domain_tests COMMAND order_flow_domain_tests)

  # F-09 (T-F09-034): combo repository integration test. Needs libpqxx;
  # skips at runtime if TEST_PG_DSN is unset (CI-without-PG stays green).
  add_executable(order_flow_repo_tests
    tests/infra/postgres_combo_order_repository_test.cpp
    src/infra/postgres_combo_order_repository.cpp
    src/domain/combo_order.cpp
  )
  target_include_directories(order_flow_repo_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(order_flow_repo_tests PRIVATE
    cex_common
    contracts_proto
    ${ORDER_FLOW_PQXX_TARGET}
    PostgreSQL::PostgreSQL
  )
  target_compile_options(order_flow_repo_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME order_flow_repo_tests COMMAND order_flow_repo_tests)

  # F-09 (T-F09-035): grouped orders.normalized producer test (mock publish fn).
  add_executable(order_flow_producer_tests
    tests/infra/orders_normalized_grouped_producer_test.cpp
    src/infra/orders_normalized_grouped_producer.cpp
    src/domain/combo_order.cpp
  )
  target_include_directories(order_flow_producer_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(order_flow_producer_tests PRIVATE
    cex_common
    contracts_proto
  )
  target_compile_options(order_flow_producer_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME order_flow_producer_tests COMMAND order_flow_producer_tests)

  # F-09 (T-F09-031): CreateComboOrderUseCase test (fake repo + capturing producer).
  # Links libpqxx because the repo port header includes <pqxx/pqxx>.
  add_executable(order_flow_combo_uc_tests
    tests/app/create_combo_order_use_case_test.cpp
    src/app/create_combo_order_use_case.cpp
    src/infra/orders_normalized_grouped_producer.cpp
    src/domain/combo_order.cpp
  )
  target_include_directories(order_flow_combo_uc_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(order_flow_combo_uc_tests PRIVATE
    cex_common
    contracts_proto
    ${ORDER_FLOW_PQXX_TARGET}
    PostgreSQL::PostgreSQL
  )
  target_compile_options(order_flow_combo_uc_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME order_flow_combo_uc_tests COMMAND order_flow_combo_uc_tests)

  # F-09 (T-F09-033): CancelComboOrderUseCase test (stateful fake repo).
  add_executable(order_flow_cancel_uc_tests
    tests/app/cancel_combo_order_use_case_test.cpp
    src/app/cancel_combo_order_use_case.cpp
    src/infra/orders_normalized_grouped_producer.cpp
    src/domain/combo_order.cpp
  )
  target_include_directories(order_flow_cancel_uc_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(order_flow_cancel_uc_tests PRIVATE
    cex_common
    contracts_proto
    ${ORDER_FLOW_PQXX_TARGET}
    PostgreSQL::PostgreSQL
  )
  target_compile_options(order_flow_cancel_uc_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME order_flow_cancel_uc_tests COMMAND order_flow_cancel_uc_tests)
endif()
