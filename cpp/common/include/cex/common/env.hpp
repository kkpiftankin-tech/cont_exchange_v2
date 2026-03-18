#pragma once
#include <cstdlib>
#include <optional>
#include <string>

namespace cex::common {

// Small helper to read env vars with defaults.
// (Used across services so all config can be injected via Docker/K8s env.)
struct Env {
  static std::string get_string(const std::string& key, const std::string& def = "");
  static int get_int(const std::string& key, int def);
  static bool get_bool(const std::string& key, bool def);
  static std::optional<std::string> try_get_string(const std::string& key);
};

}  // namespace cex::common
