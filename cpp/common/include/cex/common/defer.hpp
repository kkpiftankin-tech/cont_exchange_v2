#pragma once

#include <utility>

namespace cex::common {

template <typename F>
class [[nodiscard]] Defer {
 public:
  explicit Defer(F f) : f_(std::move(f)) {}
  ~Defer() { f_(); }

  Defer(const Defer&) = delete;
  Defer& operator=(const Defer&) = delete;

 private:
  F f_;
};

}  // namespace cex::common
