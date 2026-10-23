#pragma once
// ADR-082 (шаг 0): строка измерения ликвидности биржи по паре — оборот ленты и коэффициент исполнения. Порт публикации без
// зависимостей от БД: PG-реализация живёт в infra (postgres_liquidity_stats_repository).
#include <cstdint>
#include <string>

namespace cex::venues::app {

struct LiquidityStatRow {
  std::string venue;
  std::string symbol;            // канонический ключ ленты (BTCUSDT, ETHBTC, ...)
  double tape_quote_per_s{0.0};  // оборот ленты, валюты котирования в секунду (скользящее среднее)
  int64_t tape_samples{0};       // сколько чтений ленты вошло в оценку (0 — оценки ещё нет)
  int64_t tape_reads{0};         // всего чтений ленты (включая пропущенные выборки); в таблицу не пишется, нужен для обнаружения изменений
  double fill_coef{1.0};         // коэффициент исполнения f ∈ [0, 1]; нет заявок ⇒ 1
  double fill_weight{0.0};       // суммарный (с забыванием) запрошенный объём заявок — вес оценки f, в базовой валюте
  int64_t fill_orders{0};        // сколько заявок вошло в f
  double fill_orders_eff{0.0};   // число заявок в f с забыванием на момент снимка (вес доверия к f)
  double age_s{0.0};             // только при чтении таблицы для затравки: сколько секунд назад строка записана
};

}  // namespace cex::venues::app
