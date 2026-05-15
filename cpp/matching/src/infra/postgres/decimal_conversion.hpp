#pragma once

#include <string>
#include <string_view>

#include "cex/common/decimal.hpp"

namespace cex::matching::infra::postgres {

cex::common::Decimal ParsePgNumeric(std::string_view text_value);
std::string ToPgNumeric(const cex::common::Decimal& value);

}  // namespace cex::matching::infra::postgres
