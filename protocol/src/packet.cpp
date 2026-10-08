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

std::vector<uint8_t> MakeAudioPacket(uint32_t seq, uint64_t timestamp_us, const uint8_t* cur, size_t cur_size,
                                     const uint8_t* prev, size_t prev_size) {
  if (cur_size == 0 || 1 + 2 + cur_size > kMaxPayload) return {};
  const bool with_prev = prev && prev_size > 0 && 1 + 4 + cur_size + prev_size <= kMaxPayload;
  const size_t payload = 1 + 2 + cur_size + (with_prev ? 2 + prev_size : 0);
  Header h;
  h.type = PacketType::kAudio;
  h.frame_id = seq;
  h.timestamp_us = timestamp_us;
  h.payload_size = static_cast<uint16_t>(payload);
  std::vector<uint8_t> pkt(kHeaderSize + payload);
  WriteHeader(h, pkt.data());
  uint8_t* p = pkt.data() + kHeaderSize;
  *p++ = with_prev ? 2 : 1;
  Put16(p, static_cast<uint16_t>(cur_size));
  std::memcpy(p + 2, cur, cur_size);
  p += 2 + cur_size;
  if (with_prev) {
    Put16(p, static_cast<uint16_t>(prev_size));
    std::memcpy(p + 2, prev, prev_size);
  }
  return pkt;
}

std::vector<AudioFrameRef> ParseAudioPacket(const Header& header, const uint8_t* payload) {
  std::vector<AudioFrameRef> frames;
  if (header.payload_size < 1) return frames;
  const size_t n = payload[0];
  size_t offset = 1;
  for (size_t i = 0; i < n; ++i) {
    if (offset + 2 > header.payload_size) return {};
    const size_t size = Get16(payload + offset);
    offset += 2;
    if (size == 0 || offset + size > header.payload_size) return {};
    frames.push_back({header.frame_id - static_cast<uint32_t>(i), payload + offset, size});
    offset += size;
  }
  return frames;
}

std::vector<uint8_t> EncodeStreamInfo(const StreamInfo& info) {
  const size_t name_len = std::min(info.host_name.size(), kMaxHostName);
  std::vector<uint8_t> out(kStreamInfoSize + 2 + name_len);
  Put16(out.data() + 0, info.width);
  Put16(out.data() + 2, info.height);
  Put16(out.data() + 4, info.fps);
  out[6] = static_cast<uint8_t>(info.codec);
  out[7] = info.host_flags;
  out[8] = static_cast<uint8_t>(name_len);
  if (name_len > 0) std::memcpy(out.data() + 9, info.host_name.data(), name_len);
  return out;
}

std::optional<StreamInfo> DecodeStreamInfo(const uint8_t* payload, size_t size) {
  if (size < kStreamInfoSize) return std::nullopt;
  StreamInfo info;
  info.width = Get16(payload + 0);
  info.height = Get16(payload + 2);
  info.fps = Get16(payload + 4);
  info.codec = static_cast<Codec>(payload[6]);
  if (size >= kStreamInfoSize + 2) {
    info.host_flags = payload[7];
    const size_t name_len = std::min<size_t>({payload[8], kMaxHostName, size - kStreamInfoSize - 2});
    info.host_name.assign(reinterpret_cast<const char*>(payload + 9), name_len);
  }
  return info;
}

std::vector<uint8_t> EncodeHelloConfig(const HelloConfig& c) {
  std::vector<uint8_t> out(kHelloConfigSize);
  Put16(out.data() + 0, c.width);
  Put16(out.data() + 2, c.height);
  Put16(out.data() + 4, c.fps);
  Put16(out.data() + 6, c.bitrate_mbps);
  out[8] = c.codec;
  out[9] = c.reserved;
  Put16(out.data() + 10, c.density_dpi);
  Put16(out.data() + 12, c.refresh_hz);
  return out;
}

std::optional<HelloConfig> DecodeHelloConfig(const uint8_t* payload, size_t size) {
  if (size == 0) return std::nullopt;
  // Lo que falte (un cliente con menos campos) cuenta como 0.
  uint8_t b[kHelloConfigSize] = {};
  std::memcpy(b, payload, std::min(size, kHelloConfigSize));
  HelloConfig c;
  c.width = Get16(b + 0);
  c.height = Get16(b + 2);
  c.fps = Get16(b + 4);
  c.bitrate_mbps = Get16(b + 6);
  c.codec = b[8];
  c.reserved = b[9];
  c.density_dpi = Get16(b + 10);
  c.refresh_hz = Get16(b + 12);
  return c;
}

QualityRequest ProfileDefaults(Profile profile) {
  QualityRequest r;
  r.profile = profile;
  switch (profile) {
    case Profile::kMaxQuality:
      break;  // sin límites: lo que permita el host
    case Profile::kBalanced:
      r.max_fps = 60;
      r.max_bitrate_bps = 30'000'000;
      break;
    case Profile::kSaver:
      r.max_fps = 30;
      r.max_bitrate_bps = 12'000'000;
      r.scale_percent = 75;
      break;
    case Profile::kMinimum:
      r.max_fps = 30;  // 24 fps obligaría a la pantalla a 120 Hz para no dar saltos: gasta más que 30 a 60 Hz
      r.max_bitrate_bps = 8'000'000;
      r.scale_percent = 60;
      break;
  }
  return r;
}

const char* ProfileName(Profile profile) {
  switch (profile) {
    case Profile::kMaxQuality: return "M\xC3\xA1xima calidad";  // UTF-8 escrito a mano: la librería no se compila con /utf-8
    case Profile::kBalanced: return "Equilibrado";
    case Profile::kSaver: return "Ahorro";
    case Profile::kMinimum: return "M\xC3\xADnimo";
  }
  return "Equilibrado";
}

std::vector<uint8_t> EncodeQualityRequest(const QualityRequest& request) {
  std::vector<uint8_t> out(kQualityRequestSize);
  Put16(out.data() + 0, request.max_fps);
  const uint32_t units = (request.max_bitrate_bps + 50'000) / 100'000;
  Put16(out.data() + 2, static_cast<uint16_t>(std::min<uint32_t>(units, 0xFFFF)));
  out[4] = request.scale_percent;
  out[5] = static_cast<uint8_t>(request.profile);
  return out;
}

std::optional<QualityRequest> DecodeQualityRequest(const uint8_t* payload, size_t size) {
  if (size < kQualityRequestSize) return std::nullopt;
  QualityRequest r;
  r.max_fps = Get16(payload + 0);
  if (r.max_fps != 0) r.max_fps = std::clamp<uint16_t>(r.max_fps, 10, 240);
  r.max_bitrate_bps = Get16(payload + 2) * 100'000u;
  if (r.max_bitrate_bps != 0) r.max_bitrate_bps = std::max<uint32_t>(r.max_bitrate_bps, 1'000'000);
  r.scale_percent = payload[4] == 0 ? 100 : std::clamp<uint8_t>(payload[4], 50, 100);
  r.profile = payload[5] <= static_cast<uint8_t>(Profile::kMinimum) ? static_cast<Profile>(payload[5])
                                                                    : Profile::kBalanced;
  return r;
}

void ScaledSize(uint32_t width, uint32_t height, uint8_t percent, uint32_t* out_width, uint32_t* out_height) {
  percent = percent == 0 ? 100 : std::min<uint8_t>(percent, 100);
  uint32_t w = width * percent / 100;
  if (w < 640 && width > 640) w = 640;
  if (w > width) w = width;
  uint32_t h = static_cast<uint32_t>(static_cast<uint64_t>(height) * w / (width ? width : 1));
  *out_width = w & ~1u;
  *out_height = h & ~1u;
}

}  // namespace dlp
