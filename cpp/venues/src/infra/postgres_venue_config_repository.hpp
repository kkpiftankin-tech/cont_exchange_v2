#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cex::venues::infra {

struct VenueConfigRow {
  std::string venue_id;
  std::string adapter_mode;
  std::string ws_url;
  std::string rest_base_url;
  std::string rpc_url;
  std::string chain_id;
  std::string pool_address;
  std::string venue_symbol;
  int depth_levels{20};
  std::string curve_level{"L3"};
  bool synthetic_enabled{false};
  int stale_threshold_ms{3000};
  bool circuit_breaker_enabled{true};
  int circuit_breaker_errors{10};
  int circuit_breaker_window_ms{30000};
  int circuit_breaker_cooldown_ms{30000};
  bool is_active{true};
  std::string routing_mode{"auto"};
  int64_t updated_at_ms{0};
};

class PostgresVenueConfigRepository final {
 public:
  explicit PostgresVenueConfigRepository(std::string connection_string);

  bool EnsureSchema();
  std::vector<VenueConfigRow> LoadAll();
  bool Upsert(const VenueConfigRow& row);
  bool Delete(const std::string& venue_id);

 private:
  std::string connection_string_;
};

}  // namespace cex::venues::infra
