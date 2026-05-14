# =============================================================================
# CMake-сборка библиотеки cex_common.
# Эта библиотека содержит общий код, используемый всеми C++ микросервисами
# биржи: работа с переменными окружения, UUID, время, JSON-логирование,
# сериализация protobuf и обёртки над Kafka (rdkafka++).
# Подключается во все остальные модули через target_link_libraries(... cex_common).
# =============================================================================

# Минимально требуемая версия CMake (3.24 — для современных команд find_package).
cmake_minimum_required(VERSION 3.22)
# Имя проекта/библиотеки и язык — только C++.
project(cex_common LANGUAGES CXX)

# Стандарт C++20 обязательный (без отката на более старые).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Подключаем Protobuf и gRPC: нужны для сериализации сообщений и RPC.
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)

# rdkafka++ распространяется через pkg-config — ищем именно так.
find_package(PkgConfig REQUIRED)
pkg_check_modules(RDKAFKA REQUIRED IMPORTED_TARGET rdkafka++)

# Статическая библиотека — линкуется внутрь каждого сервиса при сборке.
add_library(cex_common STATIC
  src/env.cpp       # чтение переменных окружения с дефолтами
  src/uuid.cpp      # генерация UUID v4 (для correlation_id и т.п.)
  src/time.cpp      # текущая метка времени в виде protobuf Timestamp
  src/log.cpp       # структурированный JSON-логгер
  src/proto.cpp     # сериализация/десериализация protobuf <-> bytes
  src/decimal.cpp   # арифметика над фиксированной точкой (units, scale)
  src/kafka.cpp     # обёртки над продюсером и консьюмером Kafka
)

# include/ доступен публично — то есть все, кто линкуется к cex_common,
# получают <cex/common/*.hpp> в путях поиска заголовков.
target_include_directories(cex_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Публичные зависимости — пробрасываются дальше в зависимые таргеты,
# поскольку они есть в публичных заголовках (proto.hpp, decimal.hpp и т.п.).
target_link_libraries(cex_common PUBLIC
  contracts_proto         # сгенерированный protobuf код из contracts/
  protobuf::libprotobuf   # рантайм protobuf
  gRPC::grpc++            # gRPC C++
  PkgConfig::RDKAFKA      # клиент Kafka (rdkafka++)
)

# Приватные опции компиляции — строгое предупреждение об ошибках.
target_compile_options(cex_common PRIVATE -Wall -Wextra -Wpedantic)
