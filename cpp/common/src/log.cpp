#include "cex/common/log.hpp"

#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>

namespace cex::common {

static std::string iso8601_now() {
  using namespace std::chrono;
  auto now = system_clock::now();
  auto t = system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

static std::string json_escape(const std::string& s) {
  std::ostringstream o;
  for (auto c : s) {
    switch (c) {
      case '\"': o << "\\\""; break;
      case '\\': o << "\\\\"; break;
      case '\b': o << "\\b"; break;
      case '\f': o << "\\f"; break;
      case '\n': o << "\\n"; break;
      case '\r': o << "\\r"; break;
      case '\t': o << "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          o << "\\u" << std::hex << std::setw(4) << std::setfill('0')
            << (int)static_cast<unsigned char>(c);
        } else {
          o << c;
        }
    }
  }
  return o.str();
}

void log_json(const std::string& level,
              const std::string& message,
              const std::map<std::string, std::string>& fields) {
  std::ostringstream oss;
  oss << "{";
  oss << "\"ts\":\"" << iso8601_now() << "\",";
  oss << "\"level\":\"" << json_escape(level) << "\",";
  oss << "\"msg\":\"" << json_escape(message) << "\"";
  for (const auto& [k, v] : fields) {
    oss << ",\"" << json_escape(k) << "\":\"" << json_escape(v) << "\"";
  }
  oss << "}";
  std::cout << oss.str() << std::endl;
}

} // namespace cex::common
