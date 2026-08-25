#include "infra/clickhouse/ch_snapshot_storage.hpp"

#include <chrono>
#include <ctime>
#include <sstream>
#include <vector>

#include <curl/curl.h>

#include "cex/common/log.hpp"

namespace {

size_t CurlWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

}  // namespace

namespace cex::market_data::infra::clickhouse {

namespace {

std::string ToClickHouseTimestamp(domain::Timestamp ts) {
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      ts.time_since_epoch()).count();
  const auto sec = ms / 1000;
  const auto frac = ms % 1000;
  std::time_t t = static_cast<std::time_t>(sec);
  struct tm tm_info{};
  gmtime_r(&t, &tm_info);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
  char result[40];
  snprintf(result, sizeof(result), "%s.%03lld", buf, static_cast<long long>(frac));
  return result;
}

std::string SourceToStr(domain::DataSource src) {
  switch (src) {
    case domain::DataSource::Cex:       return "cex";
    case domain::DataSource::Dex:       return "dex";
    case domain::DataSource::Composite: return "composite";
    default:                            return "internal";
  }
}

// Escapes single quotes for ClickHouse string literals.
std::string Esc(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '\'') out += "\\'";
    else           out += c;
  }
  return out;
}

}  // namespace

ChSnapshotStorage::ChSnapshotStorage(ClickHouseConfig cfg) : cfg_(std::move(cfg)) {}

bool ChSnapshotStorage::EnsureSchema() {
  // DDL зафиксирован в docs/07-data/marketdata-snapshots.md
  // и infra/clickhouse/V001__marketdata_snapshots.sql
  // Здесь проверяем только наличие таблиц (CREATE TABLE IF NOT EXISTS).
  const std::string create_snapshots = R"(
CREATE TABLE IF NOT EXISTS marketdata_snapshots (
    snapshot_id       String,
    asset             String,
    mid               String,
    best_bid          String,
    best_ask          String,
    spread            String,
    spread_bps        String,
    volume_24h        String,
    volume_quote_24h  String,
    bid_depth         String,
    ask_depth         String,
    clear_price       String,
    executed_rate     String,
    source            String,
    stale             UInt8,
    batch_id          String,
    timestamp         DateTime64(3, 'UTC')
) ENGINE = MergeTree
ORDER BY (asset, timestamp)
TTL toDateTime(timestamp) + INTERVAL 90 DAY
)";

  const std::string create_spreads = R"(
CREATE TABLE IF NOT EXISTS effective_spreads (
    fill_id               String,
    asset                 String,
    exec_price            String,
    mid_at_exec           String,
    effective_spread      String,
    effective_spread_bps  String,
    batch_id              String,
    timestamp             DateTime64(3, 'UTC')
) ENGINE = MergeTree
ORDER BY (asset, timestamp)
TTL toDateTime(timestamp) + INTERVAL 365 DAY
)";

  return ExecInsert(create_snapshots) && ExecInsert(create_spreads);
}

bool ChSnapshotStorage::SaveSnapshot(const domain::MarketDataSnapshot& snap) {
  std::ostringstream q;
  // Depth сериализуем как JSON-строки для простоты MVP
  std::ostringstream bid_json, ask_json;
  bid_json << "[";
  for (size_t i = 0; i < snap.bid_depth.size(); ++i) {
    if (i > 0) bid_json << ",";
    bid_json << "{\"price\":\"" << snap.bid_depth[i].price.to_string()
             << "\",\"qty\":\"" << snap.bid_depth[i].qty.to_string() << "\"}";
  }
  bid_json << "]";
  ask_json << "[";
  for (size_t i = 0; i < snap.ask_depth.size(); ++i) {
    if (i > 0) ask_json << ",";
    ask_json << "{\"price\":\"" << snap.ask_depth[i].price.to_string()
             << "\",\"qty\":\"" << snap.ask_depth[i].qty.to_string() << "\"}";
  }
  ask_json << "]";

  q << "INSERT INTO marketdata_snapshots VALUES ("
    << "'" << Esc(snap.snapshot_id) << "',"
    << "'" << Esc(snap.asset) << "',"
    << "'" << snap.mid.to_string() << "',"
    << "'" << snap.best_bid.to_string() << "',"
    << "'" << snap.best_ask.to_string() << "',"
    << "'" << snap.spread.to_string() << "',"
    << "'" << snap.spread_bps.to_string() << "',"
    << "'" << snap.volume_24h.to_string() << "',"
    << "'" << snap.volume_quote_24h.to_string() << "',"
    << "'" << Esc(bid_json.str()) << "',"
    << "'" << Esc(ask_json.str()) << "',"
    << "'" << snap.clear_price.to_string() << "',"
    << "'" << snap.executed_rate.to_string() << "',"
    << "'" << SourceToStr(snap.source) << "',"
    << (snap.stale ? 1 : 0) << ","
    << "'" << Esc(snap.batch_id) << "',"
    << "'" << ToClickHouseTimestamp(snap.timestamp) << "'"
    << ")";

  const bool ok = ExecInsert(q.str());
  if (!ok) {
    cex::common::log_json("ERROR", "ChSnapshotStorage: SaveSnapshot failed",
                          {{"asset", snap.asset}, {"batch_id", snap.batch_id}});
  }
  return ok;
}

bool ChSnapshotStorage::SaveRecord(const domain::EffectiveSpreadRecord& record) {
  std::ostringstream q;
  q << "INSERT INTO effective_spreads VALUES ("
    << "'" << Esc(record.fill_id) << "',"
    << "'" << Esc(record.asset) << "',"
    << "'" << record.exec_price.to_string() << "',"
    << "'" << record.mid_at_exec.to_string() << "',"
    << "'" << record.effective_spread.to_string() << "',"
    << "'" << record.effective_spread_bps.to_string() << "',"
    << "'" << Esc(record.batch_id) << "',"
    << "'" << ToClickHouseTimestamp(record.timestamp) << "'"
    << ")";

  const bool ok = ExecInsert(q.str());
  if (!ok) {
    cex::common::log_json("ERROR", "ChSnapshotStorage: SaveRecord failed",
                          {{"asset", record.asset}, {"fill_id", record.fill_id}});
  }
  return ok;
}

common::Decimal ChSnapshotStorage::GetVolume24h(const std::string& asset,
                                                uint32_t window_sec) const {
  // SELECT sum(exec_qty) FROM fills WHERE asset = ? AND timestamp > now() - interval
  // fills хранит exec_qty как String (из Decimal) — суммируем как float64
  const std::string q =
      "SELECT sum(toFloat64OrZero(exec_qty)) FROM fills"
      " WHERE instrument_symbol='" + Esc(asset) +
      "' AND timestamp > now() - INTERVAL " + std::to_string(window_sec) + " SECOND";

  std::string body;
  if (!ExecSelect(q, &body) || body.empty()) return common::Decimal::zero();

  // Ответ — одно число (TSV)
  try {
    double v = std::stod(body);
    return {static_cast<int64_t>(v * 1e8), 8};
  } catch (...) {
    return common::Decimal::zero();
  }
}

std::optional<domain::MarketDataSnapshot> ChSnapshotStorage::GetLatestSnapshot(
    const std::string& asset) const {
  const std::string q =
      "SELECT snapshot_id, asset, mid, best_bid, best_ask, spread, spread_bps, "
      "volume_24h, clear_price, executed_rate, source, stale, batch_id "
      "FROM marketdata_snapshots WHERE asset='" + Esc(asset) +
      "' ORDER BY timestamp DESC LIMIT 1 FORMAT TSV";

  std::string body;
  if (!ExecSelect(q, &body) || body.empty()) return std::nullopt;
  return ParseSnapshotRow(body);
}

std::vector<domain::MarketDataSnapshot> ChSnapshotStorage::GetSnapshotHistory(
    const std::string& asset,
    domain::Timestamp from,
    domain::Timestamp to,
    uint32_t limit) const {
  const auto from_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      from.time_since_epoch()).count();
  const auto to_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      to.time_since_epoch()).count();

  const std::string q =
      "SELECT snapshot_id, asset, mid, best_bid, best_ask, spread, spread_bps, "
      "volume_24h, clear_price, executed_rate, source, stale, batch_id "
      "FROM marketdata_snapshots WHERE asset='" + Esc(asset) +
      "' AND toUnixTimestamp64Milli(timestamp) BETWEEN " +
      std::to_string(from_ms) + " AND " + std::to_string(to_ms) +
      " ORDER BY timestamp DESC LIMIT " + std::to_string(limit) + " FORMAT TSV";

  std::string body;
  if (!ExecSelect(q, &body) || body.empty()) return {};

  std::vector<domain::MarketDataSnapshot> result;
  std::istringstream ss(body);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    auto snap = ParseSnapshotRow(line);
    if (snap) result.push_back(std::move(*snap));
  }
  return result;
}

std::vector<domain::EffectiveSpreadRecord> ChSnapshotStorage::GetSpreadHistory(
    const std::string& asset,
    domain::Timestamp from,
    domain::Timestamp to,
    uint32_t limit) const {
  const auto from_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      from.time_since_epoch()).count();
  const auto to_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      to.time_since_epoch()).count();

  const std::string q =
      "SELECT fill_id, asset, exec_price, mid_at_exec, effective_spread, "
      "effective_spread_bps, batch_id "
      "FROM effective_spreads WHERE asset='" + Esc(asset) +
      "' AND toUnixTimestamp64Milli(timestamp) BETWEEN " +
      std::to_string(from_ms) + " AND " + std::to_string(to_ms) +
      " ORDER BY timestamp DESC LIMIT " + std::to_string(limit) + " FORMAT TSV";

  std::string body;
  if (!ExecSelect(q, &body) || body.empty()) return {};

  // TSV → EffectiveSpreadRecord
  std::vector<domain::EffectiveSpreadRecord> result;
  std::istringstream ss(body);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    std::istringstream ls(line);
    std::string fill_id, rec_asset, exec_price, mid_at_exec, eff_spread, eff_bps, batch_id;
    if (!std::getline(ls, fill_id, '\t')) continue;
    std::getline(ls, rec_asset,   '\t');
    std::getline(ls, exec_price,  '\t');
    std::getline(ls, mid_at_exec, '\t');
    std::getline(ls, eff_spread,  '\t');
    std::getline(ls, eff_bps,     '\t');
    std::getline(ls, batch_id,    '\t');

    domain::EffectiveSpreadRecord rec;
    rec.fill_id   = fill_id;
    rec.asset     = rec_asset;
    rec.batch_id  = batch_id;
    rec.timestamp = std::chrono::system_clock::now();  // approx
    // Decimal из строки: stores as units with scale=8
    auto parseD = [](const std::string& s) -> common::Decimal {
      try {
        double v = std::stod(s);
        return {static_cast<int64_t>(v * 1e8), 8};
      } catch (...) { return common::Decimal::zero(); }
    };
    rec.exec_price          = parseD(exec_price);
    rec.mid_at_exec         = parseD(mid_at_exec);
    rec.effective_spread    = parseD(eff_spread);
    rec.effective_spread_bps = parseD(eff_bps);
    result.push_back(rec);
  }
  return result;
}

std::optional<domain::MarketDataSnapshot> ChSnapshotStorage::ParseSnapshotRow(
    const std::string& row) {
  // TSV колонки: snapshot_id, asset, mid, best_bid, best_ask, spread, spread_bps,
  //              volume_24h, clear_price, executed_rate, source, stale, batch_id
  std::istringstream ls(row);
  auto readCol = [&](std::string& out) { return static_cast<bool>(std::getline(ls, out, '\t')); };
  auto parseD  = [](const std::string& s) -> common::Decimal {
    try {
      double v = std::stod(s);
      return {static_cast<int64_t>(v * 1e8), 8};
    } catch (...) { return common::Decimal::zero(); }
  };

  domain::MarketDataSnapshot snap;
  std::string mid, best_bid, best_ask, spread, spread_bps, vol24, clear, exec_rate, src, stale_s;
  if (!readCol(snap.snapshot_id)) return std::nullopt;
  readCol(snap.asset); readCol(mid); readCol(best_bid); readCol(best_ask);
  readCol(spread); readCol(spread_bps); readCol(vol24); readCol(clear);
  readCol(exec_rate); readCol(src); readCol(stale_s); readCol(snap.batch_id);

  snap.mid           = parseD(mid);
  snap.best_bid      = parseD(best_bid);
  snap.best_ask      = parseD(best_ask);
  snap.spread        = parseD(spread);
  snap.spread_bps    = parseD(spread_bps);
  snap.volume_24h    = parseD(vol24);
  snap.clear_price   = parseD(clear);
  snap.executed_rate = parseD(exec_rate);
  snap.stale         = (stale_s == "1");
  snap.timestamp     = std::chrono::system_clock::now();

  if      (src == "cex")       snap.source = domain::DataSource::Cex;
  else if (src == "dex")       snap.source = domain::DataSource::Dex;
  else if (src == "composite") snap.source = domain::DataSource::Composite;
  else                         snap.source = domain::DataSource::Internal;

  return snap;
}

bool ChSnapshotStorage::ExecInsert(const std::string& query) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    cex::common::log_json("ERROR", "ChSnapshotStorage: curl_easy_init failed");
    return false;
  }

  char* encoded = curl_easy_escape(curl, query.c_str(), static_cast<int>(query.size()));
  if (!encoded) {
    curl_easy_cleanup(curl);
    return false;
  }

  const std::string url = "http://" + cfg_.host + ":" + std::to_string(cfg_.port) +
                          "/?query=" + std::string(encoded);
  curl_free(encoded);

  std::string response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  if (!cfg_.user.empty()) {
    curl_easy_setopt(curl, CURLOPT_USERNAME, cfg_.user.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg_.password.c_str());
  }

  CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    cex::common::log_json("ERROR", "ChSnapshotStorage INSERT failed",
                          {{"err", curl_easy_strerror(rc)}});
    return false;
  }
  if (http_code < 200 || http_code >= 300) {
    cex::common::log_json("ERROR", "ChSnapshotStorage INSERT non-2xx",
                          {{"http_code", std::to_string(http_code)},
                           {"body", response.substr(0, 200)}});
    return false;
  }
  return true;
}

bool ChSnapshotStorage::ExecSelect(const std::string& query, std::string* out) const {
  CURL* curl = curl_easy_init();
  if (!curl) return false;

  char* encoded = curl_easy_escape(curl, query.c_str(), static_cast<int>(query.size()));
  if (!encoded) { curl_easy_cleanup(curl); return false; }

  const std::string url = "http://" + cfg_.host + ":" + std::to_string(cfg_.port) +
                          "/?query=" + std::string(encoded);
  curl_free(encoded);

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
  if (!cfg_.user.empty()) {
    curl_easy_setopt(curl, CURLOPT_USERNAME, cfg_.user.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg_.password.c_str());
  }

  CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);
  return rc == CURLE_OK && http_code >= 200 && http_code < 300;
}

}  // namespace cex::market_data::infra::clickhouse
