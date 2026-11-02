#pragma once

// ============================================================================
// venue_endpoints.hpp — адреса REST/WebSocket биржи для записи конфигурации по умолчанию.
//
// Запись конфигурации (VenueConfigRecord) сеется при старте из env и затем правится админ-API
// (включить/выключить биржу, изменить порог). Каждое сохранение записи ПРИМЕНЯЕТСЯ к адаптеру
// (ApplyRuntimeConfig) и пишется в PG. Если в записи чужие адреса, биржа после любого сохранения
// начинает ходить не туда: 2026-10-15 после disable/enable Kraken получил адреса Binance
// (запись для всех не-Coinbase бирж сеялась адресами Binance) и стал отвечать 403.
// ============================================================================

#include <string>

namespace cex::venues::domain {

struct CexEndpoints {
  std::string ws_url;
  std::string rest_base_url;
};

// Адреса по умолчанию для каждой поддержанной биржи (значения берёт вызывающий из env).
struct CexEndpointDefaults {
  CexEndpoints binance;
  CexEndpoints coinbase;
  CexEndpoints kraken;
  CexEndpoints okx;
};

// Выбор адресов по идентификатору биржи (регистр не важен, как в остальном коде — по подстроке:
// допускаются суффиксы вида "kraken_eu"). Неизвестная биржа получает адреса Binance — прежнее поведение.
CexEndpoints SeedCexEndpoints(const std::string& venue_id, const CexEndpointDefaults& defaults);

}  // namespace cex::venues::domain
