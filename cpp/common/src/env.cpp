#include "cex/common/env.hpp"

#include <algorithm>
#include <cctype>

namespace cex::common {

std::optional<std::string> Env::try_get_string(const std::string& key) {
  const char* v = std::getenv(key.c_str());
  if (!v) return std::nullopt;
  return std::string(v);
}

std::string Env::get_string(const std::string& key, const std::string& def) {
  auto v = try_get_string(key);
  return v ? *v : def;
}

int Env::get_int(const std::string& key, int def) {
  auto v = try_get_string(key);
  if (!v) return def;
  try { return std::stoi(*v); } catch (...) { return def; }
}

bool Env::get_bool(const std::string& key, bool def) {
  auto v = try_get_string(key);
  if (!v) return def;
  std::string s = *v;
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
  if (s=="1" || s=="true" || s=="yes" || s=="y") return true;
  if (s=="0" || s=="false" || s=="no" || s=="n") return false;
  return def;
}

} // namespace cex::common
