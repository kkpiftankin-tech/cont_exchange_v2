#pragma once

#include <boost/container_hash/hash.hpp>

#include "domain/entities/alert.hpp"

namespace cex::observability::app {

struct AlertKey {
  std::string venue;
  domain::AlertCode code;

  bool operator==(const AlertKey&) const = default;
};

struct AlertCheck {
  std::string venue;
  domain::AlertCode code;
  bool should_fire;
  bool should_resolve;
  domain::Severity severity;
};

}  // namespace cex::observability::app

template <>
struct std::hash<cex::observability::app::AlertKey> {
  std::size_t operator()(const cex::observability::app::AlertKey& k) const noexcept {
    std::size_t seed = 0;
    boost::hash_combine(seed, k.venue);
    boost::hash_combine(seed, k.code);
    return seed;
  }
};
