#pragma once

#include <string>
#include <mutex>
#include <unordered_map>

#include "domain/venue_adapter.hpp"
#include "fob/execution/v1/execution.pb.h"

namespace cex::venues::app {

class ExecuteOnVenue {
 public:
  ExecuteOnVenue();
  void SetVenueSymbolOverride(const std::string& venue_id,
                              const std::string& venue_symbol);

  fob::execution::v1::ExecutionReport Run(
      const fob::execution::v1::ExecutionIntent& intent,
      domain::VenueAdapter* adapter) const;

 private:
  fob::execution::v1::ExecutionIntent BuildChildIntent(
      const fob::execution::v1::ExecutionIntent& intent,
      domain::VenueAdapter* adapter) const;

  std::string ResolveVenueSymbol(
      const fob::execution::v1::ExecutionIntent& intent,
      const std::string& venue_id) const;

  std::unordered_map<std::string, std::string> symbol_map_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::string> runtime_symbol_map_;
};

}  // namespace cex::venues::app
