#pragma once
#include "gamebridge_rtc.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace gamebridge::rtc {

template <class T> class HandleTable {
public:
  gb_rtc_handle Insert(std::shared_ptr<T> value) {
    std::lock_guard lock(mutex_);
    for (uint32_t i = 0; i < slots_.size(); ++i) {
      auto &slot = slots_[i];
      // Exhausted generations are permanently retired, never wrapped.
      if (!slot.value && slot.generation != UINT32_MAX) {
        ++slot.generation;
        slot.value = std::move(value);
        return (uint64_t(slot.generation) << 32) | (i + 1);
      }
    }
    return 0;
  }
  std::shared_ptr<T> Get(gb_rtc_handle handle) {
    std::lock_guard lock(mutex_);
    const auto index = uint32_t(handle);
    if (!index || index > slots_.size())
      return {};
    auto &slot = slots_[index - 1];
    return slot.generation == (handle >> 32) ? slot.value : nullptr;
  }
  void Erase(gb_rtc_handle handle) {
    std::shared_ptr<T> removed;
    {
      std::lock_guard lock(mutex_);
      const auto index = uint32_t(handle);
      if (!index || index > slots_.size())
        return;
      auto &slot = slots_[index - 1];
      if (slot.generation == (handle >> 32))
        removed.swap(slot.value);
    }
  }

private:
  struct Slot {
    uint32_t generation{};
    std::shared_ptr<T> value;
  };
  std::array<Slot, GB_RTC_MAX_SESSIONS> slots_;
  std::mutex mutex_;
};

struct Media {
  std::vector<uint8_t> bytes;
  uint32_t timestamp{}, duration{}, width{}, height{};
  bool key{};
};

// One bounded queue per direction/stream. Media is owned on return. The free
// list retains capacity after warmup; a producer never waits for capacity.
class MediaQueue {
public:
  MediaQueue(size_t capacity, uint32_t duration_limit)
      : capacity_(capacity), duration_limit_(duration_limit) {}
  gb_rtc_result Push(std::span<const uint8_t> bytes, uint32_t timestamp,
                     uint32_t duration, uint32_t width = 0, uint32_t height = 0,
                     bool key = false) {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock())
      return GB_RTC_BACKPRESSURE;
    if (closed_)
      return GB_RTC_STATE;
    if (duration_limit_ && duration > duration_limit_)
      return GB_RTC_INVALID;
    while (!pending_.empty() &&
           (pending_.size() >= capacity_ ||
            (duration_limit_ && duration_ + duration > duration_limit_))) {
      duration_ -= pending_.front().duration;
      RecycleUnlocked(std::move(pending_.front()));
      pending_.pop_front();
      ++dropped_;
    }
    Media item;
    if (!free_.empty()) {
      item = std::move(free_.back());
      free_.pop_back();
    }
    item.bytes.assign(bytes.begin(), bytes.end());
    item.timestamp = timestamp;
    item.duration = duration;
    item.width = width;
    item.height = height;
    item.key = key;
    pending_.push_back(std::move(item));
    duration_ += duration;
    max_depth_ = std::max(max_depth_, pending_.size());
    return GB_RTC_OK;
  }
  std::optional<Media> Pop() {
    std::lock_guard lock(mutex_);
    if (pending_.empty() || closed_)
      return {};
    Media item = std::move(pending_.front());
    pending_.pop_front();
    duration_ -= item.duration;
    return item;
  }
  void Recycle(Media item) {
    std::lock_guard lock(mutex_);
    RecycleUnlocked(std::move(item));
  }
  void Close() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    pending_.clear();
    free_.clear();
    duration_ = 0;
  }
  uint64_t Dropped() const { return dropped_.load(std::memory_order_relaxed); }
  size_t MaxDepth() const {
    std::lock_guard lock(mutex_);
    return max_depth_;
  }

private:
  void RecycleUnlocked(Media item) {
    if (free_.size() < capacity_ + 1)
      free_.push_back(std::move(item));
  }
  const size_t capacity_;
  const uint32_t duration_limit_;
  mutable std::mutex mutex_;
  std::deque<Media> pending_;
  std::vector<Media> free_;
  uint32_t duration_{};
  std::atomic<uint64_t> dropped_{};
  size_t max_depth_{};
  bool closed_{};
};

inline uint64_t ReceiverDrops(const MediaQueue &queue,
                              const std::atomic<uint64_t> &contention) {
  return queue.Dropped() + contention.load(std::memory_order_relaxed);
}

inline uint32_t OpusSamples(std::span<const uint8_t> packet) {
  if (packet.empty())
    return 0;
  auto toc = packet[0];
  uint32_t per_frame;
  if (toc & 0x80)
    per_frame = (48000u << ((toc >> 3) & 3)) / 400;
  else if ((toc & 0x60) == 0x60)
    per_frame = (toc & 8) ? 960 : 480;
  else {
    const auto n = (toc >> 3) & 3;
    per_frame = n == 3 ? 2880 : (48000u << n) / 100;
  }
  uint32_t frames = 1;
  if ((toc & 3) == 1 || (toc & 3) == 2)
    frames = 2;
  if ((toc & 3) == 3) {
    if (packet.size() < 2)
      return 0;
    frames = packet[1] & 63;
  }
  const auto total = frames * per_frame;
  return frames && total <= 5760 ? total : 0;
}

inline bool ValidAnnexB(std::span<const uint8_t> bytes) {
  if (bytes.size() < 4 || bytes.size() > GB_RTC_MAX_VIDEO_BYTES)
    return false;
  const auto find = [&](size_t begin) {
    for (size_t i = begin; i + 2 < bytes.size(); ++i)
      if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1)
        return i;
    return bytes.size();
  };
  const auto first = find(0);
  if (first > 1 || (first == 1 && bytes[0] != 0))
    return false;
  size_t position = first + 3, nals = 0, normalized = 0;
  for (;;) {
    const auto next = find(position);
    auto end = next;
    if (next < bytes.size() && next > 0 && bytes[next - 1] == 0)
      --end;
    if (end <= position || ++nals > 4096)
      return false;
    const auto type = bytes[position] & 31;
    if ((bytes[position] & 128) || !type || type >= 24)
      return false;
    if (type != 9 && type != 12) {
      normalized += end - position + 4;
      if (normalized > GB_RTC_MAX_VIDEO_BYTES)
        return false;
    }
    if (next == bytes.size())
      return normalized != 0;
    position = next + 3;
  }
}

// This TLS applies across sessions: callbacks may close each other's sessions.
inline thread_local unsigned callback_depth = 0;
class CallbackGate {
public:
  template <class F> bool Invoke(F &&callback) {
    std::unique_lock lock(mutex_);
    if (closed_)
      return false;
    // A session's dispatcher is the only emitter, except the test-only probe.
    finished_.wait(lock, [&] { return !active_ || closed_; });
    if (closed_)
      return false;
    active_ = true;
    lock.unlock();
    ++callback_depth;
    try {
      callback();
    } catch (...) { /* no exception may cross a C callback */
    }
    --callback_depth;
    lock.lock();
    active_ = false;
    finished_.notify_all();
    return true;
  }
  void Close() {
    std::unique_lock lock(mutex_);
    closed_ = true;
    finished_.notify_all();
    if (!callback_depth)
      finished_.wait(lock, [&] { return !active_; });
  }

private:
  std::mutex mutex_;
  std::condition_variable finished_;
  bool closed_{}, active_{};
};
} // namespace gamebridge::rtc
