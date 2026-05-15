#include "infra/postgres/decimal_conversion.hpp"

#include <cctype>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace cex::matching::infra::postgres {

namespace {

std::string_view trim_ascii_space(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.remove_suffix(1);
  }
  return text;
}

}  // namespace

cex::common::Decimal ParsePgNumeric(std::string_view text_value) {
  text_value = trim_ascii_space(text_value);
  if (text_value.empty()) {
    throw std::invalid_argument("NUMERIC text value is empty");
  }

  bool negative = false;
  std::size_t idx = 0;
  if (text_value.front() == '+' || text_value.front() == '-') {
    negative = text_value.front() == '-';
    idx = 1;
  }

  if (idx == text_value.size()) {
    throw std::invalid_argument("NUMERIC text value has sign without digits");
  }

  bool has_digit = false;
  bool has_dot = false;
  std::int32_t scale = 0;
  __int128 acc = 0;

  constexpr __int128 kInt64Max = static_cast<__int128>(std::numeric_limits<std::int64_t>::max());
  constexpr __int128 kInt64MinAbs = kInt64Max + 1;

  for (; idx < text_value.size(); ++idx) {
    const char ch = text_value[idx];
    if (ch == '.') {
      if (has_dot) {
        throw std::invalid_argument("NUMERIC text value contains multiple dots");
      }
      has_dot = true;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
      throw std::invalid_argument("NUMERIC text value contains non-digit characters");
    }

    has_digit = true;
    acc = acc * 10 + static_cast<int>(ch - '0');
    if ((!negative && acc > kInt64Max) || (negative && acc > kInt64MinAbs)) {
      throw std::overflow_error("NUMERIC value does not fit into Decimal::units (int64)");
    }
    if (has_dot) {
      ++scale;
    }
  }

  if (!has_digit) {
    throw std::invalid_argument("NUMERIC text value does not contain digits");
  }

  std::int64_t units = 0;
  if (negative) {
    if (acc == kInt64MinAbs) {
      units = std::numeric_limits<std::int64_t>::min();
    } else {
      units = -static_cast<std::int64_t>(acc);
    }
  } else {
    units = static_cast<std::int64_t>(acc);
  }

  return cex::common::Decimal{units, scale};
}

std::string ToPgNumeric(const cex::common::Decimal& value) {
  return value.to_string();
}

}  // namespace cex::matching::infra::postgres
