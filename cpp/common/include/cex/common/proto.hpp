#pragma once
// ============================================================================
// proto.hpp — двусторонняя серилизация protobuf <-> std::string для Kafka.
//
// Kafka producer API хочет payload как std::string (raw bytes); consumer
// тоже отдаёт строку. Эти две функции — тонкая обёртка вокруг
// SerializeToString / ParseFromString с правильной обработкой ошибок.
//
// Все Kafka messages в проекте проходят через эту пару.
// ============================================================================
#include <string>
#include <google/protobuf/message.h>

namespace cex::common {

// Serialize protobuf message into a binary string for Kafka payload.
/// Wire format — binary protobuf. На случай serialization failure (например,
/// required field не set в proto2) возвращается пустая строка. Caller
/// должен проверять result.empty() либо предполагать non-empty contract.
std::string to_bytes(const google::protobuf::Message& msg);

// Parse protobuf from bytes. Returns true on success.
/// Parse failure (corrupted bytes / wrong type / extra fields) → false,
/// msg остаётся в indeterminate state — caller должен не использовать.
bool from_bytes(const std::string& bytes, google::protobuf::Message& msg);

}  // namespace cex::common
