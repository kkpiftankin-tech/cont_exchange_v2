// =============================================================================
// Реализация хелпера для чтения переменных окружения (см. env.hpp).
// Никаких сюрпризов: тонкая обёртка над std::getenv с парсингом значений.
// =============================================================================

#include "cex/common/env.hpp"

#include <algorithm>
#include <cctype>

namespace cex::common {

// Базовый чтение переменной — возвращает nullopt, если переменная не задана
// (т.е. getenv вернул nullptr). Это позволяет вызывающему коду различать
// "переменной нет" и "переменная задана как пустая строка".
std::optional<std::string> Env::try_get_string(const std::string& key) {
  const char* v = std::getenv(key.c_str());
  if (!v) return std::nullopt;
  return std::string(v);
}

// Удобная обёртка с дефолтом — самый частый случай.
std::string Env::get_string(const std::string& key, const std::string& def) {
  auto v = try_get_string(key);
  return v ? *v : def;
}

// Парсинг int. Если переменной нет ИЛИ значение не парсится — возвращаем def
// (поглощаем std::invalid_argument / std::out_of_range, чтобы сервис не падал
// из-за опечатки в env-файле).
int Env::get_int(const std::string& key, int def) {
  auto v = try_get_string(key);
  if (!v) return def;
  try { return std::stoi(*v); } catch (...) { return def; }
}

// Bool. Принимаем популярные написания, без учёта регистра.
// "ON"/"OFF" сейчас не поддерживаются — при необходимости легко добавить.
bool Env::get_bool(const std::string& key, bool def) {
  auto v = try_get_string(key);
  if (!v) return def;
  std::string s = *v;
  // Лямбда нужна, чтобы tolower видел unsigned char (иначе UB на отрицательных).
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
  if (s=="1" || s=="true" || s=="yes" || s=="y") return true;
  if (s=="0" || s=="false" || s=="no" || s=="n") return false;
  return def;
}

} // namespace cex::common
