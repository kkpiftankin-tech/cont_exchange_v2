// ============================================================================
// decimal_conversion.cpp — мост между текстовым представлением PostgreSQL
// NUMERIC(P, S) и доменным типом cex::common::Decimal (fixed-point с int64
// units и целочисленным scale).
//
// Зачем этот файл существует:
//   PostgreSQL libpqxx возвращает значения колонок NUMERIC как std::string
//   в десятичной нотации ("60000.000000000000000000", "-0.0015", "0").
//   Доменный код проекта работает с cex::common::Decimal{units:int64,
//   scale:int32}, где value = units * 10^(-scale). Соответственно нужны
//   две функции — ParsePgNumeric() (текст → Decimal) и ToPgNumeric()
//   (Decimal → текст для exec_params).
//
// Финансовая дисциплина проекта (см. CLAUDE.md §9):
//   Никаких double/float для money. Decimal — единственно допустимый тип
//   для денег в ledger / risk / matching settlement. Эта функция — критическая
//   точка перехода: ошибка здесь = молчаливая порча балансов.
//
// История ключевого бага (PR-F02-002, 2026-06-02):
//   До фикса parser падал на самом обычном целом числе 60000, потому что
//   PostgreSQL возвращает его как "60000.000000000000000000" (NUMERIC(38,18)
//   автоматически добивает scale до объявленного), и наивный алгоритм пытался
//   построить units = 60000 * 10^18 ≈ 6e22, что переполняет int64 (макс 9.2e18).
//   Лечение — стрипать trailing zeros (и последнюю точку, если все дробные
//   нули) ДО парсинга, чтобы scale считал только значащие цифры.
//
//   Этот баг похоронил весь F-02 → F-04 конвейер в PG-режиме на ~30 минут,
//   пока я не добавил guard в ParsePgNumeric ниже.
// ============================================================================

#include "infra/postgres/decimal_conversion.hpp"

#include <cctype>      // std::isspace, std::isdigit — char classification из C runtime
#include <cstdint>     // std::int64_t / std::int32_t / std::size_t — гарантированно-широкие типы
#include <limits>      // std::numeric_limits<T>::max() / ::min() для границ int64
#include <stdexcept>   // std::invalid_argument, std::overflow_error — типизированные ошибки

namespace cex::matching::infra::postgres {

namespace {  // anonymous namespace — symbols здесь имеют internal linkage,
             // не попадают в глобальную таблицу символов, не пересекаются
             // с одноимёнными хелперами в других .cpp.

// ----------------------------------------------------------------------------
// trim_ascii_space — снимает с обоих концов строки whitespace (пробел, tab,
// \r, \n, vertical tab, form feed).
//
// Принимает и возвращает std::string_view — это lightweight non-owning view:
// pointer + size, копирование O(1), без аллокации. Идеально для парсера, не
// меняющего исходную строку.
//
// std::isspace требует unsigned char: на некоторых платформах char может быть
// signed, и передача байта >0x7F даёт UB (negative int index в таблице
// classification). static_cast<unsigned char> — каноническая защита.
// ----------------------------------------------------------------------------
std::string_view trim_ascii_space(std::string_view text) {
  // remove_prefix/remove_suffix двигают границы view без аллокации.
  // Цикл — пока непустое и первый/последний символ — пробельный.
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    text.remove_prefix(1);  // сдвигаем begin вправо на 1 символ
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.remove_suffix(1);  // сдвигаем end влево на 1 символ
  }
  return text;  // copy elision — компилятор не копирует, просто возвращает view
}

}  // namespace

// ============================================================================
// ParsePgNumeric — преобразует строку из PostgreSQL NUMERIC в Decimal.
//
// Семантика: text "X.Y" → Decimal{ units = (signed int)("XY"),
//                                  scale = len(Y) после strip trailing zeros }
//
// Примеры:
//   "60000"                          → {units=60000,         scale=0}
//   "60000.000000000000000000"       → {units=60000,         scale=0}   (PR-F02-002 fix)
//   "-0.000123"                      → {units=-123,          scale=6}
//   "0.5"                            → {units=5,             scale=1}
//   "-9223372036854775808"           → {units=int64_min,     scale=0}  (граничный случай)
//
// Выбрасывает:
//   std::invalid_argument — пустая строка / sign без цифр / две точки / не-цифра
//   std::overflow_error    — после strip zeros всё ещё не помещается в int64
//
// Контекст вызова:
//   Используется в PostgresFlowOrderRepository::map_flow_order (F-04 reader),
//   PostgresHedgeFlowRepository, и других repo-функциях, читающих NUMERIC.
// ============================================================================
cex::common::Decimal ParsePgNumeric(std::string_view text_value) {
  text_value = trim_ascii_space(text_value);    // снимаем возможные пробелы по краям
  if (text_value.empty()) {
    throw std::invalid_argument("NUMERIC text value is empty");  // пустая строка — не число
  }

  // ----- PR-F02-002 GUARD ---------------------------------------------------
  // PG возвращает значения NUMERIC(38, 18) padded до объявленного scale,
  // т.е. целое 60000 приходит как "60000.000000000000000000" — 18 нулей после
  // точки. Наивный алгоритм собрал бы units = 60000 * 10^18 ≈ 6e22 и взорвал
  // int64. Стрипаем trailing zeros так, чтобы scale считал только значащие
  // дробные цифры: "60000.000…0" → "60000", "0.000123000" → "0.000123".
  //
  // Только если в строке есть точка — иначе портим целые числа (например
  // "1000" не должно стать "1").
  if (text_value.find('.') != std::string_view::npos) {
    // text_value.size() > 1 — защита от съедания последнего значащего символа
    // (хотя для "0" эта ветка не активируется, .find('.') вернёт npos).
    while (text_value.size() > 1 && text_value.back() == '0') {
      text_value.remove_suffix(1);   // отрезали один trailing '0'
    }
    // Если после стрипа осталась только точка ("60000." — все нули съели),
    // снимаем и её. После этого scale считаться не будет (has_dot не выставится).
    if (!text_value.empty() && text_value.back() == '.') {
      text_value.remove_suffix(1);
    }
  }
  // --------------------------------------------------------------------------

  // Парсим знак: '+' оставляет negative=false, '-' — true. Любой другой
  // символ означает что цифры начинаются сразу с idx=0.
  bool negative = false;
  std::size_t idx = 0;
  if (text_value.front() == '+' || text_value.front() == '-') {
    negative = text_value.front() == '-';   // 'minus' → true
    idx = 1;                                 // пропускаем знак
  }

  // Защита от "+" / "-" без цифр — это не валидное число.
  if (idx == text_value.size()) {
    throw std::invalid_argument("NUMERIC text value has sign without digits");
  }

  bool has_digit = false;           // нашли ли хоть одну цифру (для финального sanity-check)
  bool has_dot = false;             // встретили точку (после неё цифры идут в дробную часть)
  std::int32_t scale = 0;           // сколько цифр после точки накопилось
  __int128 acc = 0;                 // буфер для накопления модуля числа — 128 бит позволяет
                                    // безопасно сравнить с границами int64 БЕЗ переполнения
                                    // самого acc (если, конечно, входная строка ≤ 38 цифр).
                                    // __int128 — GCC/Clang extension, не стандарт C++,
                                    // но доступен на всех target платформах проекта.

  // Константы для overflow detection. constexpr — вычисляются на этапе компиляции,
  // встраиваются inline. static_cast явно расширяет int64_t до __int128.
  constexpr __int128 kInt64Max    = static_cast<__int128>(std::numeric_limits<std::int64_t>::max());
  // Модуль int64_min на единицу больше int64_max (-9223372036854775808 vs +9223372036854775807).
  // Используем для проверки границы при negative числах.
  constexpr __int128 kInt64MinAbs = kInt64Max + 1;

  // Линейный проход по символам с idx по конец строки.
  for (; idx < text_value.size(); ++idx) {
    const char ch = text_value[idx];

    // Случай 1: точка. Только одна допустима. После неё каждая цифра
    // увеличивает scale (мы строим число "целая часть * 10^scale + дробная").
    if (ch == '.') {
      if (has_dot) {
        // "1.2.3" — синтаксическая ошибка.
        throw std::invalid_argument("NUMERIC text value contains multiple dots");
      }
      has_dot = true;
      continue;   // переход к следующей итерации, не цифру не обрабатываем
    }

    // Случай 2: не-цифра, не точка → синтаксическая ошибка.
    // isdigit, как и isspace, требует unsigned char для UB-safe вызова.
    if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
      throw std::invalid_argument("NUMERIC text value contains non-digit characters");
    }

    // Случай 3: цифра. Аккумулируем как обычный Horner-приём: acc = acc * 10 + digit.
    // 'ch - '0'' — стандартный трюк: коды цифр ASCII идут подряд, поэтому
    // разность даёт numeric value 0..9. static_cast<int> убирает narrowing-warning.
    has_digit = true;
    acc = acc * 10 + static_cast<int>(ch - '0');

    // Граничный overflow-check. Делаем ПОСЛЕ умножения и добавления, потому что
    // __int128 безопасно вмещает результат для ≤38 десятичных цифр, а нам важен
    // момент пересечения границы int64. Разные границы для positive (≤int64_max)
    // и negative (модуль ≤ int64_max+1 = |int64_min|).
    if ((!negative && acc > kInt64Max) || (negative && acc > kInt64MinAbs)) {
      throw std::overflow_error("NUMERIC value does not fit into Decimal::units (int64)");
    }

    // Если после точки — увеличиваем scale (число дробных цифр).
    // Если до точки — scale остаётся 0.
    if (has_dot) {
      ++scale;
    }
  }

  // Защита от строки "+" / "-" / "." без цифр (PR-F02-002 strip мог это создать
  // из ".0" например — но has_digit бы остался false).
  if (!has_digit) {
    throw std::invalid_argument("NUMERIC text value does not contain digits");
  }

  // Финальное преобразование __int128 acc → int64 units с учётом знака.
  // Особый случай: -9223372036854775808 (int64_min) НЕ выражается как
  // -(int64_max+1) = -кin64_max - 1 без UB, потому что -(int64_max+1) сначала
  // вычислит +(int64_max+1) (промежуточный overflow). Решение — собрать как
  // -(int64_max+1) через прямое присваивание int64_min.
  std::int64_t units = 0;
  if (negative) {
    if (acc == kInt64MinAbs) {
      units = std::numeric_limits<std::int64_t>::min();   // -9223372036854775808
    } else {
      units = -static_cast<std::int64_t>(acc);             // safe: acc ≤ int64_max
    }
  } else {
    units = static_cast<std::int64_t>(acc);                // safe: acc ≤ int64_max
  }

  // Aggregate-initialization Decimal — поля units и scale заполняются по порядку.
  // Никаких аллокаций, никакой динамической памяти.
  return cex::common::Decimal{units, scale};
}

// ============================================================================
// ToPgNumeric — обратное преобразование Decimal → строка для exec_params.
//
// Использует Decimal::to_string(), который генерирует канонический формат
// "[-]X.Y" без trailing zeros. libpqxx сам экранирует строку при подстановке
// в $N параметр, поэтому возвращаемое значение можно подавать прямо в
// tx.exec_params(...) без дополнительной обработки.
//
// Тривиальная обёртка — но даёт типобезопасность (нельзя случайно передать
// double вместо Decimal в SQL-запрос).
// ============================================================================
std::string ToPgNumeric(const cex::common::Decimal& value) {
  return value.to_string();   // делегируем доменному типу
}

}  // namespace cex::matching::infra::postgres
