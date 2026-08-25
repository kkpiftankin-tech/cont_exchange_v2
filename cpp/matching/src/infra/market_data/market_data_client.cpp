#include "infra/market_data/market_data_client.hpp"

#include <algorithm>
#include <unordered_set>

#include "cex/common/decimal.hpp"
#include "cex/common/log.hpp"

namespace cex::matching::infra {

MarketDataClient::MarketDataClient(const std::string& target, std::string venue)
    : venue_(std::move(venue)) {
  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  stub_ = fob::marketdata::v1::MarketDataService::NewStub(channel);
  cex::common::log_json("INFO", "Matching MarketDataClient created",
                        {{"target", target}, {"venue", venue_}});
}

std::unordered_map<std::string, fob::common::v1::Decimal>
MarketDataClient::GetReferencePrices(
    const std::vector<domain::FlowOrder>& orders) {
  std::unordered_map<std::string, fob::common::v1::Decimal> result;
  std::unordered_set<std::string> symbols;
  symbols.reserve(orders.size());
  for (const auto& order : orders) {
    for (const auto &leg : order.legs)
      symbols.insert(leg.instrument_symbol);
  }

  if (symbols.empty()) return result;

  // F5-11: один батч-RPC GetReferencePrices(assets[], ts_batch) на все символы —
  // единый логический момент вместо per-symbol GetLastTicker. Символы
  // нормализуем ("BTC/USDT" → "BTCUSDT", формат кэша market_data) и держим
  // обратное соответствие, чтобы вернуть цены под исходными символами matching.
  std::unordered_map<std::string, std::string> norm_to_raw;
  fob::marketdata::v1::GetReferencePricesRequest req;
  req.mutable_meta()->set_source("matching");
  for (const auto& s : symbols) {
    std::string norm = s;
    norm.erase(std::remove(norm.begin(), norm.end(), '/'), norm.end());
    norm_to_raw[norm] = s;
    req.add_assets(norm);
  }

  fob::marketdata::v1::GetReferencePricesResponse resp;
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(500));
  auto status = stub_->GetReferencePrices(&ctx, req, &resp);
  if (!status.ok()) {
    cex::common::log_json("WARN", "GetReferencePrices RPC failed",
                          {{"grpc_msg", status.error_message()},
                           {"symbols", std::to_string(symbols.size())}});
    return result;
  }

  for (const auto& rp : resp.prices()) {
    const auto it = norm_to_raw.find(rp.asset());
    const std::string& raw = (it != norm_to_raw.end()) ? it->second : rp.asset();
    // reference price = mid; fallback на среднее best_bid/best_ask
    if (rp.has_mid() && rp.mid().units() != 0) {
      result[raw] = rp.mid();
    } else if (rp.has_best_bid() && rp.has_best_ask()) {
      auto mid = cex::common::Decimal::add(
          cex::common::Decimal::from_proto(rp.best_bid()),
          cex::common::Decimal::from_proto(rp.best_ask()));
      mid.units /= 2;
      result[raw] = mid.to_proto();
    }
  }

  return result;
}

}  // namespace cex::matching::infra

