// =============================================================================
// Реализация генератора UUID v4. См. предупреждение о криптостойкости в .hpp.
// =============================================================================

#include "cex/common/uuid.hpp"

#include <array>
#include <random>
#include <sstream>
#include <iomanip>

namespace cex::common {

std::string uuid_v4() {
  // RFC4122 v4: 16 случайных байт с проставленными битами версии и варианта.
  // Формат вывода: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
  // (где '4' — версия, 'y' — биты 10xx — variant 1, RFC4122).
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<int> dist(0, 255);

  std::array<unsigned char, 16> bytes{};
  for (auto& b : bytes) b = static_cast<unsigned char>(dist(gen));

  // Версия 4: верхние 4 бита 7-го байта = 0100.
  bytes[6] = (bytes[6] & 0x0F) | 0x40;
  // Variant 1 (RFC4122): верхние 2 бита 9-го байта = 10.
  bytes[8] = (bytes[8] & 0x3F) | 0x80;

  // Печатаем 16 байт как 32 hex-символа с дефисами в стандартных позициях.
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (size_t i = 0; i < bytes.size(); ++i) {
    oss << std::setw(2) << static_cast<int>(bytes[i]);
    if (i==3 || i==5 || i==7 || i==9) oss << "-";
  }
  return oss.str();
}

} // namespace cex::common
