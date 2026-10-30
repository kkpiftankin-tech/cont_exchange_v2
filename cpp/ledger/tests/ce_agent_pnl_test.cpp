// ============================================================================
// ce_agent_pnl_test.cpp — ADR-087 / CALC-CE-AGENT-PNL (модель C′). Заработок агентов CE по биржам через ledger целиком:
//   1) переводчик: покупки на CE → хедж → GetAgentPnl (R внутренней биржи = выгода к справедливой цене, R внешних = дрейф + исполнение,
//      U = остаток по текущей справедливой цене);
//   2) связка: круговая сделка за такт, справедливая цена берётся у переводчиков того же актива;
//   3) сброс позиций обнуляет всё; выключатель CE_AGENT_PNL_ENABLED=0;
//   4) перезапуск: книги восстанавливаются из репозитория (заглушка вместо PostgreSQL); R известен сразу, U — после первой цены;
//   5) повторная доставка такта не удваивает книгу; засев книги по существующей позиции; хедж связки; кросс-пара (ρ≠1);
//      регресс при CE_AGENT_PNL_ENABLED=0; сбой загрузки книг не затирает сохранённые и после восстановления учёт продолжается;
//   6) закон сохранения внутренней биржи: Σ R внутренней группы по всем агентам равна нулю; R+U каждого агента сверен с прямым пересчётом.
// Pure unit: без PG и Kafka. CE_AGENT_BAND не задан ⇒ автоматической эмиссии хеджа нет, исполнение регистрируется
// через RememberExecutionIntent, как в ce_band_house_realized_test.
// ============================================================================
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "app/ledger_uc.hpp"
#include "app/persistence_ports.hpp"

using cex::common::Decimal;
using cex::ledger::app::AgentPnlBookRow;
using cex::ledger::app::AgentPositionRepositoryPort;
using cex::ledger::app::AgentPositionRow;
using cex::ledger::app::LedgerUseCases;

namespace {

int g_fail = 0;

void Near(double got, double want, double tol, const char* msg) {
  if (std::isnan(got) || std::fabs(got - want) > tol) {
    std::cerr << "FAILED: " << msg << " — got " << got << ", want " << want << '\n';
    ++g_fail;
  }
}
void Check(bool ok, const char* msg) {
  if (!ok) { std::cerr << "FAILED: " << msg << '\n'; ++g_fail; }
}

double Num(const fob::common::v1::Decimal& d) { return static_cast<double>(Decimal::from_proto(d)); }

// Заглушка репозитория (вместо PostgreSQL): накапливает позиции и книги в памяти — как это делает PG.
class FakeRepo : public AgentPositionRepositoryPort {
 public:
  using Key = std::tuple<std::string, std::string, std::string>;
  std::map<Key, AgentPositionRow> rows;
  std::map<std::string, AgentPnlBookRow> books;
  int reset_calls = 0;
  bool fail_books = false;  // имитация «схема не готова»: LoadBooks возвращает false

  AgentPositionRow ApplyDelta(const std::string& agent_id, const std::string& agent_kind, const std::string& asset,
                              const std::string& venue, const Decimal& delta, const std::string& batch_id,
                              int64_t updated_at_ms) override {
    auto& r = rows[Key{agent_id, asset, venue}];
    r.agent_id = agent_id; r.asset = asset; r.venue = venue;
    if (!agent_kind.empty()) r.agent_kind = agent_kind;
    if (!r.last_batch_id.empty() && r.last_batch_id == batch_id) return r;  // guard по batch_id, как UPSERT в PG
    r.position = Decimal::add(r.position, delta);
    r.last_batch_id = batch_id;
    r.updated_at_ms = updated_at_ms;
    return r;
  }
  std::vector<AgentPositionRow> GetPositions(const std::vector<std::string>&, const std::vector<std::string>&,
                                             const std::vector<std::string>&) override {
    std::vector<AgentPositionRow> out;
    for (const auto& kv : rows) out.push_back(kv.second);
    return out;
  }
  void ApplyHedge(const std::string& agent_id, const std::string& asset, const std::string& venue, const Decimal& dpos,
                  const Decimal& dif, int64_t, const Decimal& drealized) override {
    auto& r = rows[Key{agent_id, asset, venue}];
    r.position = Decimal::add(r.position, dpos);
    r.in_flight = Decimal::add(r.in_flight, dif);
    r.realized_vs_fair = Decimal::add(r.realized_vs_fair, drealized);
  }
  void SaveBooks(const std::vector<AgentPnlBookRow>& b) override {
    for (const auto& x : b) books[x.agent_id] = x;
  }
  bool LoadBooks(std::vector<AgentPnlBookRow>& out) override {
    out.clear();
    if (fail_books) return false;
    for (const auto& kv : books) out.push_back(kv.second);
    return true;
  }
  void ResetPnl(const std::vector<std::string>& ids) override {
    ++reset_calls;
    for (auto it = books.begin(); it != books.end();) {
      if (ids.empty() || std::find(ids.begin(), ids.end(), it->first) != ids.end()) it = books.erase(it); else ++it;
    }
  }
};

Decimal Dec(double v, int scale = 8) {
  return Decimal{static_cast<int64_t>(std::llround(v * std::pow(10.0, scale))), scale};
}

LedgerUseCases::AgentDelta MkDelta(const std::string& id, const std::string& kind, const std::string& asset,
                                   const std::string& venue, double kusd, double base_price, double fair = 0.0) {
  LedgerUseCases::AgentDelta d;
  d.agent_id = id; d.agent_kind = kind; d.asset = asset; d.venue = venue;
  d.delta = Dec(kusd, 6);
  d.base_price = Dec(base_price, 2);
  d.price_used = d.base_price;  // пара BTC/USDT: цена пары = P_base
  if (fair != 0.0) d.fair_price = Dec(fair, 2);
  d.quote = (kind == "translator") ? "USDT" : "";
  return d;
}

// Дельта с отдельной ценой пары (кросс-пара ETH/BTC): price_used — валюта котировки за штуку, base_price — USDT за штуку.
LedgerUseCases::AgentDelta MkDeltaX(const std::string& id, const std::string& kind, const std::string& asset,
                                    const std::string& venue, double kusd, double base_price, double pair_price,
                                    double fair, const std::string& quote) {
  LedgerUseCases::AgentDelta d = MkDelta(id, kind, asset, venue, kusd, base_price, 0.0);
  d.price_used = Dec(pair_price, 8);
  d.fair_price = Dec(fair, 8);
  d.quote = quote;
  return d;
}

fob::execution::v1::ExecutionIntent MkSellIntent(const std::string& agent, const std::string& asset,
                                                  const std::string& venue, double sent_kusd, double base_price) {
  fob::execution::v1::ExecutionIntent it;
  const std::string flow = "ce|band|" + agent + "|" + asset + "|" + venue;
  it.set_hedge_flow_id(flow);
  it.set_intent_id(flow + "|1");
  it.set_client_order_id(flow + "|1");
  it.set_venue(venue);
  it.mutable_instrument()->set_symbol(asset + "/USDT");
  it.mutable_instrument()->set_base(asset);
  it.mutable_instrument()->set_quote("USDT");
  it.set_venue_symbol(asset + "/USDT");
  it.set_side(fob::common::v1::SIDE_SELL);
  *it.mutable_target_notional() = Dec(sent_kusd * 1000, 2).to_proto();
  *it.mutable_target_qty() = Dec(sent_kusd * 1000.0 / base_price, 8).to_proto();
  return it;
}

// Та же заявка, но со стороной BUY (закрывает короткую внутреннюю позицию переводчика).
fob::execution::v1::ExecutionIntent MkBuyIntent(const std::string& agent, const std::string& asset,
                                                 const std::string& venue, double sent_kusd, double base_price) {
  fob::execution::v1::ExecutionIntent it = MkSellIntent(agent, asset, venue, sent_kusd, base_price);
  it.set_side(fob::common::v1::SIDE_BUY);
  return it;
}

fob::execution::v1::ExecutionReport MkFill(const std::string& agent, const std::string& asset, const std::string& venue,
                                           double filled, double avg_price, const std::string& report_id, int price_scale = 2) {
  fob::execution::v1::ExecutionReport r;
  const std::string flow = "ce|band|" + agent + "|" + asset + "|" + venue;
  r.set_intent_id(flow + "|1");
  r.set_hedge_flow_id(flow);
  r.set_client_order_id(flow + "|1");
  r.set_report_id(report_id);
  r.set_venue(venue);
  r.mutable_instrument()->set_symbol(asset + "/USDT");
  r.mutable_instrument()->set_base(asset);
  r.mutable_instrument()->set_quote("USDT");
  r.set_venue_symbol(asset + "/USDT");
  r.set_status(fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED);
  *r.mutable_filled_qty() = Dec(filled, 8).to_proto();
  *r.mutable_remaining_qty() = Dec(0.0, 8).to_proto();
  *r.mutable_average_price() = Dec(avg_price, price_scale).to_proto();
  return r;
}

void Apply(LedgerUseCases& uc, const fob::execution::v1::ExecutionReport& rep) {
  fob::ledger::v1::ApplyExecutionReportRequest req;
  *req.mutable_report() = rep;
  uc.ApplyExecutionReport(req);
}

const fob::ledger::v1::AgentPnl* Find(const fob::ledger::v1::GetAgentPnlResponse& r, const std::string& id) {
  for (const auto& a : r.agents()) if (a.agent_id() == id) return &a;
  return nullptr;
}

// Переводчик §3 карточки: +2 BTC @ 80 000 (F_0 = 80 000), +1 BTC @ 80 030 (F_0 = 80 040), хедж SELL 2 BTC @ 80 060, затем малая сделка при F_0 = 80 050.
//   R внутр. = (80040−80030)·1 = 10; R внеш. = исполнение (80060−80040)·2 = 40 + дрейф 2·(80040−80013,33) = 53,33 ⇒ 93,33;
//   остаток 1 BTC со стоимостью входа 80 013,33 при F_0 = 80 050 ⇒ U = 36,67; R+U = 140 = −160000 − 80030 + 160120 + 80050 по кассе.
void RunTranslatorScenario(LedgerUseCases& uc, double tick3_kusd = 0.0008, double tick3_price = 80050.0) {
  uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 160.0, 80000.0, 80000.0)});
  uc.ApplyPositionDelta("b2", 2000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 80.03, 80030.0, 80040.0)});
  uc.RememberExecutionIntent(MkSellIntent("T_BTC_binance", "BTC", "binance", 160.06, 80030.0));
  Apply(uc, MkFill("T_BTC_binance", "BTC", "binance", 2.0, 80060.0, "rep-1"));
  // Малая сделка по новой цене: F_0 = 80 050 (цена сделки может отличаться — тогда она даёт выгоду к F_0).
  uc.ApplyPositionDelta("b3", 3000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", tick3_kusd, tick3_price, 80050.0)});
}

void CheckTranslator(const fob::ledger::v1::GetAgentPnlResponse& r, const char* tag) {
  const auto* a = Find(r, "T_BTC_binance");
  if (a == nullptr) { std::cerr << "FAILED: " << tag << ": агент не найден\n"; ++g_fail; return; }
  Check(a->internal().has_data() && a->external().has_data() && a->total().has_data(), tag);
  Check(a->internal().u_has_data() && a->external().u_has_data() && a->total().u_has_data(), "T: U известен (есть цена оценки)");
  Near(Num(a->internal().r()), 10.0, 1e-3, "T: R внутренней биржи = (80040−80030)·1");
  Near(Num(a->internal().u()), 0.0, 1e-12, "T: U внутренней биржи ≡ 0 — на внутренней бирже всё реализуется");
  Near(Num(a->internal().r_plus_u()), Num(a->internal().r()), 1e-12, "T: на внутренней бирже R+U = R");
  Near(Num(a->external().r()), 93.3333, 1e-3, "T: R внешних = исполнение 40 + дрейф 53,33");
  Near(Num(a->external().u()), 36.6667, 0.01, "T: U внешних = остаток 1 BTC · (80050 − 80013,33)");
  Near(Num(a->total().r()), 103.3333, 1e-3, "T: R итого = R внутр. + R внеш.");
  Near(Num(a->total().u()), Num(a->external().u()), 1e-12, "T: U итого = U внешних");
  Near(Num(a->total().r_plus_u()), 140.0, 0.01, "T: R+U = результат агента по кассе (140)");
  Check(a->ext_fills() == 1, "T: одно внешнее исполнение");
  Check(a->since_ms() > 0, "T: начало накопления задано");
  Check(a->mark_age_ms() >= 0, "T: возраст цены известен");
  Check(a->venues_size() == 1 && a->venues(0) == "binance", "T: площадка binance");
  // Строка «все агенты» совпадает с единственным агентом.
  Near(Num(r.totals().total().r_plus_u()), Num(a->total().r_plus_u()), 1e-9, "T: totals = сумма по агентам");
  Check(r.totals().agent_count() == 1 && r.totals().incomplete_agents() == 0, "T: totals — счётчики");
}

// Замкнутый цикл за один такт при общей справедливой цене F_0 = 80 040: переводчик T1 (binance) продаёт связке 0,125 BTC по 80 000, связка
// везёт на kraken и продаёт переводчику T2 по 80 080 (та же стоимость 10 тыс. USDT на обеих ногах).
void Cycle(LedgerUseCases& uc) {
  uc.ApplyPositionDelta("z1", 1000, {},
                        {MkDelta("T_BTC_binance", "translator", "BTC", "binance", -10.0, 80000.0, 80040.0),
                         MkDelta("A_BTC_binancekraken", "arbitrageur", "BTC", "binance", -10.0, 80000.0),
                         MkDelta("A_BTC_binancekraken", "arbitrageur", "BTC", "kraken", 10.0, 80080.0),
                         MkDelta("T_BTC_kraken", "translator", "BTC", "kraken", 10.0, 80080.0, 80040.0)});
}

double SumInternalR(LedgerUseCases& uc) {
  const auto resp = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
  double sum = 0.0;
  for (const auto& a : resp.agents())
    if (a.internal().has_data()) sum += Num(a.internal().r());
  return sum;
}

}  // namespace

int main() {
  setenv("CE_NUMERAIRE", "USDT", 1);
  unsetenv("CE_AGENT_BAND");
  unsetenv("CE_AGENT_PNL_ENABLED");
  setenv("CE_AGENT_PNL_FAIR_MAX_AGE_MS", "0", 1);  // метки времени в сценариях условные (1000, 2000, …): свежесть проверяет отдельный сценарий 5j

  // --- 1) переводчик без репозитория ---------------------------------------------------------
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    RunTranslatorScenario(uc);
    CheckTranslator(uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{}), "T: данные есть");
  }

  // --- 2) связка: круговая сделка за такт ---------------------------------------------------
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    Cycle(uc);
    const auto r = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* a = Find(r, "A_BTC_binancekraken");
    if (a == nullptr) { std::cerr << "FAILED: связка не найдена\n"; ++g_fail; }
    else {
      // Покупка на источнике 0,125 BTC по 80 000: (80040−80000)·0,125 = 5; продажа на назначении 0,12487512 BTC по 80 080: (80080−80040)·0,12487512 = 4,995.
      Near(Num(a->internal().r()), 9.9950048, 1e-5, "L: R внутренней биржи = выгода обеих ног к справедливой цене пары");
      Near(Num(a->internal().u()), 0.0, 1e-12, "L: U внутренней биржи ≡ 0");
      Check(a->external().has_data() && a->external().u_has_data(), "L: внешняя группа — данные есть");
      // Остаток паритета 0,00012488 BTC открыт по справедливой стоимости 80 040 и оценён по ней же ⇒ U = 0.
      Near(Num(a->external().r_plus_u()), 0.0, 1e-9, "L: внешних исполнений нет, остаток паритета при F_0 входа ⇒ нули");
      Near(Num(a->total().r_plus_u()), Num(a->internal().r_plus_u()), 1e-9, "L: итого = внутренняя");
      Check(a->venues_size() == 2, "L: две площадки");
    }
  }

  // --- 3) сброс и выключатель ------------------------------------------------------------------
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    RunTranslatorScenario(uc);
    uc.ResetAgentPositions(fob::ledger::v1::ResetAgentPositionsRequest{});
    const auto r = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* a = Find(r, "T_BTC_binance");
    Check(a != nullptr, "R: агент остаётся после сброса");
    if (a != nullptr) {
      Check(a->total().has_data() && a->total().u_has_data(), "R: после сброса данные известны (позиции нет — оценивать нечего)");
      Near(Num(a->internal().r()), 0.0, 1e-12, "R: внутренний R обнулён");
      Near(Num(a->external().r()), 0.0, 1e-12, "R: внешний R обнулён");
      Near(Num(a->external().u()), 0.0, 1e-12, "R: внешний U обнулён");
      Check(a->ext_fills() == 0 && a->since_ms() == 0, "R: счётчик и начало накопления сброшены");
    }
    setenv("CE_AGENT_PNL_ENABLED", "0", 1);
    const auto off = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    Check(!off.enabled() && off.agents_size() == 0 && off.by_kind_size() == 0, "R: выключатель ⇒ пустой список и пустые итоги по видам");
    unsetenv("CE_AGENT_PNL_ENABLED");
  }

  // --- 4) перезапуск: R известен сразу из книги, U — после первой цены -----------------------------
  {
    auto repo = std::make_shared<FakeRepo>();
    {
      LedgerUseCases uc(LedgerUseCases::InitOptions{});
      uc.SetAgentPositionRepo(repo);
      RunTranslatorScenario(uc);
      CheckTranslator(uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{}), "P: до перезапуска");
      Check(repo->books.count("T_BTC_binance") == 1, "P: книга записана в репозиторий");
      Check(repo->books["T_BTC_binance"].quote == "USDT", "P: валюта котировки записана вместе с книгой");
    }
    LedgerUseCases uc2(LedgerUseCases::InitOptions{});
    uc2.SetAgentPositionRepo(repo);  // LoadAgentPositionsFromRepo: позиции и книги
    const auto r = uc2.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* a = Find(r, "T_BTC_binance");
    Check(a != nullptr, "P: агент восстановлен");
    if (a != nullptr) {
      // R цены оценки не требует: накопленное известно сразу. U — остаток 1 BTC — без текущей справедливой цены не определён.
      Check(a->internal().has_data() && a->external().has_data() && a->total().has_data(), "P: после перезапуска R известен без цены");
      Check(a->internal().u_has_data() && !a->external().u_has_data() && !a->total().u_has_data(),
            "P: U внешних не определён до первой цены (внутренний U ≡ 0 известен)");
      Near(Num(a->internal().r()), 10.0, 1e-3, "P: R внутренней биржи сохранён через перезапуск");
      Near(Num(a->external().r()), 93.3333, 1e-3, "P: R внешних сохранён через перезапуск");
      Check(!a->total().has_r_plus_u(), "P: R+U не задан, пока U не известен");
      Near(Num(r.totals().internal().r()), Num(a->internal().r()), 1e-12, "P: итоги включают R агента без U");
      Check(r.totals().incomplete_agents() == 1, "P: агент с неизвестным U помечен неполным");
      Check(!r.totals().total().has_r_plus_u() && r.totals().total().has_data() && !r.totals().total().u_has_data(),
            "P: итоги при неполных агентах — R есть, U нет, R+U не отдаётся (разные выборки)");
    }
    // Первая же дельта с ценой возвращает U; накопленные R сохранились.
    uc2.ApplyPositionDelta("b4", 4000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 0.0008, 80050.0, 80050.0)});
    const auto r2 = uc2.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* b = Find(r2, "T_BTC_binance");
    if (b == nullptr || !b->total().has_data() || !b->total().u_has_data()) { std::cerr << "FAILED: P: после дельты данные должны появиться\n"; ++g_fail; }
    else {
      Near(Num(b->internal().r()), 10.0, 1e-3, "P: R внутренней биржи после дельты");
      Near(Num(b->external().r()), 93.3333, 1e-3, "P: R внешних после дельты");
      Near(Num(b->total().r_plus_u()), 140.0, 0.01, "P: R+U после перезапуска и первой цены тот же, что до него");
    }
    uc2.ResetAgentPositions(fob::ledger::v1::ResetAgentPositionsRequest{});
    Check(repo->reset_calls == 1 && repo->books.empty(), "P: сброс очищает книги в репозитории");
  }

  // --- 5a) повторная доставка такта после перезапуска не удваивает книгу -------------------------
  {
    auto repo = std::make_shared<FakeRepo>();
    double r_ref = 0.0, u_ref = 0.0;
    {
      LedgerUseCases uc(LedgerUseCases::InitOptions{});
      uc.SetAgentPositionRepo(repo);
      // Третий такт "b3": 0,0999 BTC по 80 100 при F_0 = 80 050 ⇒ R внутр. += (80050−80100)·0,0999 = −4,99 (двойной учёт дал бы −9,99).
      RunTranslatorScenario(uc, 8.0, 80100.0);
      const auto resp_a = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
      const auto* a = Find(resp_a, "T_BTC_binance");
      if (a == nullptr || !a->total().has_data()) { std::cerr << "FAILED: D: нет данных до перезапуска\n"; ++g_fail; }
      else { r_ref = Num(a->internal().r()); u_ref = Num(a->external().u()); }
    }
    LedgerUseCases uc2(LedgerUseCases::InitOptions{});
    uc2.SetAgentPositionRepo(repo);
    // Kafka доставляет тот же такт "b3" ещё раз: guard позиции (последний batch_id) и guard книги должны его отсечь.
    uc2.ApplyPositionDelta("b3", 3000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 8.0, 80100.0, 80050.0)});
    const auto resp_a2 = uc2.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* a2 = Find(resp_a2, "T_BTC_binance");
    if (a2 == nullptr || !a2->total().has_data() || !a2->total().u_has_data()) { std::cerr << "FAILED: D: нет данных после повторной доставки\n"; ++g_fail; }
    else {
      Near(Num(a2->internal().r()), r_ref, 1e-9, "D: R внутренней биржи не изменился");
      Near(Num(a2->external().u()), u_ref, 1e-6, "D: повторный такт не изменил остаток книги");
    }
    Check(r_ref > 5.0 && r_ref < 5.01, "D: контроль — R внутр. = 10 − 4,99 ≈ 5,006; удвоение третьего такта дало бы ≈ 0,01");
  }

  // --- 5b) засев книги по существующей позиции -------------------------------------------------
  {
    auto repo = std::make_shared<FakeRepo>();
    AgentPositionRow row;
    row.agent_id = "T_BTC_binance"; row.agent_kind = "translator"; row.asset = "BTC"; row.venue = "binance";
    row.position = Dec(80.03, 6);  // позиция 1 BTC (по 80 030), книги ещё нет
    repo->rows[FakeRepo::Key{"T_BTC_binance", "BTC", "binance"}] = row;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetAgentPositionRepo(repo);
    uc.ApplyPositionDelta("s1", 1000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 0.0008, 80030.0, 80040.0)});
    const auto resp_a = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* a = Find(resp_a, "T_BTC_binance");
    Check(a != nullptr && a->external().u_has_data(), "S: данные есть");
    if (a != nullptr) Near(Num(a->external().u()), 0.0, 0.01, "S: сразу после засева U внешних ≈ 0 (позиция входит по текущей F_0)");
    // Хедж 1 BTC закрывает засеянный остаток по его справедливой стоимости; потом F_0 уходит на 80 050.
    uc.RememberExecutionIntent(MkSellIntent("T_BTC_binance", "BTC", "binance", 80.03, 80030.0));
    Apply(uc, MkFill("T_BTC_binance", "BTC", "binance", 1.0, 80060.0, "seed-rep"));
    uc.ApplyPositionDelta("s2", 2000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 0.0008, 80050.0, 80050.0)});
    const auto resp_b = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* b = Find(resp_b, "T_BTC_binance");
    if (b == nullptr) { std::cerr << "FAILED: S: агент не найден\n"; ++g_fail; }
    else {
      Near(Num(b->external().r()), 20.0, 0.01, "S: R внешних = (80060−80040)·1 (исполнение; дрейфа нет — F_0 входа = F_0 выхода)");
      Near(Num(b->external().u()), 0.0, 0.01, "S: хедж закрыл засеянный остаток ⇒ внешний U ≈ 0 (без засева был бы −10)");
    }
  }

  // --- 5c) хедж связки: в книгу не пишется (знак рыночного хеджа ноги противоположен знаку её книги, OQ-F18v2-31); счётчик исполнений растёт ---
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    Cycle(uc);
    uc.RememberExecutionIntent(MkSellIntent("A_BTC_binancekraken", "BTC", "kraken", 40.045, 80090.0));
    Apply(uc, MkFill("A_BTC_binancekraken", "BTC", "kraken", 0.5, 80090.0, "link-rep"));
    const auto resp_a = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* a = Find(resp_a, "A_BTC_binancekraken");
    if (a == nullptr) { std::cerr << "FAILED: L2: связка не найдена\n"; ++g_fail; }
    else {
      Near(Num(a->internal().r()), 9.9950048, 1e-5, "L2: R внутренней биржи связки не тронут хеджем");
      Near(Num(a->external().r()), 0.0, 1e-12, "L2: хедж связки в R внешних не попадает (OQ-F18v2-31)");
      Near(Num(a->external().u()), 0.0, 1e-6, "L2: остаток паритета при F_0 входа ⇒ U = 0");
      Check(a->ext_fills() == 1, "L2: одно внешнее исполнение посчитано");
    }
  }

  // --- 5d) кросс-пара ETH/BTC: курс валюты котировки ρ = P_b / F_0 ≠ 1 --------------------------
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.ApplyPositionDelta("x1", 1000, {}, {MkDeltaX("T_ETH_binance", "translator", "ETH", "binance", 3.0, 3000.0, 0.05, 0.05, "BTC")});
    uc.ApplyPositionDelta("x2", 2000, {}, {MkDeltaX("T_ETH_binance", "translator", "ETH", "binance", 3.06, 3060.0, 0.051, 0.0515, "BTC")});
    const auto resp_a = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* a = Find(resp_a, "T_ETH_binance");
    Check(a != nullptr && a->internal().has_data() && a->external().u_has_data(), "X: данные есть");
    if (a != nullptr) {
      // ρ = 3060/0,0515 ≈ 59 417,5 USDT за BTC. Вторая покупка 1 ETH по 0,051 при F_0 = 0,0515 ⇒ R = 0,0005 BTC ≈ 29,71 USDT.
      Near(Num(a->internal().r()), 29.7087, 0.01, "X: R внутренней биржи в USDT через курс BTC");
      // L = 2 ETH, F_in = 0,05075, оценка 0,0515 ⇒ U = 0,0015 BTC ≈ 89,13 USDT.
      Near(Num(a->external().u()), 89.1262, 0.01, "X: U внешних в USDT через курс BTC");
      Near(Num(a->external().r()), 0.0, 1e-9, "X: внешних исполнений нет");
      Check(a->quote() == "BTC", "X: котируемая валюта BTC");
    }
  }

  // --- 5e) регресс: учёт PnL не меняет прежние поля позиции ---------------------------------------
  {
    auto positions_of = [](LedgerUseCases& uc) {
      std::vector<double> v;
      const auto resp = uc.GetAgentPositions(fob::ledger::v1::GetAgentPositionsRequest{});  // не временный: range-for по полю временного — висячая ссылка
      for (const auto& p : resp.positions())
        if (p.agent_id() == "T_BTC_binance")
          v = {Num(p.position()), Num(p.in_flight()), Num(p.realized_vs_fair())};
      return v;
    };
    LedgerUseCases on(LedgerUseCases::InitOptions{});
    RunTranslatorScenario(on);
    setenv("CE_AGENT_PNL_ENABLED", "0", 1);
    LedgerUseCases off(LedgerUseCases::InitOptions{});
    RunTranslatorScenario(off);
    unsetenv("CE_AGENT_PNL_ENABLED");
    const auto a = positions_of(on), b = positions_of(off);
    Check(a.size() == 3 && b.size() == 3, "G: строки позиции найдены");
    if (a.size() == 3 && b.size() == 3) {
      Near(a[0], b[0], 1e-12, "G: position одинаков при вкл/выкл учёта");
      Near(a[1], b[1], 1e-12, "G: in_flight одинаков");
      Near(a[2], b[2], 1e-12, "G: realized_vs_fair одинаков");
    }
  }

  // --- 5f) сбой загрузки книг не затирает сохранённые; после восстановления учёт продолжается ----------
  {
    auto repo = std::make_shared<FakeRepo>();
    {
      LedgerUseCases uc(LedgerUseCases::InitOptions{});
      uc.SetAgentPositionRepo(repo);
      RunTranslatorScenario(uc, 8.0);
    }
    const Decimal qty_saved = repo->books["T_BTC_binance"].qty;
    const Decimal edge_saved = repo->books["T_BTC_binance"].edge;
    repo->fail_books = true;
    setenv("CE_AGENT_PNL_RETRY_MS", "0", 1);
    LedgerUseCases uc2(LedgerUseCases::InitOptions{});
    uc2.SetAgentPositionRepo(repo);
    const auto resp_a0 = uc2.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* a0 = Find(resp_a0, "T_BTC_binance");
    Check(a0 != nullptr && !a0->internal().has_data() && !a0->external().has_data() && !a0->total().has_data(),
          "F: книги не загружены ⇒ «нет данных», не нули");
    // Итоги по видам при незагруженных книгах: агенты посчитаны, но все неполные, суммы без данных.
    const auto resp0 = uc2.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    Check(resp0.by_kind_size() == 1 && resp0.by_kind(0).agent_kind() == "translator" &&
              resp0.by_kind(0).agent_count() == resp0.by_kind(0).incomplete_agents() && !resp0.by_kind(0).total().has_data() &&
              !resp0.totals().total().has_data(),
          "F: by_kind при незагруженных книгах — все агенты неполные, итогов нет");
    uc2.ApplyPositionDelta("f1", 5000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 8.0, 80050.0, 80050.0)});
    Check(repo->books["T_BTC_binance"].qty.units == qty_saved.units && repo->books["T_BTC_binance"].qty.scale == qty_saved.scale &&
              Decimal::cmp(repo->books["T_BTC_binance"].edge, edge_saved) == 0,
          "F: пока книги не загружены, сохранённая книга не перезаписывается");
    repo->fail_books = false;
    uc2.ApplyPositionDelta("f2", 6000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 8.0, 80050.0, 80050.0)});
    const auto resp_a1 = uc2.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* a1 = Find(resp_a1, "T_BTC_binance");
    Check(a1 != nullptr && a1->total().has_data() && a1->total().u_has_data(), "F: после восстановления учёт возобновлён");
    if (a1 != nullptr && a1->total().has_data()) {
      Near(Num(a1->internal().r()), 10.0, 1e-3, "F: накопленный R внутренней биржи сохранён");
      Near(Num(a1->external().r()), 93.3333, 1e-3, "F: накопленный R внешних сохранён");
    }
    unsetenv("CE_AGENT_PNL_RETRY_MS");
  }

  // --- 5h) агент неизвестного вида в расчёт заработка не входит ------------------------------------------------
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.ApplyPositionDelta("u1", 1000, {}, {MkDelta("X_UNKNOWN", "", "BTC", "binance", 5.0, 80000.0, 80000.0)});
    const auto resp = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    Check(Find(resp, "X_UNKNOWN") == nullptr && resp.by_kind_size() == 0 && resp.totals().agent_count() == 0,
          "U: агент неизвестного вида не попадает ни в список, ни в итоги");
  }

  // --- 5i) закон сохранения внутренней биржи (вопрос владельца 2026-10-28: «суммарный R по всем агентам должен быть нулём»).
  // Внутренняя биржа бесплатна и замкнута, справедливая цена пары общая, поэтому Σ R внутренней группы по всем агентам равна нулю;
  // внутренний U ≡ 0. Внешний результат каждого агента сверен с прямым пересчётом по кассе.
  {
    // (а) цикл открыт: у каждого агента R внутр. — выгода его сделки к F_0, и сумма равна нулю.
    {
      LedgerUseCases uc(LedgerUseCases::InitOptions{});
      Cycle(uc);
      Near(SumInternalR(uc), 0.0, 1e-6, "Z: Σ R внутренней группы по всем агентам = 0");
      const auto resp = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
      const auto* t1 = Find(resp, "T_BTC_binance");
      const auto* t2 = Find(resp, "T_BTC_kraken");
      const auto* l = Find(resp, "A_BTC_binancekraken");
      Check(t1 != nullptr && t2 != nullptr && l != nullptr, "Z: все три агента найдены");
      if (t1 != nullptr && t2 != nullptr && l != nullptr) {
        Near(Num(t1->internal().r()), -5.0, 1e-6, "Z: T1 продал по 80 000 при F=80 040 ⇒ −(40·0,125)");
        Near(Num(t2->internal().r()), -4.9950048, 1e-5, "Z: T2 купил по 80 080 при F=80 040 ⇒ −(40·0,12487512)");
        Near(Num(l->internal().r()), 9.9950048, 1e-5, "Z: связка получила обе выгоды");
        for (const auto* g : {t1, t2, l})
          Near(Num(g->internal().u()), 0.0, 1e-12, "Z: внутренний U ≡ 0 у каждого");
        // Позиции открыты по F_0, оценены по ней же: внешний результат пока нулевой.
        Near(Num(t1->total().r_plus_u()) + Num(t2->total().r_plus_u()) + Num(l->total().r_plus_u()), 0.0, 1e-6,
             "Z: Σ(R+U) = 0 пока ни один агент не вышел на внешнюю биржу");
      }
    }
    // (б) оба переводчика выходят в хедже: внутренний R не меняется, Σ R внутр. остаётся 0, R+U каждого равен результату по кассе.
    {
      LedgerUseCases uc(LedgerUseCases::InitOptions{});
      Cycle(uc);
      uc.RememberExecutionIntent(MkBuyIntent("T_BTC_binance", "BTC", "binance", 10.0, 80000.0));
      Apply(uc, MkFill("T_BTC_binance", "BTC", "binance", 0.125, 80030.0, "z-rep-1"));
      uc.RememberExecutionIntent(MkSellIntent("T_BTC_kraken", "BTC", "kraken", 10.0, 80080.0));
      Apply(uc, MkFill("T_BTC_kraken", "BTC", "kraken", 0.12487512, 80100.0, "z-rep-2"));
      Near(SumInternalR(uc), 0.0, 1e-6, "Z: после выхода на внешние биржи Σ R внутренней группы по-прежнему 0");
      const auto resp = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
      const auto* t1 = Find(resp, "T_BTC_binance");
      const auto* t2 = Find(resp, "T_BTC_kraken");
      if (t1 == nullptr || t2 == nullptr) { std::cerr << "FAILED: Z: переводчики не найдены\n"; ++g_fail; }
      else {
        // T1: продал 0,125 по 80 000, откупил по 80 030 ⇒ −3,75 по кассе = R внутр. −5 + R внеш. +1,25.
        Near(Num(t1->total().r_plus_u()), -3.75, 1e-6, "Z: T1: R+U = результат по кассе");
        Near(Num(t1->external().r()), 1.25, 1e-6, "Z: T1: R внеш. = откуп дешевле F_0 на 10 за BTC");
        // T2: купил 0,12487512 по 80 080, продал по 80 100 ⇒ +2,4975 = R внутр. −4,995 + R внеш. +7,4925.
        Near(Num(t2->total().r_plus_u()), 2.4975024, 1e-5, "Z: T2: R+U = результат по кассе");
        Near(Num(t2->external().r()), 7.4925072, 1e-5, "Z: T2: R внеш. = (80100 − 80040)·q");
        Near(Num(t2->external().u()), 0.0, 1e-9, "Z: T2: позиция закрыта ⇒ U внеш. = 0");
      }
    }
    // (в) T2 выходит позже, когда F_0 выросла до 80 050: его итог по позиции тот же — риск разновременности целиком внутри R внеш. агента.
    {
      LedgerUseCases uc(LedgerUseCases::InitOptions{});
      Cycle(uc);
      uc.ApplyPositionDelta("z2", 2000, {}, {MkDelta("T_BTC_kraken", "translator", "BTC", "kraken", 0.0008, 80080.0, 80050.0)});
      uc.RememberExecutionIntent(MkSellIntent("T_BTC_kraken", "BTC", "kraken", 10.0, 80080.0));
      Apply(uc, MkFill("T_BTC_kraken", "BTC", "kraken", 0.12487512, 80100.0, "z-rep-2"));
      const auto resp = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
      const auto* t2 = Find(resp, "T_BTC_kraken");
      if (t2 == nullptr) { std::cerr << "FAILED: Z: T2 не найден\n"; ++g_fail; }
      else {
        // (80100 − F_in)·q со стоимостью входа F_in = 80 040 (исполнение 6,24 + дрейф 1,25), не зависит от F_0 в момент выхода.
        Near(Num(t2->external().r()), 7.4925072, 0.01, "Z: T2 вышел при F_0=80 050: R внеш. = (80100−80040)·q");
      }
    }
  }

  // --- 5j) свежесть справедливой цены: протухшая F_0 для связки не используется ----------------------------------------------------------
  {
    setenv("CE_AGENT_PNL_FAIR_MAX_AGE_MS", "30000", 1);
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    // Переводчик BTC прислал F_0 две минуты назад; связка торгует сейчас, а переводчиков актива в такте нет.
    uc.ApplyPositionDelta("st1", now - 120000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 0.0008, 80000.0, 80040.0)});
    uc.ApplyPositionDelta("st2", now, {},
                          {MkDelta("A_BTC_binancekraken", "arbitrageur", "BTC", "binance", -10.0, 80000.0),
                           MkDelta("A_BTC_binancekraken", "arbitrageur", "BTC", "kraken", 10.0, 80080.0)});
    const auto resp = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* l = Find(resp, "A_BTC_binancekraken");
    Check(l != nullptr, "J: связка найдена");
    if (l != nullptr) {
      Near(Num(l->internal().r()), 0.0, 1e-12, "J: F_0 протухла ⇒ выгода сделок связки не считается (F := цена сделки)");
      Check(l->external().has_data() && !l->external().u_has_data(), "J: остаток паритета есть, свежей F_0 нет ⇒ U не определён");
    }
    // Свежая F_0 в том же такте возвращает выгоду.
    uc.ApplyPositionDelta("st3", now + 1000, {},
                          {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 0.0008, 80000.0, 80040.0),
                           MkDelta("A_BTC_binancekraken", "arbitrageur", "BTC", "binance", -10.0, 80000.0),
                           MkDelta("A_BTC_binancekraken", "arbitrageur", "BTC", "kraken", 10.0, 80080.0)});
    const auto resp2 = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* l2 = Find(resp2, "A_BTC_binancekraken");
    if (l2 != nullptr) Near(Num(l2->internal().r()), 9.9950048, 1e-5, "J: F_0 такта свежая ⇒ выгода круга считается");
    setenv("CE_AGENT_PNL_FAIR_MAX_AGE_MS", "0", 1);
  }

  // --- 5k) F_0 общая для пары: метка U переводчика берётся по последней дельте ЛЮБОГО переводчика пары -------------------------------------
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.ApplyPositionDelta("sp1", 1000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 80.0, 80000.0, 80000.0)});  // 1 BTC по F_0=80 000
    // T_BTC_binance больше дельт не получает; переводчик kraken присылает новую F_0 пары.
    uc.ApplyPositionDelta("sp2", 2000, {}, {MkDelta("T_BTC_kraken", "translator", "BTC", "kraken", 0.0008, 80100.0, 80100.0)});
    const auto resp = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* t1 = Find(resp, "T_BTC_binance");
    Check(t1 != nullptr && t1->external().u_has_data(), "SP: данные есть");
    if (t1 != nullptr) Near(Num(t1->external().u()), 100.0, 1e-3, "SP: U = 1·(80100 − 80000) по общей F_0 пары, хотя собственных дельт нет");
  }

  // --- 5l) F_0 связки на кросс-активе: F_0 пары × справедливая USDT-цена валюты котировки этого же такта ------------------------------------
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    // T_BTC (пара к USDT) F_0 = 80 000; T_ETH (ETH/BTC) F_0 = 0,05 BTC ⇒ F_0 ETH = 0,05·80 000 = 4 000 USDT. Связка ETH покупает по 3 990 и продаёт по 4 010.
    uc.ApplyPositionDelta("sx1", 1000, {},
                          {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 0.0008, 80000.0, 80000.0),
                           MkDeltaX("T_ETH_binance", "translator", "ETH", "binance", 0.0008, 4000.0, 0.05, 0.05, "BTC"),
                           MkDelta("A_ETH_binancekraken", "arbitrageur", "ETH", "binance", -4.0, 3990.0),
                           MkDelta("A_ETH_binancekraken", "arbitrageur", "ETH", "kraken", 4.0, 4010.0)});
    const auto resp = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* l = Find(resp, "A_ETH_binancekraken");
    Check(l != nullptr, "XL: связка найдена");
    // Источник: 4000/3990 = 1,00250627 ETH · (4000 − 3990) = 10,0251; назначение: 4000/4010 = 0,99750623 ETH · (4010 − 4000) = 9,9751.
    if (l != nullptr) Near(Num(l->internal().r()), 20.0002, 1e-3, "XL: выгода связки оценена по F_0 = 4 000 USDT, а не по цене узла");
  }

  // --- 5m) хедж переводчика без книги (позиция накоплена, пока учёт был выключен): книга засевается позицией до исполнения по F_0 --------------
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    setenv("CE_AGENT_PNL_ENABLED", "0", 1);
    uc.ApplyPositionDelta("nb1", 1000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 80.0, 80000.0, 80000.0)});  // 1 BTC по F_0=80 000
    unsetenv("CE_AGENT_PNL_ENABLED");
    uc.RememberExecutionIntent(MkSellIntent("T_BTC_binance", "BTC", "binance", 80.0, 80000.0));
    Apply(uc, MkFill("T_BTC_binance", "BTC", "binance", 1.0, 80060.0, "nb-rep"));
    const auto resp = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* a = Find(resp, "T_BTC_binance");
    Check(a != nullptr && a->external().has_data(), "NB: данные есть");
    if (a != nullptr) {
      Near(Num(a->external().r()), 60.0, 1e-6, "NB: засев по F_0 и хедж 1 BTC по 80 060 ⇒ R внешних = (80060 − 80000)·1");
      Near(Num(a->internal().r()), 0.0, 1e-12, "NB: внутренних сделок учёт не видел ⇒ R внутр. = 0");
    }
  }

  // --- 5n) хедж переводчика, когда завести книгу не из чего (после рестарта нет F_0 и цены базы): исполнение в R не попадает, падения нет ------
  {
    auto repo = std::make_shared<FakeRepo>();
    AgentPositionRow row;
    row.agent_id = "T_BTC_binance"; row.agent_kind = "translator"; row.asset = "BTC"; row.venue = "binance";
    row.position = Dec(80.0, 6);
    repo->rows[FakeRepo::Key{"T_BTC_binance", "BTC", "binance"}] = row;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetAgentPositionRepo(repo);
    uc.RememberExecutionIntent(MkSellIntent("T_BTC_binance", "BTC", "binance", 80.0, 80000.0));
    Apply(uc, MkFill("T_BTC_binance", "BTC", "binance", 1.0, 80060.0, "nb2-rep"));
    Check(repo->books.empty(), "NS: книгу заводить не из чего — в репозиторий ничего не записано");
    const auto resp = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* a = Find(resp, "T_BTC_binance");
    Check(a != nullptr && !a->total().has_data(), "NS: валюта котировки неизвестна ⇒ «нет данных», а не нули");
  }

  // --- 5o) хедж кросс-пары ETH/BTC: цена исполнения в BTC, результат в USDT по курсу ρ = P_b / F_0 ---------------------------------------------
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.ApplyPositionDelta("xh1", 1000, {}, {MkDeltaX("T_ETH_binance", "translator", "ETH", "binance", 3.0, 3000.0, 0.05, 0.05, "BTC")});
    uc.ApplyPositionDelta("xh2", 2000, {}, {MkDeltaX("T_ETH_binance", "translator", "ETH", "binance", 3.06, 3060.0, 0.051, 0.0515, "BTC")});
    uc.RememberExecutionIntent(MkSellIntent("T_ETH_binance", "ETH", "binance", 3.06, 3060.0));
    Apply(uc, MkFill("T_ETH_binance", "ETH", "binance", 1.0, 0.0516, "xh-rep", 8));
    const auto resp = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* a = Find(resp, "T_ETH_binance");
    Check(a != nullptr && a->external().u_has_data(), "XH: данные есть");
    if (a != nullptr) {
      // Исполнение (0,0516 − 0,0515)·1 = 0,0001 BTC; дрейф (0,0515 − 0,05075)·1 = 0,00075 BTC; всего 0,00085 BTC · ρ(59 417,48) ≈ 50,51 USDT.
      Near(Num(a->external().r()), 50.5049, 0.01, "XH: R внешних кросс-пары в USDT");
      // Остаток 1 ETH по стоимости входа 0,05075 при F_0 = 0,0515 ⇒ U = 0,00075 BTC ≈ 44,56 USDT.
      Near(Num(a->external().u()), 44.5631, 0.01, "XH: U остатка в USDT");
    }
  }

  // --- 5g) итоги по видам агентов (панель «Живые позиции» не суммирует в браузере) -------------------------
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    RunTranslatorScenario(uc);  // переводчик: R = 103,33, U ≈ 36,67; F_0 актива BTC = 80 050
    // Связка в отдельном такте: своей F_0 нет, берётся последняя известная у переводчиков актива (80 050).
    uc.ApplyPositionDelta("m1", 7000, {},
                          {MkDelta("A_BTC_binancekraken", "arbitrageur", "BTC", "binance", -10.0, 80000.0),
                           MkDelta("A_BTC_binancekraken", "arbitrageur", "BTC", "kraken", 10.0, 80080.0)});
    const auto resp = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const fob::ledger::v1::KindPnlTotals* tr = nullptr;
    const fob::ledger::v1::KindPnlTotals* ar = nullptr;
    for (const auto& k : resp.by_kind()) {
      if (k.agent_kind() == "translator") tr = &k;
      if (k.agent_kind() == "arbitrageur") ar = &k;
    }
    Check(resp.by_kind_size() == 2 && tr != nullptr && ar != nullptr, "K: две записи by_kind — переводчики и арбитражёры");
    if (tr != nullptr && ar != nullptr) {
      Check(tr->agent_count() == 1 && ar->agent_count() == 1, "K: по одному агенту каждого вида");
      Near(Num(tr->total().r()), 103.3333, 1e-3, "K: R переводчиков");
      // Источник: 0,125·(80050−80000) = 6,25; назначение: 0,12487512·(80080−80050) = 3,7463 ⇒ 9,9963.
      Near(Num(ar->total().r()), 9.9962536, 1e-4, "K: R арбитражёров — выгода обеих ног к F_0 пары");
      Near(Num(tr->total().r_plus_u()) + Num(ar->total().r_plus_u()), Num(resp.totals().total().r_plus_u()), 1e-9,
           "K: сумма по видам = строка «все агенты»");
      Near(Num(tr->external().r()) + Num(ar->external().r()), Num(resp.totals().external().r()), 1e-9,
           "K: внешний R: сумма по видам = общий");
      Near(Num(tr->internal().r()) + Num(ar->internal().r()), Num(resp.totals().internal().r()), 1e-9,
           "K: внутренний R: сумма по видам = общий");
      Near(Num(tr->internal().u()) + Num(ar->internal().u()), 0.0, 1e-12, "K: внутренний U ≡ 0");
    }
    // Запрос по одному агенту: итоги и записи по видам считаются по выбранным.
    fob::ledger::v1::GetAgentPnlRequest one;
    one.add_agent_ids("T_BTC_binance");
    const auto only = uc.GetAgentPnl(one);
    Check(only.agents_size() == 1 && only.by_kind_size() == 1 && only.by_kind(0).agent_kind() == "translator",
          "K: фильтр по агенту — в by_kind только его вид");
  }

  if (g_fail != 0) {
    std::cerr << g_fail << " checks failed\n";
    return 1;
  }
  std::cout << "ce_agent_pnl_test: OK\n";
  return 0;
}
