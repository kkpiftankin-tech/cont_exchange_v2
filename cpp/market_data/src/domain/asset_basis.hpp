#pragma once
// ============================================================================
// asset_basis.hpp — F-05A (T-F05A-201). market_data domain VO.
//
// Стабильное отображение актив → индекс. e_X — единичный вектор с 1 на индексе
// актива X. КЛЮЧЕВОЙ ИНВАРИАНТ (R-F05A-002, U-F05A-004): один и тот же актив на
// РАЗНЫХ парах/venue получает ОДИН индекс (общая компонента). Именно это связывает
// уровни между площадками — треугольный/кросс-venue клиринг возникает из Wx=0 на
// общих компонентах, без синтетической pair-book. Порядок детерминирован
// (лексикографическая сортировка) → воспроизводимо для replay.
// ============================================================================

#include <map>
#include <string>
#include <vector>

namespace cex::market_data::domain {

struct AssetBasis {
  std::vector<std::string> assets;    ///< index → asset (canonical sorted)
  std::map<std::string, int> index_of;///< asset → index
  int num_assets{0};

  int IndexOf(const std::string& asset) const {
    auto it = index_of.find(asset);
    return it == index_of.end() ? -1 : it->second;
  }
};

}  // namespace cex::market_data::domain
