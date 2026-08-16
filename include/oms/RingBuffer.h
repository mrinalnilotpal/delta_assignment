#pragma once

#include "oms/Common.h"

namespace oms {

// Fixed-capacity ring of samples: bounded memory/cost, old samples age out.
template <typename T>
class RingBuffer {
 public:
  explicit RingBuffer(std::size_t capacity) : capacity_(capacity) {
    buf_.reserve(capacity_);
  }

  void push(const T& v) {
    if (buf_.size() < capacity_) {
      buf_.push_back(v);
    } else {
      buf_[head_] = v;
    }
    head_ = (head_ + 1) % capacity_;
  }

  std::size_t size() const { return buf_.size(); }
  bool empty() const { return buf_.empty(); }
  std::size_t capacity() const { return capacity_; }
  void clear() { buf_.clear(); head_ = 0; }

  // Fraction of samples satisfying a predicate (0 when empty).
  template <typename Pred>
  double fraction(Pred pred) const {
    if (buf_.empty()) return 0.0;
    std::size_t n = 0;
    for (const auto& v : buf_) n += pred(v) ? 1 : 0;
    return static_cast<double>(n) / static_cast<double>(buf_.size());
  }

  // Linear-interpolation percentile over a copy of the samples (p in [0,1]).
  T percentile(double p) const {
    if (buf_.empty()) return T{};
    std::vector<T> s(buf_.begin(), buf_.end());
    std::sort(s.begin(), s.end());
    if (p <= 0.0) return s.front();
    if (p >= 1.0) return s.back();
    const double idx = p * static_cast<double>(s.size() - 1);
    const auto lo = static_cast<std::size_t>(idx);
    const auto hi = std::min(lo + 1, s.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return static_cast<T>(s[lo] + (s[hi] - s[lo]) * frac);
  }

 private:
  std::size_t    capacity_;
  std::size_t    head_{0};
  std::vector<T> buf_;
};

}  // namespace oms
