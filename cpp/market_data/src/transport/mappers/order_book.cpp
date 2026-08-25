#include "order_book.hpp"

#include <chrono>

#include <boost/uuid/string_generator.hpp>

#include <google/protobuf/util/time_util.h>

#include "fob/marketdata/v1/marketdata_raw.grpc.pb.h"
#include "fob/matching/v1/batch.grpc.pb.h"
#include "domain/entities/order_book.hpp"

namespace cex::market_data::transport::mappers {

namespace detail {

common::Decimal FromProto(const fob::common::v1::Decimal& d) {
  return common::Decimal{d.units(), d.scale()};
}

domain::Timestamp FromProto(const ::google::protobuf::Timestamp& t) {
  const auto duration = std::chrono::seconds{t.seconds()} +
                        std::chrono::nanoseconds{t.nanos()};
  return domain::Timestamp{
      std::chrono::duration_cast<domain::Timestamp::duration>(duration)};
}

domain::Side FromString(fob::common::v1::Side side) {
  if (side == fob::common::v1::Side::SIDE_BUY) return domain::Side::Buy;
  if (side == fob::common::v1::Side::SIDE_SELL) return domain::Side::Sell;
  if (side == fob::common::v1::Side::SIDE_UNSPECIFIED) return domain::Side::Unknown;
  throw std::invalid_argument("Unsupported Side");
}

domain::LiquiditySource LiquiditySourceFromString(const std::string& source) {
  if (source.empty() || source == "internal") return domain::LiquiditySource::Internal;
  if (source == "cex_hedge") return domain::LiquiditySource::CexHedge;
  if (source == "dex_hedge") return domain::LiquiditySource::DexHedge;
  if (source == "epsilon_mm") return domain::LiquiditySource::EpsilonMm;
  return domain::LiquiditySource::Unknown;
}

domain::MarketSource FromString(const std::string& s) {
  if (s == "internal") return domain::MarketSource::Internal;
  if (s == "binance") return domain::MarketSource::Binance;
  if (s.empty()) return domain::MarketSource::Unknown;
  return domain::MarketSource::Unknown;
}

std::string ToString(domain::MarketSource source) {
  switch (source) {
    case domain::MarketSource::Binance:
      return "binance";
    case domain::MarketSource::Internal:
      return "internal";
    case domain::MarketSource::Unknown:
      return "unknown";
  }
  return "unknown";
}

google::protobuf::Timestamp ToProto(const domain::Timestamp& ts) {
  auto nanos =
      std::chrono::time_point_cast<std::chrono::nanoseconds>(ts).time_since_epoch().count();
  return google::protobuf::util::TimeUtil::NanosecondsToTimestamp(nanos);
}

fob::common::v1::Instrument ToProtoInstrument(const std::string& symbol) {
  fob::common::v1::Instrument inst;
  inst.set_symbol(symbol);

  auto slash = symbol.find('/');
  if (slash != std::string::npos) {
    inst.set_base(symbol.substr(0, slash));
    inst.set_quote(symbol.substr(slash + 1));
  }

  return inst;
}

fob::marketdata::v1::PriceLevel ToProto(const domain::PriceLevel& pl) {
  fob::marketdata::v1::PriceLevel proto;
  *proto.mutable_price() = pl.price.to_proto();
  *proto.mutable_amount() = pl.quantity.to_proto();
  return proto;
}

}  // namespace detail

domain::OrderBook FromProto(const fob::marketdata::v1::MarketDataRaw& data) {
  const auto& raw = data.orderbook();

  domain::OrderBook book;
  book.timestamp = detail::FromProto(raw.timestamp());
  book.nonce = raw.nonce();
  book.source = detail::FromString(raw.venue());
  book.symbol = raw.instrument().symbol();

  for (const auto& b : raw.bids()) {
    domain::PriceLevel level;
    level.price = detail::FromProto(b.price());
    level.quantity = detail::FromProto(b.amount());
    book.bid_depth.insert(level);
  }

  for (const auto& a : raw.asks()) {
    domain::PriceLevel level;
    level.price = detail::FromProto(a.price());
    level.quantity = detail::FromProto(a.amount());
    book.ask_depth.insert(level);
  }

  return book;
}

domain::OrderBook FromProto(const fob::venue::v1::VenueSnapshot& snap, uint64_t nonce) {
  domain::OrderBook book;

  book.timestamp = detail::FromProto(snap.timestamp());
  book.nonce = nonce;
  book.source = detail::FromString(snap.venue_id());
  book.symbol = snap.instrument().symbol();

  if (snap.bid_prices_size() != snap.bid_quantities_size()) {
    throw std::invalid_argument("VenueSnapshot invalid: bid lengths mismatch");
  }
  for (int i = 0; i < snap.bid_prices_size(); ++i) {
    domain::PriceLevel level;
    level.price = detail::FromProto(snap.bid_prices()[i]);
    level.quantity = detail::FromProto(snap.bid_quantities()[i]);
    book.bid_depth.insert(level);
  }

  if (snap.ask_prices_size() != snap.ask_quantities_size()) {
    throw std::invalid_argument("VenueSnapshot invalid: ask lengths mismatch");
  }
  for (int i = 0; i < snap.ask_prices_size(); ++i) {
    domain::PriceLevel level;
    level.price = detail::FromProto(snap.ask_prices()[i]);
    level.quantity = detail::FromProto(snap.ask_quantities()[i]);
    book.ask_depth.insert(level);
  }

  book.volume_24h = detail::FromProto(snap.volume_24h());

  return book;
}

fob::marketdata::v1::OrderBookSnapshot ToProto(const domain::OrderBook& book) {
  fob::marketdata::v1::OrderBookSnapshot snapshot;
  snapshot.set_venue(detail::ToString(book.source));
  snapshot.set_nonce(book.nonce);
  *snapshot.mutable_timestamp() = detail::ToProto(book.timestamp);
  *snapshot.mutable_instrument() = detail::ToProtoInstrument(book.symbol);
  for (const auto& bid : book.bid_depth) {
    *snapshot.mutable_bids()->Add() = detail::ToProto(bid);
  }
  for (const auto& ask : book.ask_depth) {
    *snapshot.mutable_asks()->Add() = detail::ToProto(ask);
  }
  return snapshot;
}

std::vector<domain::FillEvent> ExtractFillEvents(const fob::matching::v1::BatchResult& result) {
  std::vector<domain::FillEvent> events;
  boost::uuids::string_generator gen;

  for (const auto& e : result.fills()) {
    if (e.fee().cost().currency() != e.instrument().base()) {
      throw std::invalid_argument("Support fee only in quote");
    }

    events.push_back(domain::FillEvent{
        boost::uuids::uuid(),
        gen(result.batch_id()),
        gen(e.order_id()),
        gen(e.user_id()),
        e.instrument().symbol(),
        detail::FromString(e.side()),
        "",
        detail::FromProto(e.executed_qty()),
        detail::FromProto(e.price()),
        detail::LiquiditySourceFromString(e.liquidity_source()),
        common::Decimal::mul(detail::FromProto(e.fee().cost().amount()),
                             detail::FromProto(e.fee().rate())),
        detail::FromProto(result.timestamp()),
    });
  }
  return events;
}

}  // namespace cex::market_data::transport::mappers
