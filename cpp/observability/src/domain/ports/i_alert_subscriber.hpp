#pragma once

#include <optional>

#include "domain/entities/alert.hpp"

namespace cex::observability::domain {

struct IAlertSubscriber {
  virtual std::optional<Alert> Consume() = 0;

 protected:
  virtual ~IAlertSubscriber() = default;
};

}  // namespace cex::observability::domain
