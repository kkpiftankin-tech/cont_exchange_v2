#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cex::venues::app {

// ADR-070 (дополнение «динамический режим»): реестр АРЕНД глубокого опроса стакана.
// Панель «Торговля» периодически присылает набор (venue, symbol), который ей нужен;
// быстрая полоса опрашивает ТОЛЬКО арендованные пары, а когда сигналы прекращаются —
// аренда истекает сама, и опрос выключается без явной команды «стоп». Лимит числа
// одновременно арендованных пар защищает ядро (хеджи, лента сделок) от перегрузки: ранее
// статический режим «4 CEX × 5 пар × 1000 уровней / 1 с» грузил venues на 300%+ CPU.
class DepthWatchRegistry {
 public:
  using Clock = std::chrono::steady_clock;
  using ClockFn = std::function<Clock::time_point()>;
  using Item = std::pair<std::string, std::string>;  // (venue_id, symbol "BASE/QUOTE")

  struct Config {
    std::size_t max_items = 8;       // потолок одновременно арендованных пар
    int64_t min_ttl_ms = 5000;       // клэмп срока аренды снизу
    int64_t max_ttl_ms = 60000;      // и сверху: «забытая» вкладка не держит опрос долго
  };

  struct TouchResult {
    std::size_t accepted = 0;        // принято: новые + продлённые
    std::size_t rejected = 0;        // отказано: новых больше, чем свободных мест (потолок)
    std::size_t active = 0;          // активных аренд после операции
    int64_t ttl_ms = 0;              // фактический срок (после клэмпа)
  };

  struct ActiveItem {
    std::string venue;
    std::string symbol;
    int64_t remaining_ms = 0;
  };

  // Перегрузки вместо `Config cfg = Config{}`: GCC не принимает NSDMI вложенной структуры
  // в аргументе по умолчанию внутри того же класса (как и с SnapshotProducer::PublishOptions).
  DepthWatchRegistry();
  explicit DepthWatchRegistry(Config cfg);
  DepthWatchRegistry(Config cfg, ClockFn clock);

  // Применить конфиг после конструирования (владелец-член класса настраивается из env).
  void SetConfig(Config cfg);

  // Создать/продлить аренды. Существующую аренду продлеваем всегда (место уже занято);
  // новую — только пока active < max_items. Дубликаты во входе схлопываются.
  TouchResult Touch(const std::vector<Item>& items, int64_t ttl_ms);

  // Активные (не истёкшие) символы биржи, отсортированы.
  std::vector<std::string> SymbolsFor(const std::string& venue);

  std::vector<ActiveItem> Active();
  // Читать после конструирования/SetConfig (SetConfig вызывается до старта потоков).
  std::size_t max_items() const { return cfg_.max_items; }

 private:
  void PurgeLocked(Clock::time_point now);

  Config cfg_;
  ClockFn clock_;
  std::mutex mu_;
  std::unordered_map<std::string, Clock::time_point> expiry_;  // "venue|symbol" → до когда
};

}  // namespace cex::venues::app
