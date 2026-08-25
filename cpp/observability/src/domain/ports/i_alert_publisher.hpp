#pragma once

#include "domain/entities/alert.hpp"

namespace cex::observability::domain {

struct IAlertPublisher {
  virtual void Publish(const Alert&) = 0;

 protected:
  virtual ~IAlertPublisher() = default;
};

}  // namespace cex::observability::domain
