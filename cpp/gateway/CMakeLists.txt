cmake_minimum_required(VERSION 3.24)
project(gateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

set(Boost_NO_BOOST_CMAKE ON)
set(Boost_NO_WARN_NEW_VERSIONS ON)
find_package(Boost REQUIRED COMPONENTS thread)
find_package(PkgConfig REQUIRED)

set(GATEWAY_JSON_TARGET "")
if (TARGET nlohmann_json::nlohmann_json)
  set(GATEWAY_JSON_TARGET nlohmann_json::nlohmann_json)
else()
  find_package(nlohmann_json CONFIG QUIET)
  if (TARGET nlohmann_json::nlohmann_json)
    set(GATEWAY_JSON_TARGET nlohmann_json::nlohmann_json)
  else()
    pkg_check_modules(NLOHMANN_JSON QUIET IMPORTED_TARGET nlohmann_json)
    if (TARGET PkgConfig::NLOHMANN_JSON)
      set(GATEWAY_JSON_TARGET PkgConfig::NLOHMANN_JSON)
    else()
      FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
      FetchContent_MakeAvailable(json)
      set(GATEWAY_JSON_TARGET nlohmann_json::nlohmann_json)
    endif()
  endif()
endif()

add_executable(gateway
  src/main.cpp
  src/transport/http_gateway.cpp
  src/transport/auth_middleware.cpp
  src/infra/order_flow_client.cpp
  src/infra/market_data_client.cpp
  src/infra/ledger_client.cpp
  src/infra/risk_client.cpp
  src/infra/replay_commands_kafka_publisher.cpp
  src/infra/replay_results_hub.cpp
  src/infra/replay_results_kafka_consumer.cpp
  src/infra/positions_hub.cpp
  src/infra/positions_update_kafka_consumer.cpp
  src/infra/positions_cache.cpp
)

target_include_directories(gateway PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/crow/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/cpp/backtest/src
  ${CMAKE_BINARY_DIR}
  ${CMAKE_BINARY_DIR}/contracts/gen
)

target_link_libraries(gateway PRIVATE
  cex_common
  contracts_proto
  Boost::thread
  backtest_lib
  ${GATEWAY_JSON_TARGET}
)

target_compile_definitions(gateway PRIVATE 
  CROW_USE_BOOST
  BOOST_ASIO_DISABLE_STD_EXPERIMENTAL
  BOOST_ASIO_USE_STD_INVOKE
)

target_compile_options(gateway PRIVATE -Wall -Wextra -Wpedantic)

if (BUILD_TESTING)
  add_executable(gateway_replay_results_hub_test
    tests/replay_results_hub_test.cpp
    src/infra/replay_results_hub.cpp
  )

  target_include_directories(gateway_replay_results_hub_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_BINARY_DIR}/contracts/gen
  )

  target_link_libraries(gateway_replay_results_hub_test PRIVATE
    cex_common
    contracts_proto
  )

  target_compile_options(gateway_replay_results_hub_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG
  )

  add_test(NAME gateway_replay_results_hub_test
    COMMAND gateway_replay_results_hub_test
  )

  add_executable(gateway_replay_create_payload_validation_test
    tests/replay_create_payload_validation_test.cpp
    src/transport/auth_middleware.cpp
    src/infra/order_flow_client.cpp
    src/infra/market_data_client.cpp
    src/infra/ledger_client.cpp
    src/infra/risk_client.cpp
    src/infra/replay_commands_kafka_publisher.cpp
    src/infra/replay_results_hub.cpp
    src/infra/replay_results_kafka_consumer.cpp
    # F-06: this test #includes http_gateway.cpp, which now uses the positions
    # WS hub + TTL cache — compile their TUs so symbols resolve at link time.
    src/infra/positions_hub.cpp
    src/infra/positions_cache.cpp
  )

  target_include_directories(gateway_replay_create_payload_validation_test PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/crow/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/cpp/backtest/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_BINARY_DIR}/contracts/gen
  )

  target_link_libraries(gateway_replay_create_payload_validation_test PRIVATE
    cex_common
    contracts_proto
    Boost::thread
    backtest_lib
    ${GATEWAY_JSON_TARGET}
  )

  target_compile_definitions(gateway_replay_create_payload_validation_test PRIVATE
    CROW_USE_BOOST
    BOOST_ASIO_DISABLE_STD_EXPERIMENTAL
    BOOST_ASIO_USE_STD_INVOKE
  )

  target_compile_options(gateway_replay_create_payload_validation_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG
  )

  add_test(NAME gateway_replay_create_payload_validation_test
    COMMAND gateway_replay_create_payload_validation_test
  )

  # F-06 (T-F06-072, ADR-046): PositionsHub fan-out + consumer parse/dedup.
  add_executable(gateway_positions_hub_test
    tests/positions_hub_test.cpp
    src/infra/positions_hub.cpp
    src/infra/positions_update_kafka_consumer.cpp
  )

  target_include_directories(gateway_positions_hub_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_BINARY_DIR}/contracts/gen
  )

  target_link_libraries(gateway_positions_hub_test PRIVATE
    cex_common
    contracts_proto
    ${GATEWAY_JSON_TARGET}
  )

  target_compile_options(gateway_positions_hub_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG
  )

  add_test(NAME gateway_positions_hub_test
    COMMAND gateway_positions_hub_test
  )

  # F-06 (T-F06-071-cache, ADR-045): short-lived per-user positions TTL cache.
  add_executable(gateway_positions_cache_test
    tests/positions_cache_test.cpp
    src/infra/positions_cache.cpp
  )

  target_include_directories(gateway_positions_cache_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_BINARY_DIR}/contracts/gen
  )

  target_link_libraries(gateway_positions_cache_test PRIVATE
    cex_common
    contracts_proto
    ${GATEWAY_JSON_TARGET}
  )

  target_compile_options(gateway_positions_cache_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG
  )

  add_test(NAME gateway_positions_cache_test
    COMMAND gateway_positions_cache_test
  )
endif()
