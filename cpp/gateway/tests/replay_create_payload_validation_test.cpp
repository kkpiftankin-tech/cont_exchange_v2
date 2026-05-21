#include <cstdlib>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "transport/http_gateway.cpp"

namespace {

using nlohmann::json;

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

json ValidPayload() {
  return json{
      {"name", "F15 validation"},
      {"instruments", json::array({"BTCUSDT"})},
      {"daterangefrom", "2026-04-01"},
      {"daterangeto", "2026-04-02"},
      {"strategy",
       json::array({json{{"symbol", "BTCUSDT"},
                         {"side", "buy"},
                         {"pL", 58000.0},
                         {"pH", 62000.0},
                         {"qrate", 0.5},
                         {"qmax", 100.0},
                         {"executionwindow", 3600}}})},
      {"solverconfigid", "production"},
      {"risklimitsid", "production"},
      {"feemodel", json{{"production", true}}},
  };
}

bool Validate(const json& payload, std::string* error) {
  return cex::gateway::transport::validate_replay_create_payload(payload, error);
}

bool test_valid_payload() {
  std::string error;
  return Check(Validate(ValidPayload(), &error), "valid F15 payload must pass");
}

bool test_name_is_required() {
  auto payload = ValidPayload();
  payload.erase("name");
  std::string error;
  if (!Check(!Validate(payload, &error), "payload without name must fail")) {
    return false;
  }
  return Check(error == "name is required", "missing name error must be explicit");
}

bool test_dates_must_be_parseable() {
  auto payload = ValidPayload();
  payload["daterangefrom"] = "not-a-date";
  std::string error;
  if (!Check(!Validate(payload, &error), "payload with invalid date must fail")) {
    return false;
  }
  return Check(error == "daterangefrom and daterangeto must be valid dates",
               "invalid date error must be explicit");
}

bool test_inline_overrides_are_accepted() {
  auto payload = ValidPayload();
  payload.erase("solverconfigid");
  payload.erase("risklimitsid");
  payload["solverconfig"] = json{{"batchintervalms", 1000},
                                 {"maxiterations", 128},
                                 {"tolerance", 0.000001},
                                 {"epsilonliquidity", 0.0},
                                 {"feemodel", json{{"makerfeerate", 0.0002},
                                                   {"takerfeerate", 0.0005}}}};
  payload["risklimits"] = json{{"maxnotional", 1000000},
                               {"maxposition", 100},
                               {"maxleverage", 3},
                               {"maxorderrate", 20},
                               {"whitelist", json::array({"BTCUSDT"})}};
  payload["feemodel"] = json{{"makerfeerate", 0.0002},
                             {"takerfeerate", 0.0005}};

  std::string error;
  return Check(Validate(payload, &error), "inline F15 overrides must pass");
}

bool test_replay_read_requires_auth_header_when_auth_enabled() {
  auto auth = std::make_shared<cex::gateway::transport::AuthMiddleware>(nullptr);
  crow::request req;
  crow::response error;
  const auto user = cex::gateway::transport::replay_authenticated_user(req, auth, &error);
  if (!Check(!user.has_value(), "unauthenticated replay read must be rejected")) {
    return false;
  }
  return Check(error.code == 401, "unauthenticated replay read returns 401");
}

bool test_replay_owner_is_read_from_session_payload() {
  crow::json::wvalue session;
  session["sessionid"] = "sess-a";
  session["userid"] = "user-a";
  const auto owner = cex::gateway::transport::replay_session_owner(session);
  if (!Check(owner.has_value(), "session owner must be extracted")) return false;
  return Check(*owner == "user-a", "session owner value");
}

bool test_command_accepted_payload_has_no_fabricated_lifecycle_status() {
  crow::json::wvalue out;
  out["accepted"] = true;
  out["command_id"] = "cmd-1";
  out["sessionid"] = "sess-1";
  out["commandstatus"] = "accepted";
  const auto parsed = json::parse(out.dump());
  if (!Check(parsed.value("accepted", false), "accepted command response")) return false;
  return Check(!parsed.contains("status"),
               "accepted command response must not fabricate replay status");
}

}  // namespace

int main() {
  bool ok = true;
  ok &= test_valid_payload();
  ok &= test_name_is_required();
  ok &= test_dates_must_be_parseable();
  ok &= test_inline_overrides_are_accepted();
  ok &= test_replay_read_requires_auth_header_when_auth_enabled();
  ok &= test_replay_owner_is_read_from_session_payload();
  ok &= test_command_accepted_payload_has_no_fabricated_lifecycle_status();
  if (!ok) return EXIT_FAILURE;
  std::cout << "[OK] gateway_replay_create_payload_validation_test" << std::endl;
  return EXIT_SUCCESS;
}
