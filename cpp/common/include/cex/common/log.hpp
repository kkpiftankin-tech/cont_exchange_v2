#pragma once
#include <map>
#include <string>

namespace cex::common {

// Minimal JSON logger (stdout).
// Goal: consistent structured logs across services, as recommended by the methodology.
void log_json(const std::string& level,
              const std::string& message,
              const std::map<std::string, std::string>& fields = {});

}  // namespace cex::common
