#include "domain/venue_endpoints.hpp"

#include <algorithm>
#include <cctype>

namespace cex::venues::domain {

CexEndpoints SeedCexEndpoints(const std::string& venue_id, const CexEndpointDefaults& defaults) {
  std::string id = venue_id;
  std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (id.find("coinbase") != std::string::npos) return defaults.coinbase;
  if (id.find("kraken") != std::string::npos) return defaults.kraken;
  if (id.find("okx") != std::string::npos) return defaults.okx;
  return defaults.binance;
}

}  // namespace cex::venues::domain
