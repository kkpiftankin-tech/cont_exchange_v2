// ============================================================================
// positions_update_publisher.cpp — реализация producer'а positions.update.
//
// Payload — лёгкий JSON {user_id, batch_id, ts} (ADR-046). См. .hpp.
// ============================================================================
#include "infra/positions_update_publisher.hpp"

#include <string>

#include "cex/common/log.hpp"

namespace cex::ledger::infra {

namespace {

constexpr char kTopic[] = "positions.update";

/// Минимальное JSON-экранирование строкового значения (кавычки, бэкслеши,
/// управляющие символы). user_id/batch_id — обычно безопасные ID, но
/// экранируем, чтобы не сломать payload при нестандартных значениях.
void append_json_string(std::string& out, const std::string& value) {
  out.push_back('"');
  for (const char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          static const char* hex = "0123456789abcdef";
          out += "\\u00";
          out.push_back(hex[(c >> 4) & 0xF]);
          out.push_back(hex[c & 0xF]);
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
}

}  // namespace

PositionsUpdatePublisher::PositionsUpdatePublisher(cex::common::KafkaProducer producer)
    : producer_(std::move(producer)) {}

bool PositionsUpdatePublisher::Publish(const std::string& user_id,
                                       const std::string& batch_id,
                                       int64_t ts_unix) {
  // payload: {"user_id":"...","batch_id":"...","ts":<unix_seconds>}
  std::string payload = "{\"user_id\":";
  append_json_string(payload, user_id);
  payload += ",\"batch_id\":";
  append_json_string(payload, batch_id);
  payload += ",\"ts\":";
  payload += std::to_string(ts_unix);
  payload.push_back('}');

  // key = user_id — push партиционируется/маршрутизируется по пользователю.
  const bool ok = producer_.produce(kTopic, user_id, payload);
  if (ok) {
    cex::common::log_json("INFO", "Published positions.update",
                          {{"topic", kTopic},
                           {"user_id", user_id},
                           {"batch_id", batch_id}});
  } else {
    cex::common::log_json("ERROR", "Failed to publish positions.update",
                          {{"topic", kTopic},
                           {"user_id", user_id},
                           {"batch_id", batch_id}});
  }
  return ok;
}

}  // namespace cex::ledger::infra
