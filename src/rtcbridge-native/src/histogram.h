#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>

namespace gamebridge::rtc {
struct Histogram {
  std::array<uint64_t, 4096> buckets{};
  uint64_t maximum{};
  void Record(uint64_t us) {
    maximum = std::max(maximum, us);
    if (!us) {
      ++buckets[0];
      return;
    }
    const auto exponent = std::bit_width(us) - 1;
    const auto base = uint64_t{1} << exponent;
    const auto bucket = exponent * 128 + (us - base) * 128 / base + 1;
    ++buckets[std::min<uint64_t>(bucket, buckets.size() - 1)];
  }
  uint64_t Count() const {
    uint64_t count = 0;
    for (auto value : buckets)
      count += value;
    return count;
  }
  uint64_t Upper(size_t index) const {
    if (!index)
      return 0;
    if (index == buckets.size() - 1)
      return maximum;
    --index;
    const auto base = uint64_t{1} << (index / 128);
    return base + ((index % 128 + 1) * base + 127) / 128 - 1;
  }
  uint64_t Percentile(uint64_t percentile) const {
    const auto target = (Count() * percentile + 99) / 100;
    uint64_t sum = 0;
    for (size_t i = 0; i < buckets.size(); ++i) {
      sum += buckets[i];
      if (sum >= target)
        return Upper(i);
    }
    return 0;
  }
};
} // namespace gamebridge::rtc
