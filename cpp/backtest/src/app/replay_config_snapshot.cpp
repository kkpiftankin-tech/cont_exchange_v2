#include "app/replay_config_snapshot.hpp"

#include <iomanip>
#include <sstream>

namespace cex::backtest::app {
namespace {

std::string JsonEscape(const std::string& input) {
  std::ostringstream out;
  for (char c : input) {
    const auto uc = static_cast<unsigned char>(c);
    switch (c) {
      case '\"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (uc < 0x20) {
          out << "\\u00";
          constexpr char kHex[] = "0123456789abcdef";
          out << kHex[(uc >> 4) & 0x0f] << kHex[uc & 0x0f];
        } else {
          out << c;
        }
    }
  }
  return out.str();
}

std::string FormatTolerance(double tolerance) {
  std::ostringstream oss;
  oss << std::setprecision(17) << tolerance;
  return oss.str();
}

void WriteSection(std::ostringstream& out,
                  const std::string& key,
                  const ReplayConfigSection& section) {
  out << "\"" << key << "\":{"
      << "\"id\":\"" << JsonEscape(section.id) << "\","
      << "\"version\":" << section.version << ","
      << "\"inline_override\":" << (section.inline_override ? "true" : "false") << ","
      << "\"body_json\":\"" << JsonEscape(section.body_json) << "\""
      << "}";
}

std::string SerializeSnapshot(const SessionConfigSnapshot& snap) {
  std::ostringstream out;
  out << "{";
  out << "\"snapshot_version\":" << SessionConfigSnapshot::kSchemaVersion << ",";
  WriteSection(out, "solver_config", snap.solver_config); out << ",";
  WriteSection(out, "risk_limits", snap.risk_limits); out << ",";
  WriteSection(out, "fee_model", snap.fee_model); out << ",";
  WriteSection(out, "reward_config", snap.reward_config); out << ",";
  out << "\"random_seed\":" << snap.random_seed << ",";
  out << "\"tolerance\":" << FormatTolerance(snap.tolerance) << ",";
  out << "\"avgis_rule\":\"" << JsonEscape(snap.avgis_rule) << "\",";
  out << "\"decision_price_source\":\""
      << JsonEscape(snap.decision_price_source) << "\"";
  out << "}";
  return out.str();
}

bool ResolveSection(IReplayConfigRepository* repo,
                    const std::string& id,
                    const std::optional<std::string>& inline_override,
                    std::optional<StoredConfigDocument> (IReplayConfigRepository::*loader)(
                        const std::string&),
                    const char* section_name,
                    ReplayConfigSection* out_section,
                    std::string* error) {
  out_section->id = id;
  if (inline_override.has_value()) {
    out_section->body_json = *inline_override;
    out_section->version = 0;
    out_section->inline_override = true;
    return true;
  }
  if (id.empty()) {
    *error = std::string("Missing ") + section_name + " id and no inline override";
    return false;
  }
  if (repo == nullptr) {
    *error = std::string("Config repository is not configured for ") + section_name;
    return false;
  }
  auto doc = (repo->*loader)(id);
  if (!doc.has_value()) {
    *error = std::string(section_name) + " not found: id=" + id;
    return false;
  }
  out_section->id = doc->id.empty() ? id : doc->id;
  out_section->version = doc->version;
  out_section->body_json = doc->body_json;
  out_section->inline_override = false;
  return true;
}

}  // namespace

ReplayConfigSnapshotBuilder::ReplayConfigSnapshotBuilder(
    IReplayConfigRepository* repository)
    : repository_(repository) {}

SnapshotResult ReplayConfigSnapshotBuilder::Build(
    const ReplayConfigRequest& request) const {
  SnapshotResult result;
  SessionConfigSnapshot& snap = result.snapshot;

  if (!ResolveSection(repository_, request.solver_config_id,
                      request.solver_config_inline_override,
                      &IReplayConfigRepository::GetSolverConfig,
                      "solver_config", &snap.solver_config, &result.error)) {
    return result;
  }
  if (!ResolveSection(repository_, request.risk_limits_id,
                      request.risk_limits_inline_override,
                      &IReplayConfigRepository::GetRiskLimits,
                      "risk_limits", &snap.risk_limits, &result.error)) {
    return result;
  }
  if (!ResolveSection(repository_, request.fee_model_id,
                      request.fee_model_inline_override,
                      &IReplayConfigRepository::GetFeeModel,
                      "fee_model", &snap.fee_model, &result.error)) {
    return result;
  }
  if (!ResolveSection(repository_, request.reward_config_id,
                      request.reward_config_inline_override,
                      &IReplayConfigRepository::GetRewardConfig,
                      "reward_config", &snap.reward_config, &result.error)) {
    return result;
  }

  snap.random_seed = request.random_seed.value_or(0);
  snap.tolerance = request.tolerance.value_or(0.0);
  snap.avgis_rule = request.avgis_rule.empty() ? "volume_weighted"
                                               : request.avgis_rule;
  snap.decision_price_source =
      request.decision_price_source.empty()
          ? "marketdata_mid_with_clearprice_fallback"
          : request.decision_price_source;
  snap.snapshot_json = SerializeSnapshot(snap);
  result.ok = true;
  return result;
}

}  // namespace cex::backtest::app
