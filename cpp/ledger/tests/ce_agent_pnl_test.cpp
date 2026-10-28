// ============================================================================
// ce_agent_pnl_test.cpp — ADR-087 / CALC-CE-AGENT-PNL. Заработок агентов CE по биржам через ledger целиком:
//   1) переводчик: покупки на CE → хедж → GetAgentPnl (числа §3.1 карточки);
//   2) связка: круговая сделка за такт (§3.3), внешняя группа — нули с признаком «есть данные»;
//   3) сброс позиций обнуляет всё; выключатель CE_AGENT_PNL_ENABLED=0;
//   4) перезапуск: книги и внешние суммы восстанавливаются из репозитория (заглушка вместо PostgreSQL);
//   5) повторная доставка такта не удваивает книгу; засев книги по существующей позиции; хедж связки; кросс-пара (ρ≠1);
//      регресс при CE_AGENT_PNL_ENABLED=0; сбой загрузки книг не затирает сохранённые и после восстановления учёт продолжается.
// Pure unit: без PG и Kafka. CE_AGENT_BAND не задан ⇒ автоматической эмиссии хеджа нет, исполнение регистрируется
// через RememberExecutionIntent, как в ce_band_house_realized_test.
// ============================================================================
#include <algorithm>
#include <cassert>
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
using cex::ledger::app::ExternalFillDelta;
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

// Заглушка репозитория (вместо PostgreSQL): накапливает позиции, внешние суммы и книги в памяти — как это делает PG.
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
  void ApplyExternalFill(const std::string& agent_id, const std::string& asset, const std::string& venue,
                         const Decimal& dif, const Decimal& drealized, const ExternalFillDelta& ext, int64_t) override {
    auto& r = rows[Key{agent_id, asset, venue}];
    r.in_flight = Decimal::add(r.in_flight, dif);
    r.realized_vs_fair = Decimal::add(r.realized_vs_fair, drealized);
    r.ext_buy_base = Decimal::add(r.ext_buy_base, ext.buy_base);
    r.ext_sell_base = Decimal::add(r.ext_sell_base, ext.sell_base);
    r.ext_buy_notional = Decimal::add(r.ext_buy_notional, ext.buy_notional);
    r.ext_sell_notional = Decimal::add(r.ext_sell_notional, ext.sell_notional);
    r.ext_fair_signed = Decimal::add(r.ext_fair_signed, ext.fair_signed);
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
    for (auto& kv : rows) {
      if (!ids.empty() && std::find(ids.begin(), ids.end(), std::get<0>(kv.first)) == ids.end()) continue;
      kv.second.ext_buy_base = kv.second.ext_sell_base = kv.second.ext_buy_notional = kv.second.ext_sell_notional =
          kv.second.ext_fair_signed = kv.second.rvf_base = Decimal::zero();
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

fob::execution::v1::ExecutionReport MkFill(const std::string& agent, const std::string& asset, const std::string& venue,
                                           double filled, double avg_price, const std::string& report_id) {
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
  *r.mutable_average_price() = Dec(avg_price, 2).to_proto();
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

// Переводчик из §3.1: +2 BTC @ 80 000, +1 BTC @ 80 030 (F_0 = 80 040), хедж 2 BTC @ 80 060, затем цена оценки → 80 050.
void RunTranslatorScenario(LedgerUseCases& uc, double tick3_kusd = 0.0008, double tick3_price = 80050.0) {
  uc.ApplyPositionDelta("b1", 1000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 160.0, 80000.0, 80000.0)});
  uc.ApplyPositionDelta("b2", 2000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 80.03, 80030.0, 80040.0)});
  uc.RememberExecutionIntent(MkSellIntent("T_BTC_binance", "BTC", "binance", 160.06, 80030.0));
  Apply(uc, MkFill("T_BTC_binance", "BTC", "binance", 2.0, 80060.0, "rep-1"));
  // Цена оценки обновилась до 80 050: малая сделка (0,8 USDT) по новой цене, F_0 = 80 050.
  uc.ApplyPositionDelta("b3", 3000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", tick3_kusd, tick3_price, 80050.0)});
}

void CheckTranslator(const fob::ledger::v1::GetAgentPnlResponse& r, const char* tag) {
  const auto* a = Find(r, "T_BTC_binance");
  if (a == nullptr) { std::cerr << "FAILED: " << tag << ": агент не найден\n"; ++g_fail; return; }
  Check(a->internal().has_data() && a->external().has_data() && a->total().has_data(), tag);
  Near(Num(a->internal().r()), 60.0, 1e-4, "T: R внутренней биржи = 2·(80040−80010)");
  Near(Num(a->internal().u()), 40.0, 0.01, "T: U внутренней биржи = остаток по цене оценки 80 050");
  Near(Num(a->external().r()), 40.0, 1e-4, "T: R внешних = 2·(80060−80040)");
  Near(Num(a->external().u()), 0.0, 1e-9, "T: U внешних = 0 (хедж сопоставлен с книгой)");
  Near(Num(a->total().r()), 100.0, 1e-4, "T: R итого");
  Near(Num(a->total().r_plus_u()), 140.0, 0.01, "T: R+U итого = реальный результат (140)");
  Near(Num(a->total().r_plus_u()), Num(a->internal().r_plus_u()) + Num(a->external().r_plus_u()), 1e-9,
       "T: итого = внутренняя + внешняя");
  Check(a->ext_fills() == 1, "T: одно внешнее исполнение");
  Check(a->since_ms() > 0, "T: начало накопления задано");
  Check(a->mark_age_ms() >= 0, "T: возраст цены известен");
  Check(a->venues_size() == 1 && a->venues(0) == "binance", "T: площадка binance");
  // Строка «все агенты» совпадает с единственным агентом.
  Near(Num(r.totals().total().r_plus_u()), Num(a->total().r_plus_u()), 1e-9, "T: totals = сумма по агентам");
  Check(r.totals().agent_count() == 1 && r.totals().incomplete_agents() == 0, "T: totals — счётчики");
}

}  // namespace

int main() {
  setenv("CE_NUMERAIRE", "USDT", 1);
  unsetenv("CE_AGENT_BAND");
  unsetenv("CE_AGENT_PNL_ENABLED");

  // --- 1) переводчик без репозитория ---------------------------------------------------------
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    RunTranslatorScenario(uc);
    CheckTranslator(uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{}), "T: данные есть");
  }

  // --- 2) связка: круговая сделка за такт ---------------------------------------------------
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    // f = 10 тыс. USDT: источник (binance) Δ=−10 @ 80 000, назначение (kraken) Δ=+10 @ 80 080, один такт.
    uc.ApplyPositionDelta("l1", 1000, {},
                          {MkDelta("A_BTC_binancekraken", "arbitrageur", "BTC", "binance", -10.0, 80000.0),
                           MkDelta("A_BTC_binancekraken", "arbitrageur", "BTC", "kraken", 10.0, 80080.0)});
    const auto r = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* a = Find(r, "A_BTC_binancekraken");
    if (a == nullptr) { std::cerr << "FAILED: связка не найдена\n"; ++g_fail; }
    else {
      Near(Num(a->internal().r()), 9.9900096, 1e-5, "L: R внутренней биржи = базис круговой сделки");
      Near(Num(a->internal().u()), 0.0049952, 1e-5, "L: U = остаток паритета по среднему 80 040");
      Check(a->external().has_data(), "L: внешняя группа — данные есть");
      Near(Num(a->external().r_plus_u()), 0.0, 1e-12, "L: внешних исполнений нет ⇒ нули");
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
      Near(Num(a->internal().r()), 0.0, 1e-12, "R: внутренний R обнулён");
      Near(Num(a->internal().u()), 0.0, 1e-12, "R: внутренний U обнулён");
      Near(Num(a->external().r()), 0.0, 1e-12, "R: внешний R обнулён");
      Check(a->ext_fills() == 0 && a->since_ms() == 0, "R: счётчик и начало накопления сброшены");
    }
    setenv("CE_AGENT_PNL_ENABLED", "0", 1);
    const auto off = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    Check(!off.enabled() && off.agents_size() == 0, "R: выключатель ⇒ пустой список");
    unsetenv("CE_AGENT_PNL_ENABLED");
  }

  // --- 4) перезапуск: состояние из репозитория --------------------------------------------------
  {
    auto repo = std::make_shared<FakeRepo>();
    {
      LedgerUseCases uc(LedgerUseCases::InitOptions{});
      uc.SetAgentPositionRepo(repo);
      RunTranslatorScenario(uc);
      CheckTranslator(uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{}), "P: до перезапуска");
      Check(repo->books.count("T_BTC_binance") == 1, "P: книга записана в репозиторий");
    }
    LedgerUseCases uc2(LedgerUseCases::InitOptions{});
    uc2.SetAgentPositionRepo(repo);  // LoadAgentPositionsFromRepo: позиции, внешние суммы и книги
    const auto r = uc2.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* a = Find(r, "T_BTC_binance");
    Check(a != nullptr, "P: агент восстановлен");
    if (a != nullptr) {
      // Цена оценки после перезапуска ещё не пришла (last_price хранится в памяти): группы «нет данных», а не ноль.
      Check(!a->internal().has_data() && !a->external().has_data() && !a->total().has_data(),
            "P: после перезапуска без цены — признак «нет данных»");
    }
    // Первая же дельта с ценой возвращает числа; прежний R_int и R_ext сохранились.
    uc2.ApplyPositionDelta("b4", 4000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 0.0008, 80050.0, 80050.0)});
    const auto r2 = uc2.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* b = Find(r2, "T_BTC_binance");
    if (b == nullptr || !b->total().has_data()) { std::cerr << "FAILED: P: после дельты данные должны появиться\n"; ++g_fail; }
    else {
      Near(Num(b->internal().r()), 60.0, 1e-4, "P: R внутренней биржи сохранён через перезапуск");
      Near(Num(b->external().r()), 40.0, 1e-4, "P: R внешних сохранён через перезапуск");
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
      RunTranslatorScenario(uc, 8.0, 80100.0);  // третий такт "b3": ≈0,1 BTC по 80 100 при оценке 80 050 — двойной учёт был бы заметен в U
      const auto resp_a = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* a = Find(resp_a, "T_BTC_binance");
      if (a == nullptr || !a->total().has_data()) { std::cerr << "FAILED: D: нет данных до перезапуска\n"; ++g_fail; }
      else { r_ref = Num(a->internal().r()); u_ref = Num(a->internal().u()); }
    }
    LedgerUseCases uc2(LedgerUseCases::InitOptions{});
    uc2.SetAgentPositionRepo(repo);
    // Kafka доставляет тот же такт "b3" ещё раз: guard позиции (последний batch_id) и guard книги должны его отсечь.
    uc2.ApplyPositionDelta("b3", 3000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 8.0, 80100.0, 80050.0)});
    const auto resp_a2 = uc2.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* a2 = Find(resp_a2, "T_BTC_binance");
    if (a2 == nullptr || !a2->total().has_data()) { std::cerr << "FAILED: D: нет данных после повторной доставки\n"; ++g_fail; }
    else {
      Near(Num(a2->internal().r()), r_ref, 1e-9, "D: R внутренней биржи не изменился");
      Near(Num(a2->internal().u()), u_ref, 1e-6, "D: повторный такт не удвоил остаток книги");
    }
    Check(u_ref > 34.0 && u_ref < 36.0, "D: контроль — U = 40 − 0,0999·50 ≈ 35,0; удвоение третьего такта дало бы ≈30,0");
  }

  // --- 5b) засев книги по существующей позиции -------------------------------------------------
  {
    auto repo = std::make_shared<FakeRepo>();
    AgentPositionRow row;
    row.agent_id = "T_BTC_binance"; row.agent_kind = "translator"; row.asset = "BTC"; row.venue = "binance";
    row.position = Dec(80.03, 6);  // позиция 1 BTC по 80 030, книги ещё нет
    repo->rows[FakeRepo::Key{"T_BTC_binance", "BTC", "binance"}] = row;
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.SetAgentPositionRepo(repo);
    uc.ApplyPositionDelta("s1", 1000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 0.0008, 80030.0, 80040.0)});
    const auto resp_a = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* a = Find(resp_a, "T_BTC_binance");
    Check(a != nullptr && a->internal().has_data(), "S: данные есть");
    if (a != nullptr) Near(Num(a->internal().u()), 0.0, 0.01, "S: сразу после засева U внутренней биржи ≈ 0");
    // Хедж 1 BTC сопоставляется с засеянным остатком; цена оценки потом уходит на 80 050.
    uc.RememberExecutionIntent(MkSellIntent("T_BTC_binance", "BTC", "binance", 80.03, 80030.0));
    Apply(uc, MkFill("T_BTC_binance", "BTC", "binance", 1.0, 80060.0, "seed-rep"));
    uc.ApplyPositionDelta("s2", 2000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 0.0008, 80050.0, 80050.0)});
    const auto resp_b = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* b = Find(resp_b, "T_BTC_binance");
    if (b == nullptr) { std::cerr << "FAILED: S: агент не найден\n"; ++g_fail; }
    else Near(Num(b->external().u()), 0.0, 1e-6, "S: хедж сопоставлен с засеянным остатком ⇒ внешний U = 0 (без засева был бы −10)");
  }

  // --- 5c) хедж связки через ledger: внутренняя книга не трогается, внешняя корзина считается -------
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.ApplyPositionDelta("l1", 1000, {},
                          {MkDelta("A_BTC_binancekraken", "arbitrageur", "BTC", "binance", -10.0, 80000.0),
                           MkDelta("A_BTC_binancekraken", "arbitrageur", "BTC", "kraken", 10.0, 80080.0)});
    uc.RememberExecutionIntent(MkSellIntent("A_BTC_binancekraken", "BTC", "kraken", 40.045, 80090.0));
    Apply(uc, MkFill("A_BTC_binancekraken", "BTC", "kraken", 0.5, 80090.0, "link-rep"));
    const auto resp_a = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* a = Find(resp_a, "A_BTC_binancekraken");
    if (a == nullptr) { std::cerr << "FAILED: L2: связка не найдена\n"; ++g_fail; }
    else {
      Near(Num(a->internal().r()), 9.9900096, 1e-5, "L2: внутренняя книга связки не тронута хеджем");
      Near(Num(a->external().r()), 0.0, 1e-9, "L2: только продажи ⇒ круговой оборот 0");
      // Корзина ноги kraken: (0 − 0,5)·80 080 + 0,5·80 090 = +5.
      Near(Num(a->external().u()), 5.0, 1e-6, "L2: внешний U = корзина по ценам ноги");
      Check(a->ext_fills() == 1, "L2: одно внешнее исполнение");
    }
  }

  // --- 5d) кросс-пара ETH/BTC: курс валюты котировки ρ = P_b / p ≠ 1 ------------------------------
  {
    LedgerUseCases uc(LedgerUseCases::InitOptions{});
    uc.ApplyPositionDelta("x1", 1000, {}, {MkDeltaX("T_ETH_binance", "translator", "ETH", "binance", 3.0, 3000.0, 0.05, 0.05, "BTC")});
    uc.ApplyPositionDelta("x2", 2000, {}, {MkDeltaX("T_ETH_binance", "translator", "ETH", "binance", 3.06, 3060.0, 0.051, 0.0515, "BTC")});
    const auto resp_a = uc.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* a = Find(resp_a, "T_ETH_binance");
    Check(a != nullptr && a->internal().has_data(), "X: данные есть");
    if (a != nullptr) {
      // L = 2 ETH, p̄ = 0,0505 BTC, оценка 0,0515 BTC ⇒ U = 0,002 BTC; ρ = 3060/0,051 = 60 000 ⇒ 120 USDT.
      Near(Num(a->internal().u()), 120.0, 0.01, "X: U в USDT через курс BTC");
      Near(Num(a->internal().r()), 0.0, 1e-9, "X: R внутренней биржи = 0 (позиция только росла)");
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
    const Decimal realized_saved = repo->books["T_BTC_binance"].realized;
    repo->fail_books = true;
    setenv("CE_AGENT_PNL_RETRY_MS", "0", 1);
    LedgerUseCases uc2(LedgerUseCases::InitOptions{});
    uc2.SetAgentPositionRepo(repo);
    const auto resp_a0 = uc2.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* a0 = Find(resp_a0, "T_BTC_binance");
    Check(a0 != nullptr && !a0->internal().has_data() && !a0->external().has_data() && !a0->total().has_data(),
          "F: книги не загружены ⇒ «нет данных», не нули");
    uc2.ApplyPositionDelta("f1", 5000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 8.0, 80050.0, 80050.0)});
    Check(repo->books["T_BTC_binance"].qty.units == qty_saved.units && repo->books["T_BTC_binance"].qty.scale == qty_saved.scale &&
              Decimal::cmp(repo->books["T_BTC_binance"].realized, realized_saved) == 0,
          "F: пока книги не загружены, сохранённая книга не перезаписывается");
    repo->fail_books = false;
    uc2.ApplyPositionDelta("f2", 6000, {}, {MkDelta("T_BTC_binance", "translator", "BTC", "binance", 8.0, 80050.0, 80050.0)});
    const auto resp_a1 = uc2.GetAgentPnl(fob::ledger::v1::GetAgentPnlRequest{});
    const auto* a1 = Find(resp_a1, "T_BTC_binance");
    Check(a1 != nullptr && a1->total().has_data(), "F: после восстановления учёт возобновлён");
    if (a1 != nullptr && a1->total().has_data()) {
      Near(Num(a1->internal().r()), 60.0, 1e-4, "F: накопленный R внутренней биржи сохранён");
      Near(Num(a1->external().r()), 40.0, 1e-4, "F: накопленный R внешних сохранён");
    }
    unsetenv("CE_AGENT_PNL_RETRY_MS");
  }

  if (g_fail != 0) {
    std::cerr << g_fail << " checks failed\n";
    return 1;
  }
  std::cout << "ce_agent_pnl_test: OK\n";
  return 0;
}
