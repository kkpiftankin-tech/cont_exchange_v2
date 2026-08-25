#pragma once

#include <string>

#include "app/liquidity_curve_producer.hpp"

namespace cex::venues::infra {

class PostgresSyntheticOrderRepository final
    : public app::ISyntheticOrderRepository {
 public:
  explicit PostgresSyntheticOrderRepository(std::string connection_string);

  bool SaveSyntheticOrder(
      const fob::orders::v1::SyntheticFlowOrder& order) override;

 private:
  std::string connection_string_;
};

}  // namespace cex::venues::infra
