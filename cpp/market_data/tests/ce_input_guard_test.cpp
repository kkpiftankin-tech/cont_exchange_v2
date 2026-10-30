// Защита входа CE-клиринга от несогласованности (инцидент 2026-10-26): котировка ссылается на актив/площадку вне списков ⇒ matching падает на map::at.
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "domain/ce_input_guard.hpp"

namespace {

using cex::market_data::domain::CeQuoteRef;
using cex::market_data::domain::FindInconsistentQuote;

bool Check(const bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

// Точный случай инцидента: assets = {ETH, SOL}, а у котировки ETH/BTC котируемая валюта BTC (кросс-пара) ⇒ несогласован.
bool TestIncidentCase() {
  const std::vector<std::string> assets{"ETH", "SOL"};
  const std::vector<std::string> venues{"binance", "okx", "kraken"};
  const std::vector<CeQuoteRef> quotes{{"ETH", "", "binance"}, {"SOL", "", "okx"}, {"ETH", "BTC", "binance"}};
  return Check(FindInconsistentQuote(assets, venues, "USDT", quotes) == 2, "cross-pair quote on BTC missing from assets is inconsistent (index 2)");
}

// Полный набор активов ⇒ согласован; номинал допустим всегда; пустая котируемая валюта = номинал.
bool TestConsistent() {
  const std::vector<std::string> assets{"BTC", "ETH", "SOL"};
  const std::vector<std::string> venues{"binance", "okx"};
  const std::vector<CeQuoteRef> quotes{{"BTC", "", "binance"}, {"ETH", "BTC", "okx"}, {"SOL", "USDT", "okx"}};
  return Check(FindInconsistentQuote(assets, venues, "USDT", quotes) == -1, "consistent input passes") &&
         Check(FindInconsistentQuote(assets, venues, "USDT", {}) == -1, "no quotes is consistent");
}

// Площадка вне списка и базовый актив вне списка.
bool TestUnknownVenueAndBase() {
  const std::vector<std::string> assets{"BTC"};
  const std::vector<std::string> venues{"binance"};
  return Check(FindInconsistentQuote(assets, venues, "USDT", {{"BTC", "", "okx"}}) == 0, "unknown venue is inconsistent") &&
         Check(FindInconsistentQuote(assets, venues, "USDT", {{"BTC", "", "binance"}, {"SOL", "", "binance"}}) == 1, "unknown base asset is inconsistent (index 1)");
}

// Правило пропуска актива: BTC как базовый исключён по возрасту, но кросс-пара ETH/BTC остаётся ⇒ BTC на ссылки есть и убирать его нельзя;
// на актив нет ссылок ни в одной роли ⇒ можно пропустить.
bool TestReferencedAssets() {
  using cex::market_data::domain::ReferencedAssets;
  const auto kept = ReferencedAssets({{"ETH", "", "binance"}, {"SOL", "", "okx"}, {"ETH", "BTC", "binance"}}, "USDT");
  const auto none = ReferencedAssets({{"ETH", "", "binance"}, {"SOL", "", "okx"}}, "USDT");
  return Check(kept.count("BTC") == 1, "BTC is referenced as the quote currency of ETH/BTC: must not be dropped") &&
         Check(kept.count("ETH") == 1 && kept.count("SOL") == 1 && kept.count("USDT") == 1, "base assets and numeraire are referenced") &&
         Check(none.count("BTC") == 0, "BTC with no references in any role may be skipped") && Check(ReferencedAssets({}, "USDT").empty(), "no quotes, no references");
}

// Свойство: список активов, отобранный SelectClearingAssets по ссылкам оставшихся котировок, вместе с ними всегда проходит FindInconsistentQuote.
// Случай инцидента: BTC исключён (все его котировки убрал вес), но ETH/BTC на него ссылается ⇒ BTC остаётся; без ссылок ⇒ пропускается.
bool TestSelectClearingAssets() {
  using cex::market_data::domain::ReferencedAssets;
  using cex::market_data::domain::SelectClearingAssets;
  const std::vector<std::string> order{"BTC", "ETH", "SOL"};
  const std::vector<std::string> venues{"binance", "okx"};
  const std::set<std::string> excluded{"BTC", "SOL"};

  const std::vector<CeQuoteRef> cross{{"ETH", "", "binance"}, {"ETH", "BTC", "okx"}};
  const auto kept = SelectClearingAssets(order, excluded, ReferencedAssets(cross, "USDT"));

  const std::vector<CeQuoteRef> plain{{"ETH", "", "binance"}};
  const auto pruned = SelectClearingAssets(order, excluded, ReferencedAssets(plain, "USDT"));

  return Check(kept == (std::vector<std::string>{"BTC", "ETH"}), "BTC stays (quote of ETH/BTC), SOL without references is skipped, order kept") &&
         Check(FindInconsistentQuote(kept, venues, "USDT", cross) == -1, "kept assets and cross quotes are consistent") &&
         Check(pruned == (std::vector<std::string>{"ETH"}), "excluded assets without references are skipped") &&
         Check(FindInconsistentQuote(pruned, venues, "USDT", plain) == -1, "pruned assets and plain quotes are consistent") &&
         Check(SelectClearingAssets(order, {}, {}) == order, "nothing excluded keeps every asset") &&
         Check(SelectClearingAssets({}, excluded, {}).empty(), "no assets, no result");
}

}  // namespace

int main() {
  if (!(TestIncidentCase() && TestConsistent() && TestUnknownVenueAndBase() && TestReferencedAssets() && TestSelectClearingAssets())) return 1;
  std::cout << "all passed\n";
  return 0;
}
