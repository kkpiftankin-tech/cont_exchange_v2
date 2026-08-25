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
  src/infra/positions_update_publisher.cpp
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

  # F-06 (T-F06-021/022/023): position transitions (increase/reduce/close/flip,
  # short), mark-to-market unrealized PnL, Decimal scale invariant.
  add_executable(ledger_f06_positions_test
    tests/ledger_f06_positions_test.cpp
    src/app/ledger_uc.cpp
  )

  target_include_directories(ledger_f06_positions_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(ledger_f06_positions_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(ledger_f06_positions_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  add_test(NAME ledger_f06_positions_test COMMAND ledger_f06_positions_test)

  # F-06 (T-F06-023): buy-reserve-leak — leftover reserved released on terminal order.
  add_executable(ledger_reserve_leak_test
    tests/ledger_reserve_leak_test.cpp
    src/app/ledger_uc.cpp
  )

  target_include_directories(ledger_reserve_leak_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(ledger_reserve_leak_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(ledger_reserve_leak_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  add_test(NAME ledger_reserve_leak_test COMMAND ledger_reserve_leak_test)

  # F-06 (T-F06-061): in-process integration chain test. Wires the same app
  # objects the live services use (ledger ApplyBatchResult -> GetPositionsView
  # -> risk ComputeMargin) and drives I2/I3/I4/I5 deterministically without a
  # Kafka/PG/docker stack. Pulls in the risk margin_calculator domain source.
  add_executable(ledger_f06_integration_chain_test
    tests/ledger_f06_integration_chain_test.cpp
    src/app/ledger_uc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../risk/src/domain/margin_calculator.cpp
  )

  target_include_directories(ledger_f06_integration_chain_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../risk/src
  )

  target_link_libraries(ledger_f06_integration_chain_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(ledger_f06_integration_chain_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  add_test(NAME ledger_f06_integration_chain_test COMMAND ledger_f06_integration_chain_test)

  # F-06 P0-A (T-F06-070, ADR-044): mirror reserve/release into PG `accounts`.
  # ReserveTx free->reserved, ReleaseTx reserved->free, full reserve->fill cycle
  # leaves reserved=0 with no underflow (GREATEST floor removed). Uses an
  # in-memory fake mirror — no live DB needed (matches other ledger unit tests).
  add_executable(ledger_account_reserve_test
    tests/ledger_account_reserve_test.cpp
    src/app/ledger_uc.cpp
  )

  target_include_directories(ledger_account_reserve_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(ledger_account_reserve_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(ledger_account_reserve_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  add_test(NAME ledger_account_reserve_test COMMAND ledger_account_reserve_test)

  # F-06 / F6-5 (T-F06-072, ADR-046): positions.update producer. Verifies that
  # ApplyBatchResult emits one positions.update signal per UNIQUE user_id in the
  # batch (dedup), via a fake publisher (no Kafka). Pure app-layer test.
  add_executable(ledger_positions_update_test
    tests/ledger_positions_update_test.cpp
    src/app/ledger_uc.cpp
  )

  target_include_directories(ledger_positions_update_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(ledger_positions_update_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(ledger_positions_update_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  add_test(NAME ledger_positions_update_test COMMAND ledger_positions_update_test)

endif()
