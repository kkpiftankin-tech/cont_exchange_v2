#pragma once
// =============================================================================
// Утилиты сериализации protobuf <-> бинарная строка.
//
// Используются в основном при работе с Kafka: туда нельзя положить objект,
// нужен payload в виде std::string (фактически byte buffer).
// =============================================================================

#include <string>
#include <google/protobuf/message.h>

namespace cex::common {

// Сериализовать protobuf-сообщение в бинарную строку (для отправки в Kafka/файл).
// Сообщение должно быть валидным (все required-поля заполнены, если есть).
std::string to_bytes(const google::protobuf::Message& msg);

// Распарсить бинарную строку в protobuf-сообщение.
// Возвращает false, если данные повреждены или не подходят под схему msg.
bool from_bytes(const std::string& bytes, google::protobuf::Message& msg);

}  // namespace cex::common
