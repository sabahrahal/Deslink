#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

// Guarda los últimos paquetes de vídeo enviados para poder reenviar los que la tablet reclame (NACK).
class PacketHistory {
 public:
  explicit PacketHistory(size_t capacity = 4096) : capacity_(capacity) {}

  void Put(uint32_t frame_id, uint16_t index, const std::vector<uint8_t>& packet) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t key = Key(frame_id, index);
    if (map_.emplace(key, packet).second) order_.push_back(key);
    while (order_.size() > capacity_) {
      map_.erase(order_.front());
      order_.pop_front();
    }
  }

  // Devuelve false si el paquete ya no está.
  bool Get(uint32_t frame_id, uint16_t index, std::vector<uint8_t>* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(Key(frame_id, index));
    if (it == map_.end()) return false;
    *out = it->second;
    return true;
  }

  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    map_.clear();
    order_.clear();
  }

 private:
  static uint64_t Key(uint32_t frame_id, uint16_t index) { return (static_cast<uint64_t>(frame_id) << 16) | index; }

  size_t capacity_;
  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, std::vector<uint8_t>> map_;
  std::deque<uint64_t> order_;
};
