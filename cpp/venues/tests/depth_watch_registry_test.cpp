#include <cstdlib>
#include <iostream>
#include <string>

#include "app/depth_watch_registry.hpp"

using cex::venues::app::DepthWatchRegistry;

namespace {

bool Check(bool cond, const std::string& msg) {
  if (!cond) std::cerr << "[FAIL] " << msg << std::endl;
  return cond;
}

struct FakeClock {
  DepthWatchRegistry::Clock::time_point now = DepthWatchRegistry::Clock::now();
  void AdvanceMs(int64_t ms) { now += std::chrono::milliseconds(ms); }
};

DepthWatchRegistry Make(FakeClock* c, std::size_t cap = 3) {
  DepthWatchRegistry::Config cfg;
  cfg.max_items = cap;
  cfg.min_ttl_ms = 1000;
  cfg.max_ttl_ms = 60000;
  return DepthWatchRegistry(cfg, [c] { return c->now; });
}

bool TestLeaseExpires() {
  FakeClock c; auto r = Make(&c);
  const auto t = r.Touch({{"binance", "BTC/USDT"}}, 30000);
  bool ok = Check(t.accepted == 1 && t.active == 1 && t.ttl_ms == 30000, "аренда принята на 30 с");
  ok = Check(r.SymbolsFor("binance").size() == 1, "символ активен") && ok;
  c.AdvanceMs(29000);
  ok = Check(r.SymbolsFor("binance").size() == 1, "за 1 с до конца ещё активен") && ok;
  c.AdvanceMs(1500);
  ok = Check(r.SymbolsFor("binance").empty(), "после срока аренда истекла сама (опрос выключается)") && ok;
  ok = Check(r.Active().empty(), "реестр пуст") && ok;
  return ok;
}

bool TestRenewalExtendsButNeverShortens() {
  FakeClock c; auto r = Make(&c);
  r.Touch({{"okx", "ETH/BTC"}}, 30000);
  c.AdvanceMs(20000);
  r.Touch({{"okx", "ETH/BTC"}}, 30000);           // продление: осталось 30 с
  c.AdvanceMs(25000);
  bool ok = Check(!r.SymbolsFor("okx").empty(), "продлённая аренда жива через 45 с от первого сигнала");
  r.Touch({{"okx", "ETH/BTC"}}, 1000);            // короткий сигнал не сокращает остаток
  const auto act = r.Active();
  ok = Check(act.size() == 1 && act[0].remaining_ms > 4000, "короткий сигнал не сократил аренду") && ok;
  return ok;
}

bool TestCapRejectsNewKeepsExisting() {
  FakeClock c; auto r = Make(&c, 2);
  auto t = r.Touch({{"binance", "BTC/USDT"}, {"okx", "BTC/USDT"}, {"kraken", "BTC/USDT"}}, 30000);
  bool ok = Check(t.accepted == 2 && t.rejected == 1 && t.active == 2, "потолок 2: третья пара отклонена");
  t = r.Touch({{"binance", "BTC/USDT"}}, 30000);   // существующую продлеваем при заполненном реестре
  ok = Check(t.accepted == 1 && t.rejected == 0, "продление существующей не упирается в потолок") && ok;
  c.AdvanceMs(31000);                               // всё истекло — место освободилось
  t = r.Touch({{"kraken", "BTC/USDT"}}, 30000);
  ok = Check(t.accepted == 1 && t.active == 1, "после истечения место свободно") && ok;
  return ok;
}

bool TestDedupAndTtlClamp() {
  FakeClock c; auto r = Make(&c, 8);
  auto t = r.Touch({{"binance", "BTC/USDT"}, {"binance", "BTC/USDT"}, {"", "X/Y"}, {"okx", ""}}, 999999);
  bool ok = Check(t.accepted == 1 && t.active == 1, "дубликаты и пустые элементы не считаются");
  ok = Check(t.ttl_ms == 60000, "срок ограничен сверху (забытая вкладка не держит опрос)") && ok;
  t = r.Touch({{"binance", "ETH/USDT"}}, 1);
  ok = Check(t.ttl_ms == 1000, "срок ограничен снизу") && ok;
  return ok;
}

bool TestSymbolsForIsPerVenueAndSorted() {
  FakeClock c; auto r = Make(&c, 8);
  r.Touch({{"binance", "SOL/USDT"}, {"binance", "BTC/USDT"}, {"okx", "ETH/BTC"}}, 30000);
  const auto b = r.SymbolsFor("binance");
  bool ok = Check(b.size() == 2 && b[0] == "BTC/USDT" && b[1] == "SOL/USDT", "binance: отсортировано, без чужих");
  ok = Check(r.SymbolsFor("kraken").empty(), "у kraken нет аренд") && ok;
  ok = Check(r.SymbolsFor("bin").empty(), "префикс имени биржи не даёт ложных совпадений") && ok;
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestLeaseExpires() && ok;
  ok = TestRenewalExtendsButNeverShortens() && ok;
  ok = TestCapRejectsNewKeepsExisting() && ok;
  ok = TestDedupAndTtlClamp() && ok;
  ok = TestSymbolsForIsPerVenueAndSorted() && ok;
  if (ok) std::cout << "[OK] depth_watch_registry_test passed (5 tests)" << std::endl;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
