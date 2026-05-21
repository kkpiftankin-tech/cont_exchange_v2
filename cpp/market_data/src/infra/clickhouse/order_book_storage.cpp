#include <sstream>
#include <nlohmann/json.hpp>

#include "order_book_storage.hpp"
#include "cex/common/log.hpp"

namespace cex::market_data::infra::clickhouse {

namespace detail {

using json = nlohmann::json;

template <typename TCol, typename TVal, typename... CtorArgs>
::clickhouse::ColumnRef MakeCol(TVal&& val, CtorArgs&&... ctor_args) {
  auto col = std::make_shared<TCol>(std::forward<CtorArgs>(ctor_args)...);
  col->Append(std::forward<TVal>(val));
  return col;
}

std::string_view SourceToString(domain::MarketSource source) {
  switch (source) {
    case domain::MarketSource::Internal:
      return "internal";
    case domain::MarketSource::Binance:
      return "binance";
    case domain::MarketSource::Unknown:
      return "unknown";
  }
  return "unknown";
}

std::string SerializeDepth(const auto& depth) {
  auto arr = json::array();
  for (const auto& level : depth) {
    arr.push_back({
        {"price", static_cast<double>(level.price)},
        {"quantity", static_cast<double>(level.quantity)},
    });
  }
  return arr.dump();
}

::clickhouse::Block ToBlock(const domain::OrderBook& data) {
  using F64 = ::clickhouse::ColumnFloat64;
  using Str = ::clickhouse::ColumnString;
  using DT64 = ::clickhouse::ColumnDateTime64;

  const double volume_24h = data.volume_24h.has_value()
                                ? static_cast<double>(data.volume_24h.value())
                                : 0.0;

  const std::pair<std::string, ::clickhouse::ColumnRef> cols[] = {
      {"timestamp", MakeCol<DT64>(data.timestamp.time_since_epoch().count(), 3)},
      {"source", MakeCol<Str>(SourceToString(data.source))},
      {"symbol", MakeCol<Str>(data.symbol)},
      {"mid_price", MakeCol<F64>(data.MidPrice())},
      {"best_bid", MakeCol<F64>(data.BestBid())},
      {"best_ask", MakeCol<F64>(data.BestAsk())},
      {"bid_depth_json", MakeCol<Str>(SerializeDepth(data.bid_depth))},
      {"ask_depth_json", MakeCol<Str>(SerializeDepth(data.ask_depth))},
      {"spread", MakeCol<F64>(data.Spread())},
      {"volume_24h", MakeCol<F64>(data.volume_24h.has_value() ? static_cast<double>(*data.volume_24h) : 0.0)},
  };

  ::clickhouse::Block block;
  for (const auto& [name, col] : cols) {
    block.AppendColumn(name, col);
  }
  return block;
}

::clickhouse::ClientOptions MakeOptions(const ClickHouseConfig& config) {
  return ::clickhouse::ClientOptions()
      .SetHost(config.host)
      .SetPort(config.tcp_port)
      .SetUser(config.user)
      .SetPassword(config.password)
      .SetDefaultDatabase(config.database);
}

}  // namespace detail

const std::string kTableName = "OrderBook";

OrderBookStorage::OrderBookStorage(const ClickHouseConfig& config)
    : client_(detail::MakeOptions(config)) {
  std::ostringstream query;
  query << "CREATE TABLE IF NOT EXISTS " << kTableName << " ("
        << "timestamp       DateTime64(3),"
        << "source          String,"
        << "symbol          String,"
        << "mid_price       Float64,"
        << "best_bid        Float64,"
        << "best_ask        Float64,"
        << "bid_depth_json  String,"
        << "ask_depth_json  String,"
        << "spread          Float64,"
        << "volume_24h      Float64"
        << ") ENGINE = MergeTree() ORDER BY (timestamp, source, symbol)";
  client_.Execute(query.str());
  // Legacy schema compatibility: older versions used Nullable(Float64).
  // Keep table writable for current producer format.
  try {
    client_.Execute("ALTER TABLE " + kTableName + " MODIFY COLUMN volume_24h Float64");
  } catch (const std::exception& e) {
    cex::common::log_json("WARN", "OrderBook schema migration skipped",
                          {{"error", e.what()}});
  }
}

void OrderBookStorage::Store(const domain::OrderBook& data) {
  try {
    const auto& block = detail::ToBlock(data);
    client_.Insert(kTableName, block);
  } catch (const std::exception& e) {
    cex::common::log_json("ERROR", "Failed to store order book in ClickHouse",
                          {{"error", e.what()},
                           {"symbol", data.symbol}});
  }
}

}  // namespace cex::market_data::infra::clickhouse
