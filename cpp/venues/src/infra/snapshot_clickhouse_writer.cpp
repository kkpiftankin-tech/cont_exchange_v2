#include "infra/snapshot_clickhouse_writer.hpp"

#include <cmath>
#include <algorithm>
#include <mutex>
#include <sstream>
#include <string>

#include <curl/curl.h>

#include "cex/common/decimal.hpp"
#include "cex/common/log.hpp"

namespace cex::venues::infra {

namespace {

size_t CurlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

std::string JsonEscape(const std::string& input) {
  std::ostringstream out;
  for (char c : input) {
    const unsigned char uc = static_cast<unsigned char>(c);
    switch (c) {
      case '\"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (uc < 0x20) {
          constexpr char kHex[] = "0123456789abcdef";
          out << "\\u00" << kHex[(uc >> 4) & 0x0f] << kHex[uc & 0x0f];
        } else {
          out << c;
        }
    }
  }
  return out.str();
}

double DecimalToDouble(const fob::common::v1::Decimal& d) {
  return static_cast<double>(d.units()) / std::pow(10.0, d.scale());
}

int64_t TimestampToUnixMs(const google::protobuf::Timestamp& ts) {
  return static_cast<int64_t>(ts.seconds()) * 1000LL +
         static_cast<int64_t>(ts.nanos()) / 1000000LL;
}

std::string DecimalArrayToJson(
    const google::protobuf::RepeatedPtrField<fob::common::v1::Decimal>& prices,
    const google::protobuf::RepeatedPtrField<fob::common::v1::Decimal>& quantities) {
  std::ostringstream out;
  out << "[";
  const int n = std::min(prices.size(), quantities.size());
  for (int i = 0; i < n; ++i) {
    if (i > 0) out << ",";
    out << "[" << DecimalToDouble(prices.Get(i)) << ","
        << DecimalToDouble(quantities.Get(i)) << "]";
  }
  out << "]";
  return out.str();
}

}  // namespace

SnapshotClickHouseWriter::SnapshotClickHouseWriter(SnapshotClickHouseConfig cfg)
    : cfg_(std::move(cfg)) {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::string SnapshotClickHouseWriter::FullTableName() const {
  return cfg_.database + "." + cfg_.table;
}

bool SnapshotClickHouseWriter::EnsureSchema() {
  const int retention_days = std::max(1, cfg_.retention_days);
  const std::string create_db = "CREATE DATABASE IF NOT EXISTS " + cfg_.database;
  const std::string create_table =
      "CREATE TABLE IF NOT EXISTS " + FullTableName() + " ("
      "venue_id String,"
      "symbol String,"
      "event_time_ms Int64,"
      "best_bid Float64,"
      "best_ask Float64,"
      "mid_price Float64,"
      "spread Float64,"
      "bid_depth_json String,"
      "ask_depth_json String,"
      "maker_fee Float64,"
      "taker_fee Float64,"
      "tick_size Float64,"
      "lot_size Float64,"
      "status LowCardinality(String),"
      "volume_24h Float64,"
      "source String,"
      "correlation_id String,"
      "ingested_at DateTime DEFAULT now()"
      ") ENGINE = MergeTree "
      "ORDER BY (venue_id, symbol, event_time_ms) "
      "TTL toDateTime(event_time_ms / 1000) + INTERVAL " + std::to_string(retention_days) +
      " DAY DELETE";
  const std::string apply_ttl =
      "ALTER TABLE " + FullTableName() + " MODIFY TTL toDateTime(event_time_ms / 1000) + "
      "INTERVAL " + std::to_string(retention_days) + " DAY DELETE";

  return ExecQuery(create_db) && ExecQuery(create_table) && ExecQuery(apply_ttl);
}

bool SnapshotClickHouseWriter::SaveSnapshot(
    const fob::venue::v1::VenueSnapshot& snapshot) {
  const int64_t event_time_ms = snapshot.has_timestamp()
      ? TimestampToUnixMs(snapshot.timestamp())
      : (snapshot.has_meta() && snapshot.meta().has_ts_event()
             ? TimestampToUnixMs(snapshot.meta().ts_event()) : 0);

  const std::string bid_depth = DecimalArrayToJson(
      snapshot.bid_prices(), snapshot.bid_quantities());
  const std::string ask_depth = DecimalArrayToJson(
      snapshot.ask_prices(), snapshot.ask_quantities());

  std::ostringstream row;
  row << "{";
  row << "\"venue_id\":\"" << JsonEscape(snapshot.venue_id()) << "\",";
  row << "\"symbol\":\"" << JsonEscape(snapshot.instrument().symbol()) << "\",";
  row << "\"event_time_ms\":" << event_time_ms << ",";
  row << "\"best_bid\":" << DecimalToDouble(snapshot.best_bid()) << ",";
  row << "\"best_ask\":" << DecimalToDouble(snapshot.best_ask()) << ",";
  row << "\"mid_price\":" << DecimalToDouble(snapshot.mid_price()) << ",";
  row << "\"spread\":" << DecimalToDouble(snapshot.spread()) << ",";
  row << "\"bid_depth_json\":\"" << JsonEscape(bid_depth) << "\",";
  row << "\"ask_depth_json\":\"" << JsonEscape(ask_depth) << "\",";
  row << "\"maker_fee\":" << DecimalToDouble(snapshot.maker_fee()) << ",";
  row << "\"taker_fee\":" << DecimalToDouble(snapshot.taker_fee()) << ",";
  row << "\"tick_size\":" << DecimalToDouble(snapshot.tick_size()) << ",";
  row << "\"lot_size\":" << DecimalToDouble(snapshot.lot_size()) << ",";
  row << "\"status\":\"" << JsonEscape(snapshot.status()) << "\",";
  row << "\"volume_24h\":" << DecimalToDouble(snapshot.volume_24h()) << ",";
  row << "\"source\":\"" << JsonEscape(
      snapshot.has_meta() ? snapshot.meta().source() : "") << "\",";
  row << "\"correlation_id\":\"" << JsonEscape(
      snapshot.has_meta() ? snapshot.meta().correlation_id() : "") << "\"";
  row << "}\n";

  const std::string query =
      "INSERT INTO " + FullTableName() + " FORMAT JSONEachRow";
  const bool ok = ExecQuery(query, row.str());
  if (ok) {
    cex::common::log_json("INFO", "Stored venue snapshot in ClickHouse",
                          {{"service", "venues"},
                           {"component", "snapshot_clickhouse_writer"},
                           {"participant", "ClickHouse"},
                           {"stage", "store_snapshot"},
                           {"table", FullTableName()},
                           {"venue", snapshot.venue_id()},
                           {"symbol", snapshot.instrument().symbol()},
                           {"event_time_ms", std::to_string(event_time_ms)},
                           {"status", snapshot.status()},
                           {"best_bid", DecimalToDouble(snapshot.best_bid()) == 0.0
                                            ? "0"
                                            : cex::common::Decimal::from_proto(
                                                  snapshot.best_bid()).to_string()},
                           {"best_ask", DecimalToDouble(snapshot.best_ask()) == 0.0
                                            ? "0"
                                            : cex::common::Decimal::from_proto(
                                                  snapshot.best_ask()).to_string()},
                           {"volume_24h", snapshot.has_volume_24h()
                                             ? cex::common::Decimal::from_proto(
                                                   snapshot.volume_24h()).to_string()
                                             : "0"},
                           {"source_file",
                            "cpp/venues/src/infra/snapshot_clickhouse_writer.cpp"}});
  }
  return ok;
}

bool SnapshotClickHouseWriter::ExecQuery(const std::string& query,
                                         const std::string& body) {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    cex::common::log_json("ERROR", "curl_easy_init failed");
    return false;
  }

  char* encoded = curl_easy_escape(curl, query.c_str(), static_cast<int>(query.size()));
  if (encoded == nullptr) {
    cex::common::log_json("ERROR", "Failed to URL-encode ClickHouse query");
    curl_easy_cleanup(curl);
    return false;
  }

  std::string url = cfg_.url + "/?query=" + encoded;
  curl_free(encoded);

  std::string response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, cfg_.timeout_ms);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  if (!cfg_.user.empty()) {
    curl_easy_setopt(curl, CURLOPT_USERNAME, cfg_.user.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg_.password.c_str());
  }

  CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    cex::common::log_json("ERROR", "ClickHouse request failed",
                          {{"err", curl_easy_strerror(rc)}});
    return false;
  }
  if (http_code < 200 || http_code >= 300) {
    cex::common::log_json("ERROR", "ClickHouse HTTP error",
                          {{"code", std::to_string(http_code)},
                           {"response", response}});
    return false;
  }

  return true;
}

}  // namespace cex::venues::infra
