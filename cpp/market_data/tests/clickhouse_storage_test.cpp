#include <chrono>
#include <cstdlib>
// #include <format>
#include <iostream>
#include <sstream>
#include <string>

#include <curl/curl.h>

#include "infra/clickhouse_storage.hpp"

#include "cex/common/env.hpp"

namespace {

size_t CurlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

bool RunQuery(const std::string& url, const std::string& query, const std::string& userpwd,
              std::string* response) {
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
  if (!userpwd.empty()) {
    curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
  }

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

  cex::market_data::infra::ClickHouseConfig cfg;
  cfg.host = cex::common::Env::get_string("CLICKHOUSE_TEST_HOST", "127.0.0.1");
  cfg.port = cex::common::Env::get_int("CLICKHOUSE_TEST_PORT", 18123);
  cfg.database = "mds2_test";
  cfg.batchresults_table = "batchresults";
  cfg.fills_table = "fills";
  cfg.user = cex::common::Env::get_string("CLICKHOUSE_TEST_USER", "default");
  cfg.password = cex::common::Env::get_string("CLICKHOUSE_TEST_PASSWORD", "test");
  cfg.timeout_ms = 3000;

  cex::market_data::infra::ClickHouseBatchStorage storage(cfg);
  if (!Check(storage.EnsureSchema(), "EnsureSchema must succeed")) return EXIT_FAILURE;

  const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const std::string batch_id = "batch-test-" + std::to_string(now_ns);

  fob::matching::v1::BatchResult batch;
  batch.set_batch_id(batch_id);
  batch.mutable_meta()->set_source("market_data_test");
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

  std::ostringstream url_stream;
  url_stream << "http://" << cfg.host << ":" << cfg.port;
  std::string url = url_stream.str();
  
  std::string userpwd = cfg.user;
  if (!cfg.user.empty()) {
    userpwd += ":" + cfg.password;
  }
  std::string response;
  const std::string batch_count_query =
      "SELECT count() FROM mds2_test.batchresults WHERE batch_id = '" + batch_id +
      "' FORMAT TabSeparatedRaw";
  if (!Check(RunQuery(url, batch_count_query, userpwd, &response),
             "batchresults count query must succeed")) {
    return EXIT_FAILURE;
  }
  if (!Check(TrimRight(response) == "1",
             "batchresults must contain exactly one row for test batch")) {
    std::cerr << "actual response: '" << response << "'" << std::endl;
    return EXIT_FAILURE;
  }

  response.clear();
  const std::string fills_count_query = "SELECT count() FROM mds2_test.fills WHERE batch_id = '" +
                                        batch_id + "' FORMAT TabSeparatedRaw";
  if (!Check(RunQuery(url, fills_count_query, userpwd, &response),
             "fills count query must succeed")) {
    return EXIT_FAILURE;
  }
  if (!Check(TrimRight(response) == "1", "fills must contain exactly one row for test batch")) {
    std::cerr << "actual response: '" << response << "'" << std::endl;
    return EXIT_FAILURE;
  }

  std::cout << "[OK] clickhouse_storage_test passed" << std::endl;
  return EXIT_SUCCESS;
}
