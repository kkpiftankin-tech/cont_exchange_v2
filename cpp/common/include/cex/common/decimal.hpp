#pragma once
// =============================================================================
// Decimal — арифметика над числами с фиксированной запятой.
//
// На бирже категорически нельзя использовать double/float для денежных сумм:
// ошибки округления приводят к рассинхрону баланса и расхождению ledger'а
// с реальностью. Мы храним значение в виде пары (units, scale), где
// настоящее число равно units * 10^(-scale).
//
// Например: цена 1234.56 -> units = 123456, scale = 2.
// Такое же представление используется в proto-сообщении fob.common.v1.Decimal,
// поэтому маппинг туда-обратно тривиален.
// =============================================================================

#include <cstdint>
#include <string>
#include "fob/common/v1/common.pb.h"

namespace cex::common {

// POD-структура, описывающая число с фиксированной точкой.
struct Decimal {
  int64_t units{0};   // мантисса (целое значение в минимальных единицах)
  int32_t scale{0};   // степень десятки в знаменателе

  // Конвертация из proto-сообщения (используется при чтении из Kafka/gRPC).
  static Decimal from_proto(const fob::common::v1::Decimal& d);
  // Конвертация в proto-сообщение для отправки по сети.
  fob::common::v1::Decimal to_proto() const;

  // Человекочитаемое представление, например "1234.56". Только для логов и отладки —
  // не использовать как способ передачи между сервисами (округление зависит от scale).
  std::string to_string() const;

  // Перемножение двух чисел: результирующий scale = a.scale + b.scale.
  // Внимание: для больших значений возможно переполнение int64. Для MVP/dev этого
  // достаточно; в проде стоит заменить на boost::multiprecision или __int128.
  static Decimal mul(const Decimal& a, const Decimal& b);

  // Сложение: результат имеет scale = max(a.scale, b.scale) — так не теряется точность
  // меньшего числа. Реализовано через выравнивание (умножение units на 10^delta).
  static Decimal add(const Decimal& a, const Decimal& b);

  // Вычитание по тем же правилам выравнивания, что и add.
  static Decimal sub(const Decimal& a, const Decimal& b);

  // Возвращает наименьший из двух Decimal с предварительным выравниванием scale.
  static Decimal min(const Decimal& a, const Decimal& b);

  // Возвращает наибольший — симметрично min().
  static Decimal max(const Decimal& a, const Decimal& b);

  // Сравнение: -1 если a<b, 0 если равны, 1 если a>b.
  // Сравнивает после выравнивания scale, чтобы 1.0 и 1.00 считались равными.
  static int cmp(const Decimal& a, const Decimal& b);
};

}  // namespace cex::common
