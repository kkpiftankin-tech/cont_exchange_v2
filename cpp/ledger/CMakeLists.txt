cmake_minimum_required(VERSION 3.24)
project(ledger LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPQXX QUIET IMPORTED_TARGET libpqxx)

add_executable(ledger
  src/main.cpp
  src/transport/grpc_ledger_service.cpp
  src/app/ledger_uc.cpp
  src/infra/kafka_consumers.cpp
  src/infra/postgres_repositories.cpp
)

target_include_directories(ledger PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(ledger PRIVATE
  cex_common
  contracts_proto
)

if (LIBPQXX_FOUND)
  target_link_libraries(ledger PRIVATE PkgConfig::LIBPQXX)
  target_compile_definitions(ledger PRIVATE CEX_LEDGER_HAS_LIBPQXX=1)
endif()

target_compile_options(ledger PRIVATE -Wall -Wextra -Wpedantic)

if (BUILD_TESTING)
  add_executable(ledger_uc_test
    tests/ledger_uc_test.cpp
    src/app/ledger_uc.cpp
  )

  target_include_directories(ledger_uc_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(ledger_uc_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(ledger_uc_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)

  add_test(NAME ledger_uc_test COMMAND ledger_uc_test)

  # F-20 DoD-7 sim-book applier (pure; no PG, no Kafka).
  add_executable(ledger_sim_book_applier_test
    tests/sim_book_applier_test.cpp
    src/app/sim_book_applier.cpp
  )

  target_include_directories(ledger_sim_book_applier_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(ledger_sim_book_applier_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(ledger_sim_book_applier_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)

  add_test(NAME ledger_sim_book_applier_test COMMAND ledger_sim_book_applier_test)

  add_executable(ledger_persistence_test
    tests/ledger_persistence_test.cpp
    src/app/ledger_uc.cpp
  )

  target_include_directories(ledger_persistence_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(ledger_persistence_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(ledger_persistence_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)

  add_test(NAME ledger_persistence_test COMMAND ledger_persistence_test)

  add_executable(ledger_idempotency_test
    tests/ledger_idempotency_test.cpp
    src/app/ledger_uc.cpp
  )

  target_include_directories(ledger_idempotency_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(ledger_idempotency_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(ledger_idempotency_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)

  add_test(NAME ledger_idempotency_test COMMAND ledger_idempotency_test)

  add_executable(ledger_pnl_test
    tests/ledger_pnl_test.cpp
    src/app/ledger_uc.cpp
  )

  target_include_directories(ledger_pnl_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(ledger_pnl_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(ledger_pnl_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  add_test(NAME ledger_pnl_test COMMAND ledger_pnl_test)

  add_executable(ledger_unrealised_pnl_test
    tests/ledger_unrealised_pnl_test.cpp
    src/app/ledger_uc.cpp
  )

  target_include_directories(ledger_unrealised_pnl_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(ledger_unrealised_pnl_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(ledger_unrealised_pnl_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  add_test(NAME ledger_unrealised_pnl_test COMMAND ledger_unrealised_pnl_test)

  add_executable(ledger_venue_balance_test
    tests/ledger_venue_balance_test.cpp
    src/app/ledger_uc.cpp
  )

  target_include_directories(ledger_venue_balance_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(ledger_venue_balance_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(ledger_venue_balance_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  add_test(NAME ledger_venue_balance_test COMMAND ledger_venue_balance_test)

  add_executable(ledger_hedge_pnl_test
    tests/ledger_hedge_pnl_test.cpp
    src/app/ledger_uc.cpp
  )

  target_include_directories(ledger_hedge_pnl_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(ledger_hedge_pnl_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(ledger_hedge_pnl_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  add_test(NAME ledger_hedge_pnl_test COMMAND ledger_hedge_pnl_test)

  add_executable(ledger_execution_reconciliation_test
    tests/ledger_execution_reconciliation_test.cpp
    src/app/ledger_uc.cpp
  )

  target_include_directories(ledger_execution_reconciliation_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(ledger_execution_reconciliation_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(ledger_execution_reconciliation_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  add_test(NAME ledger_execution_reconciliation_test COMMAND ledger_execution_reconciliation_test)

endif()
