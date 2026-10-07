#include "dlp/packet.h"

#include <algorithm>
#include <cstring>

namespace dlp {
namespace {

void Put16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void Put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void Put64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint16_t Get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t Get32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
  return v;
}
uint64_t Get64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

}  // namespace

void WriteHeader(const Header& h, uint8_t* out) {
  Put16(out + 0, kMagic);
  out[2] = kVersion;
  out[3] = static_cast<uint8_t>(h.type);
  out[4] = h.flags;
  out[5] = 0;
  Put32(out + 6, h.frame_id);
  Put16(out + 10, h.packet_index);
  Put16(out + 12, h.packet_count);
  Put64(out + 14, h.timestamp_us);
  Put16(out + 22, h.payload_size);
}

std::optional<Header> ParseHeader(const uint8_t* data, size_t size) {
  if (size < kHeaderSize) return std::nullopt;
  if (Get16(data) != kMagic || data[2] != kVersion) return std::nullopt;
  Header h;
  h.type = static_cast<PacketType>(data[3]);
  h.flags = data[4];
  h.frame_id = Get32(data + 6);
  h.packet_index = Get16(data + 10);
  h.packet_count = Get16(data + 12);
  h.timestamp_us = Get64(data + 14);
  h.payload_size = Get16(data + 22);
  if (h.payload_size > kMaxPayload || size < kHeaderSize + h.payload_size) {
    return std::nullopt;
  }
  return h;
}

std::vector<std::vector<uint8_t>> PacketizeFrame(const uint8_t* data, size_t size,
                                                 uint32_t frame_id, bool keyframe,
                                                 uint64_t timestamp_us) {
  std::vector<std::vector<uint8_t>> packets;
  if (size == 0) return packets;
  const size_t count = (size + kMaxPayload - 1) / kMaxPayload;
  if (count > 0xFFFF) return packets;

  packets.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const size_t offset = i * kMaxPayload;
    const size_t n = std::min(kMaxPayload, size - offset);
    Header h;
    h.type = PacketType::kVideo;
    h.flags = static_cast<uint8_t>((keyframe ? kFlagKeyframe : 0) |
                                   (i + 1 == count ? kFlagLastPacket : 0));
    h.frame_id = frame_id;
    h.packet_index = static_cast<uint16_t>(i);
    h.packet_count = static_cast<uint16_t>(count);
    h.timestamp_us = timestamp_us;
    h.payload_size = static_cast<uint16_t>(n);

    std::vector<uint8_t> pkt(kHeaderSize + n);
    WriteHeader(h, pkt.data());
    std::memcpy(pkt.data() + kHeaderSize, data + offset, n);
    packets.push_back(std::move(pkt));
  }
  return packets;
}

std::vector<uint8_t> MakeControlPacket(PacketType type, const uint8_t* payload,
                                       size_t payload_size, uint64_t timestamp_us) {
  payload_size = std::min(payload_size, kMaxPayload);
  Header h;
  h.type = type;
  h.timestamp_us = timestamp_us;
  h.payload_size = static_cast<uint16_t>(payload_size);
  std::vector<uint8_t> pkt(kHeaderSize + payload_size);
  WriteHeader(h, pkt.data());
  if (payload_size > 0) std::memcpy(pkt.data() + kHeaderSize, payload, payload_size);
  return pkt;
}

std::vector<uint8_t> EncodeStreamInfo(const StreamInfo& info) {
  std::vector<uint8_t> out(kStreamInfoSize);
  Put16(out.data() + 0, info.width);
  Put16(out.data() + 2, info.height);
  Put16(out.data() + 4, info.fps);
  out[6] = static_cast<uint8_t>(info.codec);
  return out;
}

std::optional<StreamInfo> DecodeStreamInfo(const uint8_t* payload, size_t size) {
  if (size < kStreamInfoSize) return std::nullopt;
  StreamInfo info;
  info.width = Get16(payload + 0);
  info.height = Get16(payload + 2);
  info.fps = Get16(payload + 4);
  info.codec = static_cast<Codec>(payload[6]);
  return info;
}

}  // namespace dlp
