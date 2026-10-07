#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>

#include "dlp/packet.h"
#include "dlp/reassembler.h"

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FALLO %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                                        \
    }                                                                      \
  } while (0)

using namespace dlp;

static std::vector<uint8_t> MakeData(size_t n, uint8_t seed) {
  std::vector<uint8_t> d(n);
  for (size_t i = 0; i < n; ++i) d[i] = static_cast<uint8_t>(seed + i * 31);
  return d;
}

static void TestHeaderRoundtrip() {
  Header h;
  h.type = PacketType::kVideo;
  h.flags = kFlagKeyframe | kFlagLastPacket;
  h.frame_id = 0xDEADBEEF;
  h.packet_index = 7;
  h.packet_count = 9;
  h.timestamp_us = 0x0123456789ABCDEFull;
  h.payload_size = 0;
  uint8_t buf[kHeaderSize];
  WriteHeader(h, buf);
  auto p = ParseHeader(buf, sizeof(buf));
  CHECK(p.has_value());
  CHECK(p->frame_id == h.frame_id && p->packet_index == 7 && p->packet_count == 9);
  CHECK(p->timestamp_us == h.timestamp_us && p->flags == h.flags);
}

static void TestHeaderRejects() {
  uint8_t buf[kHeaderSize] = {};
  CHECK(!ParseHeader(buf, sizeof(buf)).has_value());  // magic incorrecto
  Header h;
  WriteHeader(h, buf);
  CHECK(ParseHeader(buf, sizeof(buf)).has_value());
  CHECK(!ParseHeader(buf, kHeaderSize - 1).has_value());  // corto
  h.payload_size = 100;
  WriteHeader(h, buf);
  CHECK(!ParseHeader(buf, kHeaderSize).has_value());  // payload declarado no cabe
}

static void TestRoundtripSizes() {
  for (size_t size : {size_t{1}, kMaxPayload - 1, kMaxPayload, kMaxPayload + 1,
                      kMaxPayload * 3, size_t{100000}}) {
    FrameReassembler r;
    auto data = MakeData(size, 3);
    auto packets = PacketizeFrame(data.data(), data.size(), 1, true, 42);
    CHECK(packets.size() == (size + kMaxPayload - 1) / kMaxPayload);
    std::optional<Frame> out;
    for (size_t i = 0; i < packets.size(); ++i) {
      CHECK(packets[i].size() <= kMaxPacketSize);
      auto f = r.Push(packets[i].data(), packets[i].size());
      if (i + 1 < packets.size()) CHECK(!f.has_value());
      else out = std::move(f);
    }
    CHECK(out.has_value());
    CHECK(out->data == data && out->keyframe && out->timestamp_us == 42);
    CHECK(!r.needs_keyframe());
  }
}

static void TestOutOfOrderAndDuplicates() {
  FrameReassembler r;
  auto data = MakeData(20000, 9);
  auto packets = PacketizeFrame(data.data(), data.size(), 5, false, 1);
  std::vector<size_t> order(packets.size());
  std::iota(order.begin(), order.end(), 0);
  std::mt19937 rng(1234);
  std::shuffle(order.begin(), order.end(), rng);
  std::optional<Frame> out;
  for (size_t i : order) {
    auto f = r.Push(packets[i].data(), packets[i].size());
    // Un duplicado no debe romper nada.
    CHECK(!r.Push(packets[i].data(), packets[i].size()).has_value());
    if (f) out = std::move(f);
  }
  CHECK(out.has_value() && out->data == data);
}

static void TestLossRequestsKeyframe() {
  FrameReassembler r;
  auto k = MakeData(5000, 1);
  auto kp = PacketizeFrame(k.data(), k.size(), 1, true, 0);
  for (auto& p : kp) r.Push(p.data(), p.size());
  CHECK(r.frames_completed() == 1 && !r.needs_keyframe());

  // El frame 2 pierde un paquete; el 3 llega completo.
  auto f2 = MakeData(5000, 2);
  auto p2 = PacketizeFrame(f2.data(), f2.size(), 2, false, 0);
  for (size_t i = 0; i + 1 < p2.size(); ++i) r.Push(p2[i].data(), p2[i].size());
  auto f3 = MakeData(3000, 3);
  auto p3 = PacketizeFrame(f3.data(), f3.size(), 3, false, 0);
  std::optional<Frame> got;
  for (auto& p : p3) {
    auto f = r.Push(p.data(), p.size());
    if (f) got = std::move(f);
  }
  CHECK(got.has_value() && got->frame_id == 3);
  CHECK(r.frames_dropped() == 1 && r.needs_keyframe());

  // El paquete tardío del frame 2 se ignora (ya pasó el 3).
  CHECK(!r.Push(p2.back().data(), p2.back().size()).has_value());

  // Un frame clave limpia la petición.
  auto k2 = MakeData(2000, 4);
  auto pk = PacketizeFrame(k2.data(), k2.size(), 4, true, 0);
  for (auto& p : pk) r.Push(p.data(), p.size());
  CHECK(!r.needs_keyframe());
}

static void TestGapCountsAsLoss() {
  FrameReassembler r;
  auto a = MakeData(100, 1);
  auto b = MakeData(100, 2);
  for (auto& p : PacketizeFrame(a.data(), a.size(), 10, true, 0)) r.Push(p.data(), p.size());
  for (auto& p : PacketizeFrame(b.data(), b.size(), 13, false, 0)) r.Push(p.data(), p.size());
  CHECK(r.frames_dropped() == 2 && r.needs_keyframe());
}

static void TestStreamInfoAndControl() {
  StreamInfo info;
  info.width = 2944;
  info.height = 1840;
  info.fps = 60;
  info.codec = Codec::kHevc;
  auto payload = EncodeStreamInfo(info);
  auto pkt = MakeControlPacket(PacketType::kStreamInfo, payload.data(), payload.size(), 77);
  auto h = ParseHeader(pkt.data(), pkt.size());
  CHECK(h && h->type == PacketType::kStreamInfo && h->timestamp_us == 77);
  auto d = DecodeStreamInfo(pkt.data() + kHeaderSize, h->payload_size);
  CHECK(d && d->width == 2944 && d->height == 1840 && d->fps == 60 && d->codec == Codec::kHevc);
}

static void TestBurstTypes() {
  const uint8_t payload[kBurstRequestSize] = {0x90, 0x01, 0xB0, 0x04, 0x64, 0x00};
  auto pkt = MakeControlPacket(PacketType::kBurstRequest, payload, sizeof(payload), 42);
  auto h = ParseHeader(pkt.data(), pkt.size());
  CHECK(h && h->type == PacketType::kBurstRequest && h->payload_size == kBurstRequestSize);
  CHECK(static_cast<uint8_t>(PacketType::kBurstData) == 8);
}

int main() {
  TestBurstTypes();
  TestHeaderRoundtrip();
  TestHeaderRejects();
  TestRoundtripSizes();
  TestOutOfOrderAndDuplicates();
  TestLossRequestsKeyframe();
  TestGapCountsAsLoss();
  TestStreamInfoAndControl();
  std::puts("dlp_tests: OK");
  return 0;
}
