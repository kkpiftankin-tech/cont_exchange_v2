#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "domain/trades_fetch_scheduler.hpp"

namespace {

using cex::venues::domain::IsStalestTradesCandidate;
using cex::venues::domain::TradesReadDue;
using cex::venues::domain::TradesFreshnessEntry;

bool Check(const bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

// Базовый случай: самый старый по age_ms выигрывает приоритет.
bool TestOldestAgeWins() {
  const TradesFreshnessEntry btc{"BTCUSDT", 9000};
  const std::vector<TradesFreshnessEntry> others = {
      {"ETHUSDT", 3000},
      {"SOLUSDT", 1000},
      {"XRPUSDT", 500},
  };
  if (!Check(IsStalestTradesCandidate(btc, others),
             "symbol with the largest age_ms must win priority")) {
    return false;
  }
  // Любой из "свежих" не должен выиграть против BTC.
  const TradesFreshnessEntry eth{"ETHUSDT", 3000};
  const std::vector<TradesFreshnessEntry> others_for_eth = {
      {"BTCUSDT", 9000},
      {"SOLUSDT", 1000},
      {"XRPUSDT", 500},
  };
  if (!Check(!IsStalestTradesCandidate(eth, others_for_eth),
             "fresher symbol must NOT win priority over a staler one")) {
    return false;
  }
  return true;
}

// "Никогда не читали" (age_ms<0) — бесконечно устаревший, выше любого
// конечного age_ms.
bool TestNeverReadBeatsAnyFiniteAge() {
  const TradesFreshnessEntry never_read{"OKXSYM", -1};
  const std::vector<TradesFreshnessEntry> others = {
      {"BINANCESYM", 999999999},  // огромный, но конечный возраст
  };
  if (!Check(IsStalestTradesCandidate(never_read, others),
             "never-read (-1) symbol must win over any finite age")) {
    return false;
  }

  const TradesFreshnessEntry finite_age{"BINANCESYM", 999999999};
  const std::vector<TradesFreshnessEntry> others2 = {
      {"OKXSYM", -1},
  };
  if (!Check(!IsStalestTradesCandidate(finite_age, others2),
             "finite age symbol must NOT win over never-read symbol")) {
    return false;
  }
  return true;
}

// Детерминированный тай-брейк на первом такте: все "никогда не читались" —
// только лексикографически наименьший ключ получает приоритет, остальные
// символы этого же такта обязаны уступить (иначе все одновременно дёргают
// один REST token bucket и проблема не решается).
bool TestFirstTickTieBreakIsDeterministicAndExclusive() {
  const std::vector<TradesFreshnessEntry> all = {
      {"BTCUSDT", -1},
      {"ETHUSDT", -1},
      {"SOLUSDT", -1},
      {"XRPUSDT", -1},
  };

  int winners = 0;
  std::string winner_key;
  for (std::size_t i = 0; i < all.size(); ++i) {
    std::vector<TradesFreshnessEntry> others;
    for (std::size_t j = 0; j < all.size(); ++j) {
      if (j != i) others.push_back(all[j]);
    }
    if (IsStalestTradesCandidate(all[i], others)) {
      ++winners;
      winner_key = all[i].venue_symbol;
    }
  }
  if (!Check(winners == 1, "exactly one symbol must win the tie on the first tick")) {
    return false;
  }
  if (!Check(winner_key == "BTCUSDT",
             "lexicographically smallest key must win the never-read tie")) {
    return false;
  }
  return true;
}

// Round-robin по голоду: как только победитель такта читает трейды (его age
// обнуляется), приоритет на следующем такте переходит к следующему по
// очереди (следующему самому старому) символу — не застревает на одном.
bool TestRoundRobinAdvancesAfterWinnerRefreshes() {
  // Такт 1: ETH самый старый (age=5000) -> выигрывает.
  {
    const TradesFreshnessEntry eth{"ETHUSDT", 5000};
    const std::vector<TradesFreshnessEntry> others = {
        {"BTCUSDT", 1000},
        {"SOLUSDT", 2000},
    };
    if (!Check(IsStalestTradesCandidate(eth, others),
               "tick1: ETH (oldest) must win")) {
      return false;
    }
  }
  // Такт 2: ETH только что обновился (age=0) -> теперь SOL (2000) самый старый.
  {
    const TradesFreshnessEntry sol{"SOLUSDT", 2000};
    const std::vector<TradesFreshnessEntry> others = {
        {"BTCUSDT", 1000},
        {"ETHUSDT", 0},
    };
    if (!Check(IsStalestTradesCandidate(sol, others),
               "tick2: SOL must win once ETH refreshed to age=0")) {
      return false;
    }
    const TradesFreshnessEntry eth_refreshed{"ETHUSDT", 0};
    const std::vector<TradesFreshnessEntry> others_for_eth = {
        {"BTCUSDT", 1000},
        {"SOLUSDT", 2000},
    };
    if (!Check(!IsStalestTradesCandidate(eth_refreshed, others_for_eth),
               "tick2: freshly-refreshed ETH must NOT win again immediately")) {
      return false;
    }
  }
  return true;
}

// Единственный символ на адаптере (DEX/AMM, inst_n=1) — others пуст, функция
// обязана вернуть true безусловно (не регрессировать прежнее always-true
// поведение single-instrument адаптеров).
bool TestSingleSymbolAlwaysWins() {
  const TradesFreshnessEntry only{"POOLUSD", -1};
  const std::vector<TradesFreshnessEntry> others;
  if (!Check(IsStalestTradesCandidate(only, others),
             "single-symbol adapter (empty others) must always win")) {
    return false;
  }
  return true;
}

// Режим планировщика (ADR-078): ленту пара читает не чаще минимального интервала; ещё не читавшаяся — сразу.
bool TestTradesReadDueRespectsMinInterval() {
  if (!Check(TradesReadDue(-1, 15000), "ещё не читали — читать сразу")) return false;
  if (!Check(!TradesReadDue(0, 15000), "только что прочитана — рано")) return false;
  if (!Check(!TradesReadDue(14999, 15000), "на 1 мс раньше интервала — рано")) return false;
  if (!Check(TradesReadDue(15000, 15000), "ровно интервал — пора")) return false;
  if (!Check(TradesReadDue(40000, 15000), "давно не читали — пора")) return false;
  if (!Check(TradesReadDue(0, 0), "нулевой интервал — всегда пора (прежнее поведение)")) return false;
  return true;
}

}  // namespace

int main() {
  if (!TestTradesReadDueRespectsMinInterval()) return EXIT_FAILURE;
  if (!TestOldestAgeWins()) return EXIT_FAILURE;
  if (!TestNeverReadBeatsAnyFiniteAge()) return EXIT_FAILURE;
  if (!TestFirstTickTieBreakIsDeterministicAndExclusive()) return EXIT_FAILURE;
  if (!TestRoundRobinAdvancesAfterWinnerRefreshes()) return EXIT_FAILURE;
  if (!TestSingleSymbolAlwaysWins()) return EXIT_FAILURE;

  std::cout << "[OK] trades_fetch_scheduler_test passed" << std::endl;
  return EXIT_SUCCESS;
}
