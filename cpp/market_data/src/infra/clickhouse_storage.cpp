#include "infra/clickhouse_storage.hpp"

#include <cmath>
#include <mutex>
#include <sstream>
#include <string>

#include <curl/curl.h>

#include "cex/common/decimal.hpp"
#include "cex/common/log.hpp"

namespace cex::market_data::infra {

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
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (uc < 0x20) {
          out << "\\u00";
          constexpr char kHex[] = "0123456789abcdef";
          out << kHex[(uc >> 4) & 0x0f] << kHex[uc & 0x0f];
        } else {
          out << c;
        }
    }
  }
  return out.str();
}

double DecimalToDouble(const fob::common::v1::Decimal& decimal) {
  return static_cast<double>(decimal.units()) / std::pow(10.0, decimal.scale());
}

int64_t TimestampToUnixMs(const google::protobuf::Timestamp& ts) {
  return static_cast<int64_t>(ts.seconds()) * 1000LL +
         static_cast<int64_t>(ts.nanos()) / 1000000LL;
}

std::string SideToString(fob::common::v1::Side side) {
  switch (side) {
    case fob::common::v1::SIDE_BUY: return "buy";
    case fob::common::v1::SIDE_SELL: return "sell";
    default: return "unspecified";
  }
}

std::string DecimalMapToJsonString(
    const google::protobuf::Map<std::string, fob::common::v1::Decimal>& values) {
  std::ostringstream out;
  out << "{";
  bool first = true;
  for (const auto& [name, decimal] : values) {
    if (!first) out << ",";
    first = false;
    out << "\"" << JsonEscape(name) << "\":\""
        << JsonEscape(cex::common::Decimal::from_proto(decimal).to_string()) << "\"";
  }
  out << "}";
  return out.str();
}

std::string LiquidityAuditToJsonString(
    const google::protobuf::RepeatedPtrField<fob::matching::v1::LiquidityProvenance>& values) {
  std::ostringstream out;
  out << "[";
  bool first = true;
  for (const auto& value : values) {
    if (!first) out << ",";
    first = false;
    out << "{"
        << "\"liquidity_source\":\"" << JsonEscape(value.liquidity_source()) << "\","
        << "\"venue_id\":\"" << JsonEscape(value.venue_id()) << "\","
        << "\"snapshot_id\":\"" << JsonEscape(value.snapshot_id()) << "\","
        << "\"curve_id\":\"" << JsonEscape(value.curve_id()) << "\""
        << "}";
  }
  out << "]";
  return out.str();
}

int64_t EventTimeMs(const fob::matching::v1::BatchResult& evt) {
  if (evt.has_timestamp()) return TimestampToUnixMs(evt.timestamp());
  if (evt.has_meta() && evt.meta().has_ts_event()) return TimestampToUnixMs(evt.meta().ts_event());
  return 0;
}

std::string BuildBatchResultJsonRow(const fob::matching::v1::BatchResult& evt) {
  const std::string clear_prices_json = DecimalMapToJsonString(evt.clear_prices());
  const std::string executed_rates_json = DecimalMapToJsonString(evt.executed_rates());
  const std::string used_liquidity_json =
      evt.has_diagnostics() ? LiquidityAuditToJsonString(evt.diagnostics().used_liquidity()) : "[]";

  std::ostringstream row;
  row << "{";
  row << "\"batch_id\":\"" << JsonEscape(evt.batch_id()) << "\",";
  row << "\"event_time_ms\":" << EventTimeMs(evt) << ",";
  row << "\"source\":\"" << JsonEscape(evt.has_meta() ? evt.meta().source() : "") << "\",";
  row << "\"correlation_id\":\""
      << JsonEscape(evt.has_meta() ? evt.meta().correlation_id() : "") << "\",";
  row << "\"partition_key\":\""
      << JsonEscape(evt.has_meta() ? evt.meta().partition_key() : "") << "\",";
  row << "\"residual_norm\":"
      << (evt.has_diagnostics() ? evt.diagnostics().residual_norm() : 0.0) << ",";
  row << "\"solve_time_ms\":"
      << (evt.has_diagnostics() ? evt.diagnostics().solve_time_ms() : 0) << ",";
  row << "\"num_active_orders\":"
      << (evt.has_diagnostics() ? evt.diagnostics().num_active_orders() : 0) << ",";
  row << "\"config_version\":"
      << (evt.has_diagnostics() ? evt.diagnostics().config_version() : 0) << ",";
  row << "\"solver_diagnostics_json\":\""
      << JsonEscape(evt.has_diagnostics() ? evt.diagnostics().solver_diagnostics_json() : "")
      << "\",";
  row << "\"clear_prices_json\":\"" << JsonEscape(clear_prices_json) << "\",";
  row << "\"executed_rates_json\":\"" << JsonEscape(executed_rates_json) << "\",";
  row << "\"used_liquidity_json\":\"" << JsonEscape(used_liquidity_json) << "\",";
  row << "\"fills_count\":" << evt.fills_size();
  row << "}";
  return row.str();
}

std::string BuildFillsJsonRows(const fob::matching::v1::BatchResult& evt) {
  std::ostringstream rows;
  const int64_t event_time_ms = EventTimeMs(evt);
  for (const auto& fill : evt.fills()) {
    const double fee_amount =
        (fill.has_fee() && fill.fee().has_cost() && fill.fee().cost().has_amount())
            ? DecimalToDouble(fill.fee().cost().amount())
            : 0.0;
    const std::string fee_currency =
        (fill.has_fee() && fill.fee().has_cost()) ? fill.fee().cost().currency() : "";

    rows << "{";
    rows << "\"batch_id\":\"" << JsonEscape(evt.batch_id()) << "\",";
    rows << "\"event_time_ms\":" << event_time_ms << ",";
    rows << "\"order_id\":\"" << JsonEscape(fill.order_id()) << "\",";
    rows << "\"user_id\":\"" << JsonEscape(fill.user_id()) << "\",";
    rows << "\"symbol\":\"" << JsonEscape(fill.instrument().symbol()) << "\",";
    rows << "\"base\":\"" << JsonEscape(fill.instrument().base()) << "\",";
    rows << "\"quote\":\"" << JsonEscape(fill.instrument().quote()) << "\",";
    rows << "\"side\":\"" << JsonEscape(SideToString(fill.side())) << "\",";
    rows << "\"executed_qty\":" << DecimalToDouble(fill.executed_qty()) << ",";
    rows << "\"price\":" << DecimalToDouble(fill.price()) << ",";
    rows << "\"executed_notional\":" << DecimalToDouble(fill.executed_notional()) << ",";
    rows << "\"fee_amount\":" << fee_amount << ",";
    rows << "\"fee_currency\":\"" << JsonEscape(fee_currency) << "\",";
    rows << "\"liquidity_source\":\"" << JsonEscape(
        fill.liquidity_source().empty()
            ? (fill.has_provenance() && !fill.provenance().liquidity_source().empty()
                   ? fill.provenance().liquidity_source()
                   : "internal")
            : fill.liquidity_source()) << "\",";
    rows << "\"venue_id\":\"" << JsonEscape(
        fill.has_provenance() ? fill.provenance().venue_id() : "") << "\",";
    rows << "\"snapshot_id\":\"" << JsonEscape(
        fill.has_provenance() ? fill.provenance().snapshot_id() : "") << "\",";
    rows << "\"curve_id\":\"" << JsonEscape(
        fill.has_provenance() ? fill.provenance().curve_id() : "") << "\"";
    rows << "}\n";
  }
  return rows.str();
}

std::string BuildExecutionReportJsonRow(const fob::execution::v1::ExecutionReport& evt) {
  const int64_t event_time_ms =
      (evt.has_meta() && evt.meta().has_ts_event()) ? TimestampToUnixMs(evt.meta().ts_event()) : 0;
  const double filled_qty = evt.has_filled_qty() ? DecimalToDouble(evt.filled_qty()) : 0.0;
  const double remaining_qty = evt.has_remaining_qty() ? DecimalToDouble(evt.remaining_qty()) : 0.0;
  const double average_price = evt.has_average_price() ? DecimalToDouble(evt.average_price()) : 0.0;
  double fee_total_amount = 0.0;
  std::string fee_total_currency;
  if (evt.has_fee_total() && evt.fee_total().has_cost()) {
    fee_total_currency = evt.fee_total().cost().currency();
    if (evt.fee_total().cost().has_amount()) {
      fee_total_amount = DecimalToDouble(evt.fee_total().cost().amount());
    }
  }

  std::ostringstream row;
  row << "{";
  row << "\"report_id\":\"" << JsonEscape(evt.report_id()) << "\",";
  row << "\"intent_id\":\"" << JsonEscape(evt.intent_id()) << "\",";
  row << "\"venue\":\"" << JsonEscape(evt.venue()) << "\",";
  row << "\"symbol\":\"" << JsonEscape(evt.instrument().symbol()) << "\",";
  row << "\"venue_symbol\":\"" << JsonEscape(evt.venue_symbol()) << "\",";
  row << "\"venue_order_id\":\"" << JsonEscape(evt.venue_order_id()) << "\",";
  row << "\"client_order_id\":\"" << JsonEscape(evt.client_order_id()) << "\",";
  row << "\"status\":" << static_cast<int>(evt.status()) << ",";
  row << "\"filled_qty\":" << filled_qty << ",";
  row << "\"remaining_qty\":" << remaining_qty << ",";
  row << "\"average_price\":" << average_price << ",";
  row << "\"fee_total_amount\":" << fee_total_amount << ",";
  row << "\"fee_total_currency\":\"" << JsonEscape(fee_total_currency) << "\",";
  row << "\"error_code\":\"" << JsonEscape(evt.has_error() ? evt.error().code() : "") << "\",";
  row << "\"error_message\":\"" << JsonEscape(evt.has_error() ? evt.error().message() : "") << "\",";
  row << "\"event_time_ms\":" << event_time_ms << ",";
  row << "\"source\":\"" << JsonEscape(evt.has_meta() ? evt.meta().source() : "") << "\",";
  row << "\"correlation_id\":\"" << JsonEscape(evt.has_meta() ? evt.meta().correlation_id() : "") << "\",";
  row << "\"partition_key\":\"" << JsonEscape(evt.has_meta() ? evt.meta().partition_key() : "") << "\"";
  row << "}";
  return row.str();
}

// Helper function for hedge PnL decimal conversion with high precision
std::string DecimalToClickHouseDecimal(const fob::common::v1::Decimal& decimal) {
  return cex::common::Decimal::from_proto(decimal).to_string();
}

// Helper to parse ClickHouse JSONEachRow response and extract hedge PnL records
bool ParseHedgePnLResponse(const std::string& response, fob::marketdata::v1::GetHedgePnLResponse* out) {
  // Simple JSON parsing for NDJSON (JSONEachRow format)
  // Each line is a JSON object
  std::istringstream iss(response);
  std::string line;
  
  while (std::getline(iss, line)) {
    if (line.empty()) continue;
    
    // Very simple parsing - in production you'd use a proper JSON library
    auto* record = out->add_records();
    
    auto extract_string = [&line](const std::string& key) -> std::string {
      std::string search = "\"" + key + "\":\"";
      size_t start = line.find(search);
      if (start == std::string::npos) return "";
      start += search.length();
      size_t end = line.find("\"", start);
      if (end == std::string::npos) return "";
      return line.substr(start, end - start);
    };
    
    auto extract_double = [&line](const std::string& key) -> double {
      std::string search = "\"" + key + "\":";
      size_t start = line.find(search);
      if (start == std::string::npos) return 0.0;
      start += search.length();
      size_t end = line.find_first_of(",}\n", start);
      if (end == std::string::npos) return 0.0;
      return std::stod(line.substr(start, end - start));
    };
    
    auto extract_uint64 = [&line](const std::string& key) -> uint64_t {
      std::string search = "\"" + key + "\":";
      size_t start = line.find(search);
      if (start == std::string::npos) return 0;
      start += search.length();
      size_t end = line.find_first_of(",}\n", start);
      if (end == std::string::npos) return 0;
      return static_cast<uint64_t>(std::stoull(line.substr(start, end - start)));
    };
    
    record->set_batch_id(extract_string("batch_id"));
    record->set_session_id(extract_string("session_id"));
    record->set_user_id(extract_string("user_id"));
    record->set_venue(extract_string("venue"));
    record->set_instrument(extract_string("instrument"));
    record->set_total_hedge_pnl(extract_double("total_hedge_pnl"));
    record->set_total_filled_qty(extract_double("total_filled_qty"));
    record->set_trade_count(extract_uint64("trade_count"));
    
    int64_t batch_time_seconds = static_cast<int64_t>(extract_double("batch_time"));
    if (batch_time_seconds > 0) {
      record->mutable_batch_time()->set_seconds(batch_time_seconds);
    }
  }
  
  return true;
}

}  // namespace

ClickHouseBatchStorage::ClickHouseBatchStorage(ClickHouseConfig cfg)
    : cfg_(std::move(cfg)) {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

bool ClickHouseBatchStorage::EnsureSchema() {
  const std::string create_db = "CREATE DATABASE IF NOT EXISTS " + cfg_.database;
  const std::string create_batchresults =
      "CREATE TABLE IF NOT EXISTS " + BatchResultsTableName() + " ("
      "batch_id String,"
      "event_time_ms Int64,"
      "source String,"
      "correlation_id String,"
      "partition_key String,"
      "residual_norm Float64,"
      "solve_time_ms UInt32,"
      "num_active_orders UInt32,"
      "config_version UInt32,"
      "solver_diagnostics_json String,"
      "clear_prices_json String,"
      "executed_rates_json String,"
      "used_liquidity_json String,"
      "fills_count UInt32,"
      "ingested_at DateTime DEFAULT now()"
      ") ENGINE = MergeTree "
      "ORDER BY (event_time_ms, batch_id)";

  const std::string create_fills =
      "CREATE TABLE IF NOT EXISTS " + FillsTableName() + " ("
      "batch_id String,"
      "event_time_ms Int64,"
      "order_id String,"
      "user_id String,"
      "symbol String,"
      "base String,"
      "quote String,"
      "side LowCardinality(String),"
      "executed_qty Float64,"
      "price Float64,"
      "executed_notional Float64,"
      "fee_amount Float64,"
      "fee_currency String,"
      "liquidity_source String,"
      "venue_id String,"
      "snapshot_id String,"
      "curve_id String,"
      "ingested_at DateTime DEFAULT now()"
      ") ENGINE = MergeTree "
      "ORDER BY (event_time_ms, batch_id, order_id)";

  const std::string create_execution_venue =
      "CREATE TABLE IF NOT EXISTS " + ExecutionVenueTableName() + " ("
      "report_id String,"
      "intent_id String,"
      "venue String,"
      "symbol String,"
      "venue_symbol String,"
      "venue_order_id String,"
      "client_order_id String,"
      "status Int32,"
      "filled_qty Float64,"
      "remaining_qty Float64,"
      "average_price Float64,"
      "fee_total_amount Float64,"
      "fee_total_currency String,"
      "error_code String,"
      "error_message String,"
      "event_time_ms Int64,"
      "source String,"
      "correlation_id String,"
      "partition_key String,"
      "ingested_at DateTime DEFAULT now()"
      ") ENGINE = MergeTree "
      "ORDER BY (event_time_ms, venue, intent_id, report_id)";

  const std::string create_hedge_pnl =
      "CREATE TABLE IF NOT EXISTS " + HedgePnLTableName() + " ("
      "batch_id String,"
      "session_id String,"
      "user_id String,"
      "venue String,"
      "instrument String,"
      "total_hedge_pnl Decimal(24, 8),"
      "total_filled_qty Decimal(24, 8),"
      "trade_count UInt64,"
      "batch_time DateTime,"
      "created_at DateTime DEFAULT now()"
      ") ENGINE = SummingMergeTree() "
      "ORDER BY (batch_time, venue, instrument)";

  return ExecQuery(create_db) &&
         ExecQuery(create_batchresults) &&
         ExecQuery(create_fills) &&
         ExecQuery(create_execution_venue) &&
         ExecQuery(create_hedge_pnl);
}

bool ClickHouseBatchStorage::SaveBatchResult(const fob::matching::v1::BatchResult& evt) {
  const std::string query =
      "INSERT INTO " + BatchResultsTableName() + " FORMAT JSONEachRow";
  const std::string row = BuildBatchResultJsonRow(evt) + "\n";
  return ExecQuery(query, row);
}

bool ClickHouseBatchStorage::SaveFills(const fob::matching::v1::BatchResult& evt) {
  if (evt.fills().empty()) return true;
  const std::string query =
      "INSERT INTO " + FillsTableName() + " FORMAT JSONEachRow";
  return ExecQuery(query, BuildFillsJsonRows(evt));
}

bool ClickHouseBatchStorage::SaveExecutionReport(
    const fob::execution::v1::ExecutionReport& evt) {
  const std::string query =
      "INSERT INTO " + ExecutionVenueTableName() + " FORMAT JSONEachRow";
  const std::string row = BuildExecutionReportJsonRow(evt) + "\n";
  return ExecQuery(query, row);
}

bool ClickHouseBatchStorage::ExecQuery(const std::string& query, const std::string& body) {
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

  std::ostringstream url_builder;
  url_builder << "http://" << cfg_.host << ":" << cfg_.port << "/?query=" << encoded;
  std::string url = url_builder.str();
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
    cex::common::log_json(
        "ERROR", "ClickHouse returned non-success status",
        {{"http_code", std::to_string(http_code)},
         {"response", response.empty() ? "<empty>" : response}});
    return false;
  }
  return true;
}

bool ClickHouseBatchStorage::ExecSelectQuery(const std::string& query, std::string* response) {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    cex::common::log_json("ERROR", "curl_easy_init failed for SELECT");
    return false;
  }

  char* encoded = curl_easy_escape(curl, query.c_str(), static_cast<int>(query.size()));
  if (encoded == nullptr) {
    cex::common::log_json("ERROR", "Failed to URL-encode ClickHouse SELECT query");
    curl_easy_cleanup(curl);
    return false;
  }

  std::ostringstream url_builder;
  url_builder << "http://" << cfg_.host << ":" << cfg_.port << "/?query=" << encoded;
  std::string url = url_builder.str();
  curl_free(encoded);

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, cfg_.timeout_ms);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
  if (!cfg_.user.empty()) {
    curl_easy_setopt(curl, CURLOPT_USERNAME, cfg_.user.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg_.password.c_str());
  }

  CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    cex::common::log_json("ERROR", "ClickHouse SELECT request failed",
                          {{"err", curl_easy_strerror(rc)}});
    return false;
  }
  if (http_code < 200 || http_code >= 300) {
    cex::common::log_json(
        "ERROR", "ClickHouse SELECT returned non-success status",
        {{"http_code", std::to_string(http_code)},
         {"response", response->empty() ? "<empty>" : *response}});
    return false;
  }
  return true;
}

std::string ClickHouseBatchStorage::BatchResultsTableName() const {
  return cfg_.database + "." + cfg_.batchresults_table;
}

std::string ClickHouseBatchStorage::FillsTableName() const {
  return cfg_.database + "." + cfg_.fills_table;
}

std::string ClickHouseBatchStorage::ExecutionVenueTableName() const {
  return cfg_.database + "." + cfg_.execution_venue_table;
}

std::string ClickHouseBatchStorage::HedgePnLTableName() const {
  return cfg_.database + "." + cfg_.hedge_pnl_table;
}

bool ClickHouseBatchStorage::SaveHedgePnL(
    const std::string& batch_id,
    const std::string& session_id,
    const std::string& user_id,
    const std::string& venue,
    const std::string& instrument,
    const fob::common::v1::Decimal& total_hedge_pnl,
    const fob::common::v1::Decimal& total_filled_qty,
    uint64_t trade_count,
    const google::protobuf::Timestamp& batch_time) {
  
  const std::string query = "INSERT INTO " + HedgePnLTableName() + 
      " (batch_id, session_id, user_id, venue, instrument, total_hedge_pnl, "
      "total_filled_qty, trade_count, batch_time) FORMAT JSONEachRow";
  
  std::ostringstream row;
  row << "{";
  row << "\"batch_id\":\"" << JsonEscape(batch_id) << "\",";
  row << "\"session_id\":\"" << JsonEscape(session_id) << "\",";
  row << "\"user_id\":\"" << JsonEscape(user_id) << "\",";
  row << "\"venue\":\"" << JsonEscape(venue) << "\",";
  row << "\"instrument\":\"" << JsonEscape(instrument) << "\",";
  row << "\"total_hedge_pnl\":\"" << DecimalToClickHouseDecimal(total_hedge_pnl) << "\",";
  row << "\"total_filled_qty\":\"" << DecimalToClickHouseDecimal(total_filled_qty) << "\",";
  row << "\"trade_count\":" << trade_count << ",";
  row << "\"batch_time\":" << TimestampToUnixMs(batch_time) / 1000;
  row << "}\n";
  
  return ExecQuery(query, row.str());
}

bool ClickHouseBatchStorage::GetHedgePnL(
    const std::string& batch_id,
    const std::string& session_id,
    const std::string& user_id,
    const google::protobuf::Timestamp* from_time,
    const google::protobuf::Timestamp* to_time,
    fob::marketdata::v1::GetHedgePnLResponse* response) {
    
  if (response == nullptr) {
    cex::common::log_json("ERROR", "GetHedgePnL: response is null");
    return false;
  }
  
  std::ostringstream query;
  query << "SELECT batch_id, session_id, user_id, venue, instrument, "
        << "total_hedge_pnl, total_filled_qty, trade_count, batch_time "
        << "FROM " << HedgePnLTableName() << " WHERE 1=1";
  
  if (!batch_id.empty()) {
    query << " AND batch_id = '" << batch_id << "'";
  }
  if (!session_id.empty()) {
    query << " AND session_id = '" << session_id << "'";
  }
  if (!user_id.empty()) {
    query << " AND user_id = '" << user_id << "'";
  }
  if (from_time) {
    query << " AND batch_time >= " << from_time->seconds();
  }
  if (to_time) {
    query << " AND batch_time <= " << to_time->seconds();
  }
  
  query << " FORMAT JSONEachRow";
  
  std::string result;
  if (!ExecSelectQuery(query.str(), &result)) {
    cex::common::log_json("ERROR", "GetHedgePnL: query execution failed",
                          {{"query", query.str()}});
    return false;
  }
  
  if (!ParseHedgePnLResponse(result, response)) {
    cex::common::log_json("ERROR", "GetHedgePnL: failed to parse response",
                          {{"response", result}});
    return false;
  }
  
  return true;
}

}  // namespace cex::market_data::infra