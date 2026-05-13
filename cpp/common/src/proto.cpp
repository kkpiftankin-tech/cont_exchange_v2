// =============================================================================
// Реализация утилит сериализации protobuf <-> std::string (см. proto.hpp).
// =============================================================================

#include "cex/common/proto.hpp"

namespace cex::common {

// SerializeToString отдаёт бинарный protobuf-формат (не текст). Текстовый формат
// (DebugString/MessageToString) для прода не используем — он медленнее и больше.
std::string to_bytes(const google::protobuf::Message& msg) {
  std::string out;
  msg.SerializeToString(&out);
  return out;
}

// Возвращает false при повреждённом payload или несовместимой схеме.
// Звуковой звоночек: если повсеместно возвращается false — где-то отправитель
// пишет другой тип сообщения в этот же топик.
bool from_bytes(const std::string& bytes, google::protobuf::Message& msg) {
  return msg.ParseFromString(bytes);
}

} // namespace cex::common
