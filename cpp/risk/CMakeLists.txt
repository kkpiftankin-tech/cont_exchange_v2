cmake_minimum_required(VERSION 3.24)
project(risk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(risk
  src/main.cpp
  src/transport/grpc_risk_service.cpp
  src/app/risk_uc.cpp
  src/domain/grouped_risk_check.cpp
  src/infra/risk_alerts_publisher.cpp
  src/infra/kafka_consumer.cpp
)

target_include_directories(risk PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(risk PRIVATE
  cex_common
  contracts_proto
)

target_compile_options(risk PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
  add_executable(risk_uc_test
    tests/risk_uc_test.cpp
    src/app/risk_uc.cpp
    src/infra/risk_alerts_publisher.cpp
  )

  target_include_directories(risk_uc_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(risk_uc_test PRIVATE
    cex_common
    contracts_proto
  )

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
endif()
