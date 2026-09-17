// F-05A CE money-path (ADR-057 §money-path): проекция потоков → количество ПО НОГАМ,
// по цене узла P_node=P0·exp(x/1000). Базовый такт, ref P0(BTC)=60000.
// Проверяем: разные цены узлов (O≠K), авторитетное количество, и что Σ delta_value = dBTC
// (value-сохранение, V-CEA-003). Допуск 1e-6 (qty) / 1e-1 (абсолютная цена).

#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

#include "domain/ce_agent_clearing.hpp"
#include "domain/ce_graph_assembler.hpp"
#include "domain/ce_position_projection.hpp"

namespace {
// T-F18-201 (ADR-061 §1, ADR-063): per-agent позиция на v2-графе (флаг
// CE_AGENT_POS). V=2 площадки (B,O) × 1 tradeable BTC + нумерарий USD —
// та же топология, что в matching_ce_assembler_test (T-F18-101), чтобы
// проверять знак/тип агента на уже провалидированном графе.
int RunAgentDeltasV2Test() {
  using namespace cex::matching::domain;
  int fail = 0;
  auto check = [&](bool cond, const char* what) {
    if (!cond) { std::printf("FAIL %s\n", what); ++fail; }
  };

  CeAssembleConfigV2 cfg2;
  cfg2.numeraire = "USD";
  cfg2.assets = {"BTC"};
  cfg2.venues = {"B", "O"};
  cfg2.leg["BTC"] = CeTransferLegParamsV2{12.0, 0.10};  // арбитражёр BTC B<->O
  cfg2.leg["USD"] = CeTransferLegParamsV2{18.0, 0.05};  // арбитражёр USD B<->O (замыкает цикл)
  std::vector<CeQuoteParams> quotes2 = {
      {"BTC", "B", 0.00, 50.0, 0.05},
      {"BTC", "O", 0.40, 50.0, 0.05},  // расхождение цен B/O — двигает и переводчиков, и арбитраж
  };

  CeClearInput in2 = AssembleCeGraphV2(cfg2, quotes2);
  CeClearResult r2 = ClearCe(in2);
  check(r2.converged, "v2.converged");

  std::vector<CeAgentDelta> ad = ProjectAgentDeltas(in2, r2, {});
  // 4 ребра (2 QUOTE + 2 TRANSFER, без STOCK в v2) — все активны (расхождение
  // цен площадок выводит каждое ребро из мёртвой зоны на этом сетапе).
  check(ad.size() == 4, "num agent deltas == 4");

  bool saw_translator_b = false, saw_translator_o = false;
  bool saw_arb_btc = false, saw_arb_usd = false;
  for (const auto& d : ad) {
    // мёртвая зона: эмитим только реальный поток.
    check(std::fabs(d.delta) > 1e-12, "|delta|>eps (dead-zone excluded)");
    if (d.agent_id == "T_BTC_B") {
      saw_translator_b = true;
      check(d.agent_kind == CeAgentKind::kTranslator, "T_BTC_B kind==TRANSLATOR");
      check(d.asset == "BTC", "T_BTC_B asset==BTC");
      check(d.venue == "B", "T_BTC_B venue==B (источник, node_meta[e.u])");
    } else if (d.agent_id == "T_BTC_O") {
      saw_translator_o = true;
      check(d.agent_kind == CeAgentKind::kTranslator, "T_BTC_O kind==TRANSLATOR");
      check(d.venue == "O", "T_BTC_O venue==O (источник, node_meta[e.u])");
    } else if (d.agent_id == "A_BTC_BO") {
      saw_arb_btc = true;
      check(d.agent_kind == CeAgentKind::kArbitrageur, "A_BTC_BO kind==ARBITRAGEUR");
      check(d.asset == "BTC", "A_BTC_BO asset==BTC");
      check(d.venue == "O", "A_BTC_BO venue==O (канонический dst, node_meta[e.v])");
    } else if (d.agent_id == "A_USD_BO") {
      saw_arb_usd = true;
      check(d.agent_kind == CeAgentKind::kArbitrageur, "A_USD_BO kind==ARBITRAGEUR");
      check(d.asset == "USD", "A_USD_BO asset==USD");
      check(d.venue == "O", "A_USD_BO venue==O (канонический dst, node_meta[e.v])");
    } else {
      std::printf("FAIL: неожиданный agent_id %s\n", d.agent_id.c_str());
      ++fail;
    }
  }
  check(saw_translator_b && saw_translator_o, "оба переводчика (T_BTC_B, T_BTC_O) присутствуют");
  check(saw_arb_btc && saw_arb_usd, "оба арбитражёра (A_BTC_BO, A_USD_BO) присутствуют");

  // ЗНАК = направление: сравниваем с сырым потоком клиринга r2.f по индексу
  // ребра — ProjectAgentDeltas НЕ должна ничего переворачивать/пересчитывать.
  for (std::size_t i = 0; i < in2.edges.size(); ++i) {
    const double f = r2.f[i];
    if (std::fabs(f) < 1e-12) continue;
    bool found = false;
    for (const auto& d : ad) {
      if (d.agent_id == in2.edges[i].name) {
        found = true;
        check(std::fabs(d.delta - f) < 1e-12, "delta==f_j (сырой поток, без конвертации)");
      }
    }
    check(found, "активное ребро присутствует в ProjectAgentDeltas");
  }

  // kStock не эмитится (deprecated в v2, ADR-061 §1) — регрессия на v1-графе
  // с ненулевым STOCK-плечом: убеждаемся, что S_* рёбра исключены даже если
  // поданы на входе (защита от будущего вызова на v1-конфиге).
  {
    CeAssembleConfig cfg1;
    cfg1.numeraire = "USD"; cfg1.assets = {"BTC"}; cfg1.venues = {"B", "O"};
    cfg1.leg["BTC"] = CeLegParams{12.0, 0.10, 8.0, 0.05};  // stock_depth>0
    cfg1.leg["USD"] = CeLegParams{18.0, 0.05, 6.0, 0.02};
    cfg1.mark["BTC"] = 0.10; cfg1.mark["USD"] = 0.0;
    std::vector<CeQuoteParams> quotes1 = {
        {"BTC", "B", 0.00, 50.0, 0.05}, {"BTC", "O", 0.40, 50.0, 0.05}};
    CeClearInput in1 = AssembleCeGraph(cfg1, quotes1);
    CeClearResult r1 = ClearCe(in1);
    std::vector<CeAgentDelta> ad1 = ProjectAgentDeltas(in1, r1, {});
    for (const auto& d : ad1) {
      check(d.agent_id.rfind("S_", 0) != 0, "kStock (S_*) исключён из ProjectAgentDeltas");
    }
  }

  if (fail == 0) std::printf("ce_position_projection_test (agent deltas, T-F18-201): OK\n");
  return fail;
}
}  // namespace

using namespace cex::matching::domain;

namespace {
int g_fail = 0;
bool close(double a, double b, double tol, const char* what) {
  if (std::fabs(a - b) > tol) {
    std::printf("FAIL %s: got %.10g want %.10g\n", what, a, b); ++g_fail; return false;
  }
  return true;
}
}  // namespace

int main() {
  CeAssembleConfig cfg;
  cfg.numeraire = "USD"; cfg.assets = {"BTC"}; cfg.venues = {"B", "O", "K"};
  cfg.leg["BTC"] = CeLegParams{12.0, 0.25, 8.0, 0.15};
  cfg.leg["USD"] = CeLegParams{18.0, 0.05, 60.0, 0.02};
  cfg.mark["BTC"] = 0.072; cfg.mark["USD"] = 0.0;
  std::vector<CeQuoteParams> quotes = {
      {"BTC", "B", 0.00, 60.0, 0.10}, {"BTC", "O", 0.35, 40.0, 0.10},
      {"BTC", "K", -0.20, 25.0, 0.14}};

  CeClearInput in = AssembleCeGraph(cfg, quotes);
  CeClearResult r = ClearCe(in);
  std::map<std::string, double> ref = {{"BTC", 60000.0}, {"USD", 1.0}};
  std::vector<CeAssetVenueDelta> d = ProjectPositionQuantity(in, r, ref);

  // Только две активные quote-ноги (BTC@O продажа, BTC@K покупка); BTC@B в мёртвой зоне.
  close(static_cast<double>(d.size()), 2, 0, "num deltas");

  double sum_value = 0.0, sum_qty = 0.0, p_O = 0.0, p_K = 0.0, q_O = 0.0, q_K = 0.0;
  for (const auto& x : d) {
    sum_value += x.delta_value; sum_qty += x.delta_qty;
    // авторитетность: delta_value == delta_qty·price/1000
    close(x.delta_value, x.delta_qty * x.price_used / 1000.0, 1e-9, "value==qty·price");
    if (x.venue == "O") { p_O = x.price_used; q_O = x.delta_qty; }
    if (x.venue == "K") { p_K = x.price_used; q_K = x.delta_qty; }
  }

  // цены узлов РАЗНЫЕ (per-leg, не общая марка) — суть поправки владельца
  close(p_O, 60013.4844, 1e-1, "P_node(BTC@O)");
  close(p_K, 59997.9577, 1e-1, "P_node(BTC@K)");
  if (!(std::fabs(p_O - p_K) > 1.0)) { std::printf("FAIL: цены узлов должны различаться\n"); ++g_fail; }

  // продали на O (qty<0), купили на K (qty>0)
  close(q_O, -0.00211339, 1e-6, "Δqty(BTC@O) sold");
  close(q_K, +0.00175189, 1e-6, "Δqty(BTC@K) bought");

  // value-сохранение: Σ delta_value = dBTC (−Σ f_quote), тремя способами согласовано
  const double dBTC_value = r.f[9] + r.f[10] + r.f[11];  // stock legs
  close(sum_value, dBTC_value, 1e-6, "Σ delta_value == dBTC (V-CEA-003)");
  close(sum_value, -0.02172262765169619, 1e-6, "Σ delta_value == −0.0217226");

  // демонстрация ошибки агрегата: деление dBTC_value на общую марку ≠ сумма по ногам.
  const double p_marka = 60000.0 * std::exp(cfg.mark.at("BTC") / 1000.0);
  const double q_aggregate = (dBTC_value * 1000.0) / p_marka;
  if (std::fabs(q_aggregate - sum_qty) < 1e-12) {
    std::printf("FAIL: агрегат/марка совпал с per-leg — на этом сетапе должен отличаться\n");
    ++g_fail;
  }
  std::printf("info: per-leg Σqty=%.10g, aggregate/marka=%.10g, разница=%.3g BTC\n",
              sum_qty, q_aggregate, sum_qty - q_aggregate);

  // §A7: заявки = потоки клиринга. Ожидаем SELL BTC@O, BUY BTC@K, перевод BTC K→O.
  CeOrders ord = ProjectOrders(in, r, ref);
  close(static_cast<double>(ord.venue_orders.size()), 2, 0, "venue_orders (T_O sell, T_K buy)");
  close(static_cast<double>(ord.transfers.size()), 1, 0, "transfers (A_BTC_OK)");
  for (const auto& o : ord.venue_orders) {
    if (o.venue == "O") { close(o.qty, 0.00211339, 1e-6, "SELL BTC@O qty"); if (o.side != "SELL") { std::printf("FAIL: BTC@O должна быть SELL\n"); ++g_fail; } }
    if (o.venue == "K") { close(o.qty, 0.00175189, 1e-6, "BUY BTC@K qty");  if (o.side != "BUY")  { std::printf("FAIL: BTC@K должна быть BUY\n"); ++g_fail; } }
  }
  if (!ord.transfers.empty()) {
    const auto& t = ord.transfers[0];
    if (!(t.asset == "BTC" && t.from_venue == "K" && t.to_venue == "O")) {
      std::printf("FAIL: перевод должен быть BTC K→O (got %s %s→%s)\n", t.asset.c_str(), t.from_venue.c_str(), t.to_venue.c_str()); ++g_fail;
    }
    close(t.qty, 0.00175136, 1e-6, "transfer BTC qty");
  }

  g_fail += RunAgentDeltasV2Test();

  if (g_fail == 0) { std::printf("ce_position_projection_test: OK\n"); return 0; }
  std::printf("ce_position_projection_test: %d FAIL\n", g_fail);
  return 1;
}
