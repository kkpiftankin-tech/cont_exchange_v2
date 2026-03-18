#include "cex/common/uuid.hpp"

#include <array>
#include <random>
#include <sstream>
#include <iomanip>

namespace cex::common {

std::string uuid_v4() {
  // RFC4122-ish layout: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<int> dist(0, 255);

  std::array<unsigned char, 16> bytes{};
  for (auto& b : bytes) b = static_cast<unsigned char>(dist(gen));

  bytes[6] = (bytes[6] & 0x0F) | 0x40; // version 4
  bytes[8] = (bytes[8] & 0x3F) | 0x80; // variant

  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (size_t i = 0; i < bytes.size(); ++i) {
    oss << std::setw(2) << static_cast<int>(bytes[i]);
    if (i==3 || i==5 || i==7 || i==9) oss << "-";
  }
  return oss.str();
}

} // namespace cex::common
