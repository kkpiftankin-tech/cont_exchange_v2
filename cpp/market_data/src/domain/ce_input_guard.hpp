#pragma once
// Защита входа CE-клиринга от несогласованности. matching строит узлы «актив@площадка» только из списков assets (плюс номинал) и venues входа и
// обращается к ним через node_index.at(...): котировка, ссылающаяся на актив или площадку вне списков, даёт std::out_of_range, процесс завершается
// (terminate), а после перезапуска то же сообщение читается снова — цикл падений (инцидент 2026-10-26, ADR-085). Чистая функция без ввода-вывода.

#include <set>
#include <string>
#include <vector>

namespace cex::market_data::domain {

struct CeQuoteRef {
  std::string asset;  // базовый актив котировки
  std::string quote;  // котируемая валюта; пусто ⇒ номинал
  std::string venue;
};

// Активы, на которые ссылается хотя бы одна котировка: и как базовый, и как котируемая валюта кросс-пары (ETH/BTC ссылается на BTC). Актив, которого здесь
// нет, можно не передавать в клиринг; актив, который здесь есть, убирать нельзя: matching не найдёт его узел и упадёт.
inline std::set<std::string> ReferencedAssets(const std::vector<CeQuoteRef>& quotes, const std::string& numeraire) {
  std::set<std::string> r;
  for (const auto& q : quotes) {
    r.insert(q.asset);
    r.insert(q.quote.empty() ? numeraire : q.quote);
  }
  return r;
}

// Активы, которые идут в клиринг: все из assets_order, кроме исключённых (вес или нулевая глубина убрали все их котировки), на которые при этом
// не ссылается ни одна оставшаяся котировка. Порядок сохраняется. Результат вместе с котировками всегда проходит FindInconsistentQuote по активам.
inline std::vector<std::string> SelectClearingAssets(const std::vector<std::string>& assets_order, const std::set<std::string>& excluded,
                                                     const std::set<std::string>& referenced) {
  std::vector<std::string> out;
  out.reserve(assets_order.size());
  for (const auto& a : assets_order)
    if (excluded.count(a) == 0 || referenced.count(a) != 0) out.push_back(a);
  return out;
}

// Индекс первой котировки, ссылающейся на актив (в роли базового или котируемого) или площадку вне списков; −1, если вход согласован.
// assets — торгуемые активы без номинала (как в CeClearingInput.assets); номинал допустим всегда.
inline int FindInconsistentQuote(const std::vector<std::string>& assets, const std::vector<std::string>& venues, const std::string& numeraire,
                                 const std::vector<CeQuoteRef>& quotes) {
  std::set<std::string> a(assets.begin(), assets.end());
  a.insert(numeraire);
  const std::set<std::string> v(venues.begin(), venues.end());
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const std::string& qa = quotes[i].quote.empty() ? numeraire : quotes[i].quote;
    if (a.count(quotes[i].asset) == 0 || a.count(qa) == 0 || v.count(quotes[i].venue) == 0) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace cex::market_data::domain
