#include "cex/common/replay_kafka.hpp"

#include <charconv>
#include <sstream>
#include <string>
#include <unordered_map>

namespace cex::common {
namespace {

std::string EscapeField(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (char ch : input) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '=': out += "\\="; break;
      default: out.push_back(ch); break;
    }
  }
  return out;
}

std::string UnescapeField(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    const char ch = input[i];
    if (ch == '\\' && i + 1 < input.size()) {
      const char next = input[i + 1];
      switch (next) {
        case '\\': out.push_back('\\'); ++i; continue;
        case 'n': out.push_back('\n'); ++i; continue;
        case 'r': out.push_back('\r'); ++i; continue;
        case '=': out.push_back('='); ++i; continue;
        default: break;
      }
    }
    out.push_back(ch);
  }
  return out;
}

void AppendLine(std::ostringstream& out,
                const std::string& key,
                const std::string& value) {
  out << key << "=" << EscapeField(value) << "\n";
}

void AppendLine(std::ostringstream& out,
                const std::string& key,
                const char* value) {
  out << key << "=" << EscapeField(value == nullptr ? "" : std::string(value)) << "\n";
}

void AppendLine(std::ostringstream& out,
                const std::string& key,
                int64_t value) {
  out << key << "=" << value << "\n";
}

void AppendLine(std::ostringstream& out,
                const std::string& key,
                uint32_t value) {
  out << key << "=" << value << "\n";
}

void AppendLine(std::ostringstream& out,
                const std::string& key,
                double value) {
  out << key << "=" << value << "\n";
}

void AppendLine(std::ostringstream& out,
                const std::string& key,
                bool value) {
  out << key << "=" << (value ? "1" : "0") << "\n";
}

std::unordered_map<std::string, std::string> ParseKeyValuePayload(
    const std::string& payload) {
  std::unordered_map<std::string, std::string> fields;
  std::istringstream ss(payload);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    size_t split = std::string::npos;
    for (size_t i = 0; i < line.size(); ++i) {
      if (line[i] == '=' && (i == 0 || line[i - 1] != '\\')) {
        split = i;
        break;
      }
    }
    if (split == std::string::npos) continue;
    const std::string key = line.substr(0, split);
    const std::string value = line.substr(split + 1);
    fields[key] = UnescapeField(value);
  }
  return fields;
}

int64_t ParseInt64(const std::string& value) {
  int64_t out = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  std::from_chars(begin, end, out);
  return out;
}

uint32_t ParseUint32(const std::string& value) {
  uint32_t out = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  std::from_chars(begin, end, out);
  return out;
}

double ParseDouble(const std::string& value) {
  try {
    return std::stod(value);
  } catch (...) {
    return 0.0;
  }
}

bool ParseBool(const std::string& value) {
  return value == "1" || value == "true" || value == "TRUE";
}

std::string JsonEscape(const std::string& input) {
  std::ostringstream out;
  for (char ch : input) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default: out << ch; break;
    }
  }
  return out.str();
}

}  // namespace

std::string ReplayCommandTypeToString(ReplayCommandType type) {
  switch (type) {
    case ReplayCommandType::kStart: return "start";
    case ReplayCommandType::kCancel: return "cancel";
    case ReplayCommandType::kRetry: return "retry";
    default: return "unknown";
  }
}

ReplayCommandType ReplayCommandTypeFromString(const std::string& type) {
  if (type == "start") return ReplayCommandType::kStart;
  if (type == "cancel") return ReplayCommandType::kCancel;
  if (type == "retry") return ReplayCommandType::kRetry;
  return ReplayCommandType::kUnknown;
}

std::string ReplayResultKindToString(ReplayResultKind kind) {
  switch (kind) {
    case ReplayResultKind::kProgress: return "progress";
    case ReplayResultKind::kLifecycle: return "lifecycle";
    default: return "unknown";
  }
}

ReplayResultKind ReplayResultKindFromString(const std::string& kind) {
  if (kind == "progress") return ReplayResultKind::kProgress;
  if (kind == "lifecycle") return ReplayResultKind::kLifecycle;
  return ReplayResultKind::kUnknown;
}

std::string SerializeReplayCommandMessage(const ReplayCommandMessage& msg) {
  std::ostringstream out;
  AppendLine(out, "version", int64_t{1});
  AppendLine(out, "message_type", "replay_command");
  AppendLine(out, "command_id", msg.command_id);
  AppendLine(out, "session_id", msg.session_id);
  AppendLine(out, "command_type", ReplayCommandTypeToString(msg.command_type));
  AppendLine(out, "requested_by", msg.requested_by);
  AppendLine(out, "created_at_ms", msg.created_at_ms);
  AppendLine(out, "payload_json", msg.payload_json);
  return out.str();
}

bool ParseReplayCommandMessage(const std::string& payload, ReplayCommandMessage* out) {
  if (out == nullptr) return false;
  const auto fields = ParseKeyValuePayload(payload);
  const auto it = fields.find("message_type");
  if (it == fields.end() || it->second != "replay_command") return false;

  out->command_id = fields.contains("command_id") ? fields.at("command_id") : "";
  out->session_id = fields.contains("session_id") ? fields.at("session_id") : "";
  out->command_type = ReplayCommandTypeFromString(
      fields.contains("command_type") ? fields.at("command_type") : "");
  out->requested_by = fields.contains("requested_by") ? fields.at("requested_by") : "";
  out->created_at_ms = fields.contains("created_at_ms")
                           ? ParseInt64(fields.at("created_at_ms"))
                           : 0;
  out->payload_json = fields.contains("payload_json") ? fields.at("payload_json") : "";
  return true;
}

std::string SerializeReplayResultMessage(const ReplayResultMessage& msg) {
  std::ostringstream out;
  AppendLine(out, "version", int64_t{1});
  AppendLine(out, "message_type", "replay_result");
  AppendLine(out, "kind", ReplayResultKindToString(msg.kind));
  AppendLine(out, "session_id", msg.session_id);
  AppendLine(out, "published_at_ms", msg.published_at_ms);
  AppendLine(out, "batch_seq", msg.batch_seq);
  AppendLine(out, "total_batches", msg.total_batches);
  AppendLine(out, "status", msg.status);
  AppendLine(out, "has_summary", msg.has_summary);
  AppendLine(out, "processed_batches", msg.processed_batches);
  AppendLine(out, "failed_batches", msg.failed_batches);
  AppendLine(out, "total_fill_events", msg.total_fill_events);
  AppendLine(out, "partial", msg.partial);
  AppendLine(out, "total_pnl", msg.total_pnl);
  AppendLine(out, "avg_pnl", msg.avg_pnl);
  AppendLine(out, "avg_is", msg.avg_is);
  AppendLine(out, "sharpe", msg.sharpe);
  AppendLine(out, "avg_fill_rate", msg.avg_fill_rate);
  AppendLine(out, "avg_solve_time_ms", msg.avg_solve_time_ms);
  AppendLine(out, "max_drawdown", msg.max_drawdown);
  AppendLine(out, "std_pnl", msg.std_pnl);
  AppendLine(out, "avg_vwap", msg.avg_vwap);
  AppendLine(out, "error_details", msg.error_details);
  AppendLine(out, "error_code", msg.error_code);
  return out.str();
}

bool ParseReplayResultMessage(const std::string& payload, ReplayResultMessage* out) {
  if (out == nullptr) return false;
  const auto fields = ParseKeyValuePayload(payload);
  const auto it = fields.find("message_type");
  if (it == fields.end() || it->second != "replay_result") return false;

  out->kind = ReplayResultKindFromString(fields.contains("kind") ? fields.at("kind") : "");
  out->session_id = fields.contains("session_id") ? fields.at("session_id") : "";
  out->published_at_ms = fields.contains("published_at_ms")
                             ? ParseInt64(fields.at("published_at_ms"))
                             : 0;
  out->batch_seq = fields.contains("batch_seq") ? ParseUint32(fields.at("batch_seq")) : 0;
  out->total_batches = fields.contains("total_batches")
                           ? ParseUint32(fields.at("total_batches"))
                           : 0;
  out->status = fields.contains("status") ? fields.at("status") : "";
  out->has_summary = fields.contains("has_summary")
                         ? ParseBool(fields.at("has_summary"))
                         : false;
  out->processed_batches = fields.contains("processed_batches")
                               ? ParseUint32(fields.at("processed_batches"))
                               : 0;
  out->failed_batches = fields.contains("failed_batches")
                            ? ParseUint32(fields.at("failed_batches"))
                            : 0;
  out->total_fill_events = fields.contains("total_fill_events")
                               ? ParseUint32(fields.at("total_fill_events"))
                               : 0;
  out->partial = fields.contains("partial") ? ParseBool(fields.at("partial")) : false;
  out->total_pnl = fields.contains("total_pnl") ? ParseDouble(fields.at("total_pnl")) : 0.0;
  out->avg_pnl = fields.contains("avg_pnl") ? ParseDouble(fields.at("avg_pnl")) : 0.0;
  out->avg_is = fields.contains("avg_is") ? ParseDouble(fields.at("avg_is")) : 0.0;
  out->sharpe = fields.contains("sharpe") ? ParseDouble(fields.at("sharpe")) : 0.0;
  out->avg_fill_rate = fields.contains("avg_fill_rate")
                           ? ParseDouble(fields.at("avg_fill_rate"))
                           : 0.0;
  out->avg_solve_time_ms = fields.contains("avg_solve_time_ms")
                               ? ParseDouble(fields.at("avg_solve_time_ms"))
                               : 0.0;
  out->max_drawdown = fields.contains("max_drawdown")
                          ? ParseDouble(fields.at("max_drawdown"))
                          : 0.0;
  out->std_pnl = fields.contains("std_pnl") ? ParseDouble(fields.at("std_pnl")) : 0.0;
  out->avg_vwap = fields.contains("avg_vwap") ? ParseDouble(fields.at("avg_vwap")) : 0.0;
  out->error_details = fields.contains("error_details") ? fields.at("error_details") : "";
  out->error_code = fields.contains("error_code") ? fields.at("error_code") : "";
  return true;
}

std::string ReplayResultMessageToJson(const ReplayResultMessage& msg) {
  std::ostringstream out;
  out << "{";
  out << "\"kind\":\"" << JsonEscape(ReplayResultKindToString(msg.kind)) << "\",";
  out << "\"session_id\":\"" << JsonEscape(msg.session_id) << "\",";
  out << "\"published_at_ms\":" << msg.published_at_ms << ",";
  out << "\"batch_seq\":" << msg.batch_seq << ",";
  out << "\"total_batches\":" << msg.total_batches << ",";
  out << "\"status\":\"" << JsonEscape(msg.status) << "\",";
  out << "\"has_summary\":" << (msg.has_summary ? "true" : "false") << ",";
  out << "\"error_details\":\"" << JsonEscape(msg.error_details) << "\",";
  out << "\"error_code\":\"" << JsonEscape(msg.error_code) << "\"";
  if (msg.kind == ReplayResultKind::kProgress) {
    out << ",\"progress\":{";
    out << "\"batch_seq\":" << msg.batch_seq << ",";
    out << "\"total_batches\":" << msg.total_batches;
    out << "}";
  }
  if (msg.has_summary) {
    out << ",\"summary\":{";
    out << "\"processed_batches\":" << msg.processed_batches << ",";
    out << "\"failed_batches\":" << msg.failed_batches << ",";
    out << "\"total_fill_events\":" << msg.total_fill_events << ",";
    out << "\"partial\":" << (msg.partial ? "true" : "false") << ",";
    out << "\"total_pnl\":" << msg.total_pnl << ",";
    out << "\"avg_pnl\":" << msg.avg_pnl << ",";
    out << "\"avg_is\":" << msg.avg_is << ",";
    out << "\"sharpe\":" << msg.sharpe << ",";
    out << "\"avg_fill_rate\":" << msg.avg_fill_rate << ",";
    out << "\"avg_solve_time_ms\":" << msg.avg_solve_time_ms << ",";
    out << "\"max_drawdown\":" << msg.max_drawdown << ",";
    out << "\"std_pnl\":" << msg.std_pnl << ",";
    out << "\"avg_vwap\":" << msg.avg_vwap;
    out << "}";
  }
  out << "}";
  return out.str();
}

}  // namespace cex::common
