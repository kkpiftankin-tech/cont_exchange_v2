#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#include <curl/curl.h>

#include "app/replay_orchestration_ports.hpp"
#include "infra/clickhouse_storage.hpp"

namespace {

std::string NormalizeSql(std::string value);

size_t CurlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

bool RunQuery(const std::string& url, const std::string& query, std::string* response) {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) return false;

  char* encoded = curl_easy_escape(curl, query.c_str(), static_cast<int>(query.size()));
  if (encoded == nullptr) {
    curl_easy_cleanup(curl);
    return false;
  }

  const std::string full_url = url + "/?query=" + encoded;
  curl_free(encoded);

  response->clear();
  curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);

  const CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);
  return rc == CURLE_OK && http_code >= 200 && http_code < 300;
}

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool CheckContainsNormalized(const std::string& haystack,
                             const std::string& needle,
                             const std::string& message) {
  const std::string normalized_haystack = NormalizeSql(haystack);
  const std::string normalized_needle = NormalizeSql(needle);
  if (normalized_haystack.find(normalized_needle) != std::string::npos) return true;

  std::cerr << "[FAIL] " << message << std::endl;
  std::cerr << "missing normalized substring: " << normalized_needle << std::endl;
  std::cerr << "ddl: " << haystack << std::endl;
  return false;
}

bool CheckProjectionOrderBy(const std::string& ddl,
                            const std::string& projection_name,
                            const std::string& order_by_expr,
                            const std::string& message) {
  const std::string normalized = NormalizeSql(ddl);
  const std::string projection =
      NormalizeSql("PROJECTION " + projection_name);
  const std::string order_by_with_parens =
      NormalizeSql("ORDER BY (" + order_by_expr + ")");
  const std::string order_by_without_parens =
      NormalizeSql("ORDER BY " + order_by_expr);

  if (normalized.find(projection) != std::string::npos &&
      (normalized.find(order_by_with_parens) != std::string::npos ||
       normalized.find(order_by_without_parens) != std::string::npos)) {
    return true;
  }

  std::cerr << "[FAIL] " << message << std::endl;
  std::cerr << "ddl: " << ddl << std::endl;
  return false;
}

std::string NormalizeSql(std::string value) {
  std::string normalized;
  normalized.reserve(value.size());

  bool prev_space = false;
  for (char ch : value) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (ch == '`' || ch == '"' || ch == '\'') {
      continue;
    }

    if (std::isspace(uch) != 0) {
      if (!prev_space) normalized.push_back(' ');
      prev_space = true;
      continue;
    }

    normalized.push_back(static_cast<char>(std::toupper(uch)));
    prev_space = false;
  }

  while (!normalized.empty() && normalized.back() == ' ') {
    normalized.pop_back();
  }
  return normalized;
}

bool CheckTtlConfigured(const std::string& ddl, const std::string& message) {
  const std::string normalized = NormalizeSql(ddl);
  const bool has_ttl_clause = normalized.find(" TTL ") != std::string::npos;
  const bool has_event_time = normalized.find("EVENT_TIME_MS") != std::string::npos;
  const bool has_to_datetime = normalized.find("TODATETIME(") != std::string::npos;
  const bool has_seconds_conversion =
      normalized.find("/ 1000") != std::string::npos ||
      normalized.find(", 1000") != std::string::npos ||
      normalized.find("(1000") != std::string::npos;
  const bool has_interval_form =
      normalized.find("INTERVAL 30 DAY") != std::string::npos;
  const bool has_function_form =
      normalized.find("TOINTERVALDAY(30)") != std::string::npos;

  if (has_ttl_clause && has_event_time && has_to_datetime && has_seconds_conversion &&
      (has_interval_form || has_function_form)) {
    return true;
  }

  std::cerr << "[FAIL] " << message << std::endl;
  std::cerr << "ddl: " << ddl << std::endl;
  return false;
}

std::string TrimRight(std::string value) {
  while (!value.empty()) {
    const char ch = value.back();
    if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ') {
      value.pop_back();
      continue;
    }
    break;
  }
  return value;
}

}  // namespace

int main() {
  curl_global_init(CURL_GLOBAL_DEFAULT);

  const char* url_env = std::getenv("CLICKHOUSE_TEST_URL");
  const std::string url = url_env == nullptr ? "http://127.0.0.1:18123" : url_env;
  std::string preflight_response;
  if (url_env == nullptr &&
      !RunQuery(url, "SELECT 1 FORMAT TabSeparatedRaw", &preflight_response)) {
    std::cout << "[SKIP] backtest_clickhouse_storage_test requires ClickHouse. "
              << "Set CLICKHOUSE_TEST_URL to run the live integration test."
              << std::endl;
    return EXIT_SUCCESS;
  }

  cex::backtest::infra::ClickHouseConfig cfg;
  cfg.url = url;
  cfg.database = "backtest_test";
  cfg.batchresults_table = "batchresults";
  cfg.fills_table = "fills";
  cfg.replay_agentlogs_table = "replay_agentlogs";
  cfg.replay_agentlogs_retention_days = 30;
  cfg.user = "default";
  cfg.password = "";
  cfg.timeout_ms = 3000;

  cex::backtest::infra::ClickHouseReplayStorage storage(cfg);
  if (!Check(storage.EnsureSchema(), "EnsureSchema must succeed")) return EXIT_FAILURE;

  const auto now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  const std::string batch_id = "bt-test-" + std::to_string(now_ns);

  fob::matching::v1::BatchResult batch;
  batch.set_batch_id(batch_id);
  batch.mutable_meta()->set_source("backtest_test");
  batch.mutable_meta()->set_correlation_id("corr-" + batch_id);
  batch.mutable_meta()->set_partition_key("BTC/USDT");
  batch.mutable_timestamp()->set_seconds(now_ns / 1000000000LL);
  batch.mutable_timestamp()->set_nanos(static_cast<int32_t>(now_ns % 1000000000LL));
  (*batch.mutable_clear_prices())["BTC/USDT"].set_units(10001);
  (*batch.mutable_clear_prices())["BTC/USDT"].set_scale(2);
  (*batch.mutable_executed_rates())["order-1"].set_units(125000);
  (*batch.mutable_executed_rates())["order-1"].set_scale(6);
  batch.mutable_diagnostics()->set_residual_norm(0.00042);
  batch.mutable_diagnostics()->set_solve_time_ms(12);
  batch.mutable_diagnostics()->set_num_active_orders(1);
  batch.mutable_diagnostics()->set_config_version(7);
  batch.mutable_diagnostics()->set_solver_diagnostics_json("{\"iters\":3}");

  auto* fill = batch.add_fills();
  fill->set_order_id("order-1");
  fill->set_user_id("user-1");
  fill->mutable_instrument()->set_symbol("BTC/USDT");
  fill->mutable_instrument()->set_base("BTC");
  fill->mutable_instrument()->set_quote("USDT");
  fill->set_side(fob::common::v1::SIDE_BUY);
  fill->mutable_executed_qty()->set_units(1000);
  fill->mutable_executed_qty()->set_scale(4);
  fill->mutable_price()->set_units(10001);
  fill->mutable_price()->set_scale(2);
  fill->mutable_executed_notional()->set_units(100010);
  fill->mutable_executed_notional()->set_scale(4);
  fill->mutable_fee()->mutable_cost()->mutable_amount()->set_units(5);
  fill->mutable_fee()->mutable_cost()->mutable_amount()->set_scale(4);
  fill->mutable_fee()->mutable_cost()->set_currency("USDT");

  if (!Check(storage.SaveBatchResult(batch), "SaveBatchResult must succeed")) return EXIT_FAILURE;
  if (!Check(storage.SaveFills(batch), "SaveFills must succeed")) return EXIT_FAILURE;

  std::string response;
  const std::string batch_count_query =
      "SELECT count() FROM backtest_test.batchresults WHERE batch_id = '" + batch_id +
      "' FORMAT TabSeparatedRaw";
  if (!Check(RunQuery(url, batch_count_query, &response), "batchresults count query must succeed")) {
    return EXIT_FAILURE;
  }
  if (!Check(TrimRight(response) == "1",
             "batchresults must contain exactly one row for test batch")) {
    std::cerr << "actual response: '" << response << "'" << std::endl;
    return EXIT_FAILURE;
  }

  response.clear();
  const std::string fills_count_query =
      "SELECT count() FROM backtest_test.fills WHERE batch_id = '" + batch_id +
      "' FORMAT TabSeparatedRaw";
  if (!Check(RunQuery(url, fills_count_query, &response), "fills count query must succeed")) {
    return EXIT_FAILURE;
  }
  if (!Check(TrimRight(response) == "1",
             "fills must contain exactly one row for test batch")) {
    std::cerr << "actual response: '" << response << "'" << std::endl;
    return EXIT_FAILURE;
  }

  response.clear();
  const std::string show_create_query =
      "SHOW CREATE TABLE backtest_test.replay_agentlogs FORMAT TabSeparatedRaw";
  if (!Check(RunQuery(url, show_create_query, &response),
             "SHOW CREATE TABLE replay_agentlogs must succeed")) {
    return EXIT_FAILURE;
  }
  const std::string ddl = TrimRight(response);
  // F15-BACKTEST-6: idempotent insert via ReplacingMergeTree with PK
  // (session_id, original_batch_id). Re-running the same step at retry
  // replaces the previous row at merge time.
  if (!CheckContainsNormalized(
          ddl, "ORDER BY (session_id, original_batch_id)",
          "replay_agentlogs must be keyed by session_id, original_batch_id")) {
    return EXIT_FAILURE;
  }
  if (!CheckContainsNormalized(
          ddl, "ENGINE = ReplacingMergeTree(ingested_at)",
          "replay_agentlogs must use ReplacingMergeTree(ingested_at) for idempotent upsert")) {
    return EXIT_FAILURE;
  }
  if (!CheckTtlConfigured(ddl, "replay_agentlogs must have configured TTL")) {
    return EXIT_FAILURE;
  }
  if (!CheckContainsNormalized(ddl, "state_json String",
                               "replay_agentlogs must contain state_json")) {
    return EXIT_FAILURE;
  }
  if (!CheckContainsNormalized(ddl, "log_id String",
                               "replay_agentlogs must contain log_id")) {
    return EXIT_FAILURE;
  }
  if (!CheckContainsNormalized(ddl, "action_json String",
                               "replay_agentlogs must contain action_json")) {
    return EXIT_FAILURE;
  }
  if (!CheckContainsNormalized(ddl, "metrics_json String",
                               "replay_agentlogs must contain metrics_json")) {
    return EXIT_FAILURE;
  }
  if (!CheckContainsNormalized(ddl, "session_config_snapshot_version UInt32",
                               "replay_agentlogs must contain session_config_snapshot_version")) {
    return EXIT_FAILURE;
  }
  if (!CheckContainsNormalized(
          ddl, "INDEX idx_solver_error_flag solver_error_flag TYPE set(2) GRANULARITY 1",
          "replay_agentlogs must index solver_error_flag")) {
    return EXIT_FAILURE;
  }
  if (!CheckContainsNormalized(ddl, "INDEX idx_fill_rate fill_rate TYPE minmax GRANULARITY 1",
                               "replay_agentlogs must index fill_rate")) {
    return EXIT_FAILURE;
  }
  if (!CheckContainsNormalized(ddl, "INDEX idx_pnl pnl TYPE minmax GRANULARITY 1",
                               "replay_agentlogs must index pnl")) {
    return EXIT_FAILURE;
  }

  response.clear();
  if (!Check(RunQuery(url, "SHOW CREATE TABLE backtest_test.batchresults FORMAT TabSeparatedRaw",
                      &response),
             "SHOW CREATE TABLE batchresults must succeed")) {
    return EXIT_FAILURE;
  }
  if (!CheckProjectionOrderBy(response, "prj_batchresults_by_batch_id",
                              "batch_id, event_time_ms",
                              "batchresults must have by-batch projection")) {
    return EXIT_FAILURE;
  }

  response.clear();
  if (!Check(RunQuery(url, "SHOW CREATE TABLE backtest_test.fills FORMAT TabSeparatedRaw",
                      &response),
             "SHOW CREATE TABLE fills must succeed")) {
    return EXIT_FAILURE;
  }
  if (!CheckProjectionOrderBy(response, "prj_fills_by_batch_id",
                              "batch_id, event_time_ms, order_id",
                              "fills must have by-batch projection")) {
    return EXIT_FAILURE;
  }

  response.clear();
  if (!Check(RunQuery(url, "SHOW CREATE TABLE backtest_test.venue_snapshots FORMAT TabSeparatedRaw",
                      &response),
             "SHOW CREATE TABLE venue_snapshots must succeed")) {
    return EXIT_FAILURE;
  }
  if (!CheckProjectionOrderBy(response, "prj_marketdata_by_event_time",
                              "event_time_ms, venue_id, symbol",
                              "venue_snapshots must have event-time projection")) {
    return EXIT_FAILURE;
  }

  // F15-BACKTEST-6: idempotent AgentLog upsert. Two writes with the same
  // (session_id, original_batch_id) must collapse to a single row at FINAL.
  {
    cex::backtest::app::AgentLogEntry first;
    first.session_id = "sess-idempotent";
    first.original_batch_id = "batch-idempotent";
    first.batch_seq = 1;
    first.event_time_ms = 1700000000000LL;
    first.pnl = 1.0;
    first.reward = 1.0;
    first.fill_rate = 0.5;
    first.solve_time_ms = 5;
    first.risk_status = "ok";
    first.state_json = "{\"v\":1}";
    first.action_json = "{}";

    cex::backtest::app::AgentLogEntry second = first;
    second.pnl = 99.0;
    second.reward = 99.0;
    second.state_json = "{\"v\":2}";

    storage.WriteAgentLog(first);
    storage.WriteAgentLog(second);

    response.clear();
    if (!Check(RunQuery(url,
                        "SELECT count() FROM backtest_test.replay_agentlogs FINAL "
                        "WHERE session_id = 'sess-idempotent' "
                        "AND original_batch_id = 'batch-idempotent' FORMAT TabSeparatedRaw",
                        &response),
               "SELECT FINAL count must succeed")) {
      return EXIT_FAILURE;
    }
    if (!Check(TrimRight(response) == "1",
               "ReplacingMergeTree must dedup on (session_id, original_batch_id) at FINAL")) {
      std::cerr << "actual count: '" << response << "'" << std::endl;
      return EXIT_FAILURE;
    }

    response.clear();
    if (!Check(RunQuery(url,
                        "SELECT pnl FROM backtest_test.replay_agentlogs FINAL "
                        "WHERE session_id = 'sess-idempotent' "
                        "AND original_batch_id = 'batch-idempotent' FORMAT TabSeparatedRaw",
                        &response),
               "SELECT FINAL pnl must succeed")) {
      return EXIT_FAILURE;
    }
    if (!Check(TrimRight(response) == "99",
               "Latest write (highest ingested_at) must win after FINAL merge")) {
      std::cerr << "actual pnl: '" << response << "'" << std::endl;
      return EXIT_FAILURE;
    }
  }

  std::cout << "[OK] backtest_clickhouse_storage_test passed" << std::endl;
  return EXIT_SUCCESS;
}
