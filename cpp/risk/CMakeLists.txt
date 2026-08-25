cmake_minimum_required(VERSION 3.24)
project(risk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# F-06 (T-F06-031): optional libpqxx for risk_snapshots persistence.
# Same conditional pattern as cpp/ledger — if libpqxx is missing the risk
# service builds and runs in degraded mode (no margin snapshots).
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPQXX QUIET IMPORTED_TARGET libpqxx)

add_executable(risk
  src/main.cpp
  src/transport/grpc_risk_service.cpp
  src/app/risk_uc.cpp
  src/domain/grouped_risk_check.cpp
  src/domain/margin_calculator.cpp
  src/infra/risk_alerts_publisher.cpp
  src/infra/risk_snapshot_repository.cpp
  src/infra/postgres/decimal_conversion.cpp
  src/infra/kafka_consumer.cpp
)

target_include_directories(risk PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(risk PRIVATE
  cex_common
  contracts_proto
)

if (LIBPQXX_FOUND)
  target_link_libraries(risk PRIVATE PkgConfig::LIBPQXX)
  target_compile_definitions(risk PRIVATE CEX_RISK_HAS_LIBPQXX=1)
endif()

target_compile_options(risk PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
  add_executable(risk_uc_test
    tests/risk_uc_test.cpp
    src/app/risk_uc.cpp
    src/domain/margin_calculator.cpp
    src/infra/risk_alerts_publisher.cpp
    src/infra/risk_snapshot_repository.cpp
    src/infra/postgres/decimal_conversion.cpp
  )

  target_include_directories(risk_uc_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(risk_uc_test PRIVATE
    cex_common
    contracts_proto
  )

  if (LIBPQXX_FOUND)
    target_link_libraries(risk_uc_test PRIVATE PkgConfig::LIBPQXX)
    target_compile_definitions(risk_uc_test PRIVATE CEX_RISK_HAS_LIBPQXX=1)
  endif()

  target_compile_options(risk_uc_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME risk_uc_test COMMAND risk_uc_test)

  # F-09 (T-F09-040): grouped pre-trade risk check (pure domain, Decimal only).
  add_executable(risk_grouped_risk_check_test
    tests/grouped_risk_check_test.cpp
    src/domain/grouped_risk_check.cpp
  )
  target_include_directories(risk_grouped_risk_check_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(risk_grouped_risk_check_test PRIVATE cex_common)
  target_compile_options(risk_grouped_risk_check_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME risk_grouped_risk_check_test COMMAND risk_grouped_risk_check_test)

  # F-06 (T-F06-031): margin calculator (U9/U10 — initial_margin, margin_call).
  add_executable(risk_margin_calculator_test
    tests/margin_calculator_test.cpp
    src/domain/margin_calculator.cpp
  )
  target_include_directories(risk_margin_calculator_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(risk_margin_calculator_test PRIVATE cex_common)
  target_compile_options(risk_margin_calculator_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME risk_margin_calculator_test COMMAND risk_margin_calculator_test)

  # F-06 (T-F06-073): risk_snapshots idempotency — повторная доставка одного
  # (entity_id, batch_id) даёт kDuplicate, не дубль-строку (ADR-046/ADR-020).
  # Тест пропускается (exit 0), если RISK_POSTGRES_DSN не задан или libpqxx нет
  # — idempotency проверяется на живой PG (ON CONFLICT против partial-unique
  # index). См. risk_snapshot_idempotency_test.cpp header.
  add_executable(risk_snapshot_idempotency_test
    tests/risk_snapshot_idempotency_test.cpp
    src/infra/risk_snapshot_repository.cpp
    src/infra/postgres/decimal_conversion.cpp
  )
  target_include_directories(risk_snapshot_idempotency_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(risk_snapshot_idempotency_test PRIVATE
    cex_common
    contracts_proto
  )
  if (LIBPQXX_FOUND)
    target_link_libraries(risk_snapshot_idempotency_test PRIVATE PkgConfig::LIBPQXX)
    target_compile_definitions(risk_snapshot_idempotency_test PRIVATE CEX_RISK_HAS_LIBPQXX=1)
  endif()
  target_compile_options(risk_snapshot_idempotency_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME risk_snapshot_idempotency_test COMMAND risk_snapshot_idempotency_test)
endif()
