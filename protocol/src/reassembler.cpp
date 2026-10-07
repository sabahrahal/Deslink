#include "dlp/reassembler.h"

#include <algorithm>
#include <cstring>

namespace dlp {

std::optional<Frame> FrameReassembler::Push(const uint8_t* packet, size_t size) {
  auto header = ParseHeader(packet, size);
  if (!header || header->type != PacketType::kVideo) return std::nullopt;
  const Header& h = *header;

  if (h.packet_count == 0 || h.packet_index >= h.packet_count) return std::nullopt;
  const bool is_last = h.packet_index + 1 == h.packet_count;
  // Todos los paquetes salvo el último van llenos: así la posición es index * kMaxPayload.
  if (!is_last && h.payload_size != kMaxPayload) return std::nullopt;
  if (h.payload_size == 0) return std::nullopt;

  // Frames ya entregados (o más viejos) se ignoran.
  if (last_delivered_ && static_cast<int32_t>(h.frame_id - *last_delivered_) <= 0) {
    return std::nullopt;
  }

  auto it = pending_.find(h.frame_id);
  if (it == pending_.end()) {
    // Demasiados frames a medias: el más viejo se da por perdido.
    while (pending_.size() >= kMaxPending) {
      pending_.erase(pending_.begin());
      ++frames_dropped_;
      needs_keyframe_ = true;
    }
    Pending p;
    p.packet_count = h.packet_count;
    p.keyframe = (h.flags & kFlagKeyframe) != 0;
    p.timestamp_us = h.timestamp_us;
    p.have.assign(h.packet_count, false);
    p.data.resize(static_cast<size_t>(h.packet_count) * kMaxPayload);
    it = pending_.emplace(h.frame_id, std::move(p)).first;
  }

  Pending& p = it->second;
  if (p.packet_count != h.packet_count || p.have[h.packet_index]) return std::nullopt;

  std::memcpy(p.data.data() + static_cast<size_t>(h.packet_index) * kMaxPayload,
              packet + kHeaderSize, h.payload_size);
  p.have[h.packet_index] = true;
  ++p.received;
  if (is_last) {
    p.last_payload = h.payload_size;
    p.have_last = true;
  }

  if (p.received != p.packet_count) return std::nullopt;

  Frame frame;
  frame.frame_id = h.frame_id;
  frame.keyframe = p.keyframe;
  frame.timestamp_us = p.timestamp_us;
  frame.data = std::move(p.data);
  frame.data.resize(static_cast<size_t>(p.packet_count - 1) * kMaxPayload + p.last_payload);

  // Frames anteriores incompletos y huecos de numeración cuentan como pérdidas.
  uint64_t lost = 0;
  for (auto i = pending_.begin(); i != pending_.end() && i->first != h.frame_id;) {
    if (static_cast<int32_t>(i->first - h.frame_id) < 0) {
      ++lost;
      i = pending_.erase(i);
    } else {
      ++i;
    }
  }
  pending_.erase(h.frame_id);
  if (last_delivered_) {
    const uint32_t gap = h.frame_id - *last_delivered_ - 1;
    if (gap > 0) lost = std::max<uint64_t>(lost, gap);
  }
  if (lost > 0) {
    frames_dropped_ += lost;
    needs_keyframe_ = true;
  }
  if (frame.keyframe) needs_keyframe_ = false;

  last_delivered_ = h.frame_id;
  ++frames_completed_;
  return frame;
}

}  // namespace dlp
