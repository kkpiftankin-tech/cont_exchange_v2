#pragma once
// =============================================================================
// Env — лёгкий хелпер для чтения переменных окружения.
//
// Все микросервисы конфигурируются через переменные окружения (12-factor app):
// в Docker и Kubernetes так удобнее всего — никаких файлов, всё в манифесте.
// Этот класс убирает дублирование getenv()+проверка на null+парсинг.
// =============================================================================

#include <cstdlib>
#include <optional>
#include <string>

namespace cex::common {

// Утилитный namespace-класс с статическими методами (как набор свободных функций
// в одном "пространстве имён", чтобы вызовы выглядели как Env::get_int(...)).
struct Env {
  // Возвращает значение переменной key или def, если переменная не задана.
  static std::string get_string(const std::string& key, const std::string& def = "");
  // Парсит значение как int; при отсутствии или ошибке парсинга — возвращает def.
  static int get_int(const std::string& key, int def);
  // Понимает как true: "1","true","yes","y" (без учёта регистра); как false:
  // "0","false","no","n". Иначе — возвращает def.
  static bool get_bool(const std::string& key, bool def);
  // "Сырой" вариант: nullopt если переменная не задана. Полезно, когда отсутствие
  // переменной должно отличаться от пустой строки.
  static std::optional<std::string> try_get_string(const std::string& key);
};

}  // namespace cex::common
