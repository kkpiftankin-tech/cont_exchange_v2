// Тест выбора адресов для записи конфигурации биржи по умолчанию (регрессия 2026-10-15: Kraken получил адреса Binance).
#include <cstdlib>
#include <iostream>
#include <string>

#include "domain/venue_endpoints.hpp"

using cex::venues::domain::CexEndpointDefaults;
using cex::venues::domain::SeedCexEndpoints;

namespace {

bool Check(bool cond, const std::string& msg) {
  if (!cond) std::cerr << "[FAIL] " << msg << std::endl;
  return cond;
}

CexEndpointDefaults Defaults() {
  CexEndpointDefaults d;
  d.binance = {"wss://stream.binance.com:9443/ws", "https://api.binance.com"};
  d.coinbase = {"wss://ws-feed.exchange.coinbase.com", "https://api.exchange.coinbase.com"};
  d.kraken = {"wss://ws.kraken.com", "https://api.kraken.com"};
  d.okx = {"wss://ws.okx.com:8443/ws/v5/public", "https://www.okx.com"};
  return d;
}

}  // namespace

int main() {
  const auto d = Defaults();
  bool ok = true;
  ok = Check(SeedCexEndpoints("binance", d).rest_base_url == "https://api.binance.com", "binance → Binance") && ok;
  ok = Check(SeedCexEndpoints("coinbase", d).rest_base_url == "https://api.exchange.coinbase.com", "coinbase → Coinbase") && ok;
  // Главная проверка: Kraken и OKX НЕ получают адреса Binance.
  ok = Check(SeedCexEndpoints("kraken", d).rest_base_url == "https://api.kraken.com", "kraken → Kraken REST") && ok;
  ok = Check(SeedCexEndpoints("kraken", d).ws_url == "wss://ws.kraken.com", "kraken → Kraken WS") && ok;
  ok = Check(SeedCexEndpoints("okx", d).rest_base_url == "https://www.okx.com", "okx → OKX REST") && ok;
  ok = Check(SeedCexEndpoints("okx", d).ws_url == "wss://ws.okx.com:8443/ws/v5/public", "okx → OKX WS") && ok;
  // Регистр и суффиксы.
  ok = Check(SeedCexEndpoints("KRAKEN", d).rest_base_url == "https://api.kraken.com", "регистр не важен") && ok;
  ok = Check(SeedCexEndpoints("kraken_eu", d).rest_base_url == "https://api.kraken.com", "суффикс идентификатора") && ok;
  // Неизвестная биржа — прежнее поведение (адреса Binance).
  ok = Check(SeedCexEndpoints("bitfinex", d).rest_base_url == "https://api.binance.com", "неизвестная → Binance, как раньше") && ok;
  if (!ok) return EXIT_FAILURE;
  std::cout << "venue_endpoints_test: OK" << std::endl;
  return EXIT_SUCCESS;
}
