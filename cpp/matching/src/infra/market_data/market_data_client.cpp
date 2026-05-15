#include "infra/market_data/market_data_client.hpp"

#include <unordered_set>

#include "cex/common/decimal.hpp"
#include "cex/common/log.hpp"

namespace cex::matching::infra {

namespace {

fob::common::v1::Decimal ResolveReferencePrice(
    const fob::marketdata::v1::Ticker& ticker) {
  if (ticker.has_last() && ticker.last().units() != 0) {
    return ticker.last();
  }
  if (ticker.has_bid() && ticker.has_ask()) {
    auto mid = cex::common::Decimal::add(
        cex::common::Decimal::from_proto(ticker.bid()),
        cex::common::Decimal::from_proto(ticker.ask()));
    mid.units /= 2;
    return mid.to_proto();
  }
  return ticker.bid();
}

}  // namespace

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

  for (const auto& symbol : symbols) {
    fob::marketdata::v1::GetLastTickerRequest req;
    req.set_venue(venue_);
    req.set_symbol(symbol);
    fob::marketdata::v1::GetLastTickerResponse resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(500));

    auto status = stub_->GetLastTicker(&ctx, req, &resp);
    if (!status.ok() || !resp.found() || !resp.has_ticker()) {
      cex::common::log_json("WARN", "No reference ticker for symbol",
                            {{"symbol", symbol},
                             {"grpc_ok", status.ok() ? "true" : "false"}});
      continue;
    }

    result[symbol] = ResolveReferencePrice(resp.ticker());
  }

  return result;
}

}  // namespace cex::matching::infra

