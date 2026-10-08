#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <string>

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

static void TestStreamInfoExtension() {
  StreamInfo info;
  info.width = 1920;
  info.height = 1080;
  info.fps = 120;
  info.codec = Codec::kH264;
  info.host_flags = kHostAcceptsHelloConfig;
  info.host_name = "ESCRITORIO-SABAH";
  auto payload = EncodeStreamInfo(info);
  CHECK(payload.size() == kStreamInfoSize + 2 + 16);
  auto d = DecodeStreamInfo(payload.data(), payload.size());
  CHECK(d && d->host_flags == kHostAcceptsHelloConfig && d->host_name == "ESCRITORIO-SABAH");
  // Un host antiguo manda solo 7 bytes: sin nombre ni banderas.
  auto old = DecodeStreamInfo(payload.data(), kStreamInfoSize);
  CHECK(old && old->width == 1920 && old->host_flags == 0 && old->host_name.empty());
  // Un nombre declarado más largo que el payload se recorta.
  payload[8] = 200;
  auto cut = DecodeStreamInfo(payload.data(), payload.size());
  CHECK(cut && cut->host_name == "ESCRITORIO-SABAH");
  // Nombres demasiado largos se recortan a kMaxHostName.
  info.host_name = std::string(100, 'x');
  CHECK(EncodeStreamInfo(info).size() == kStreamInfoSize + 2 + kMaxHostName);
}

static void TestHelloConfig() {
  HelloConfig c;
  c.width = 2944;
  c.height = 1840;
  c.fps = 90;
  c.bitrate_mbps = 30;
  c.codec = 2;
  c.density_dpi = 320;
  c.refresh_hz = 90;
  auto payload = EncodeHelloConfig(c);
  // Mismos bytes que comprueba android/app/src/test/.../HelloConfigTest.kt.
  const uint8_t golden[kHelloConfigSize] = {0x80, 0x0B, 0x30, 0x07, 0x5A, 0x00, 0x1E,
                                             0x00, 0x02, 0x00, 0x40, 0x01, 0x5A, 0x00};
  CHECK(payload.size() == kHelloConfigSize && std::equal(payload.begin(), payload.end(), golden));
  auto pkt = MakeControlPacket(PacketType::kHello, payload.data(), payload.size(), 5);
  auto h = ParseHeader(pkt.data(), pkt.size());
  CHECK(h && h->type == PacketType::kHello);
  auto d = DecodeHelloConfig(pkt.data() + kHeaderSize, h->payload_size);
  CHECK(d && *d == c);
  // HELLO vacío (cliente antiguo o búsqueda): nada que aplicar.
  CHECK(!DecodeHelloConfig(pkt.data() + kHeaderSize, 0).has_value());
  // Un cliente con menos campos: lo que falta es 0.
  auto shortc = DecodeHelloConfig(payload.data(), 4);
  CHECK(shortc && shortc->width == 2944 && shortc->height == 1840 && shortc->fps == 0 && shortc->codec == 0);
  // Bytes de más (campos futuros) se ignoran.
  payload.push_back(0xFF);
  auto longc = DecodeHelloConfig(payload.data(), payload.size());
  CHECK(longc && *longc == c);
  CHECK(static_cast<uint8_t>(PacketType::kDiscover) == 12);
}

static void TestBurstTypes() {
  const uint8_t payload[kBurstRequestSize] = {0x90, 0x01, 0xB0, 0x04, 0x64, 0x00};
  auto pkt = MakeControlPacket(PacketType::kBurstRequest, payload, sizeof(payload), 42);
  auto h = ParseHeader(pkt.data(), pkt.size());
  CHECK(h && h->type == PacketType::kBurstRequest && h->payload_size == kBurstRequestSize);
  CHECK(static_cast<uint8_t>(PacketType::kBurstData) == 8);
  CHECK(static_cast<uint8_t>(PacketType::kRateReport) == 9);
  CHECK(static_cast<uint8_t>(PacketType::kNack) == 10);
  CHECK(static_cast<uint8_t>(PacketType::kTouch) == 11);
}

static void TestAudioPacket() {
  CHECK(static_cast<uint8_t>(PacketType::kAudio) == 14);
  CHECK(static_cast<uint8_t>(PacketType::kAudioControl) == 15);
  auto cur = MakeData(160, 1);
  auto prev = MakeData(150, 2);
  auto pkt = MakeAudioPacket(1000, 55, cur.data(), cur.size(), prev.data(), prev.size());
  auto h = ParseHeader(pkt.data(), pkt.size());
  CHECK(h && h->type == PacketType::kAudio && h->frame_id == 1000 && h->timestamp_us == 55);
  auto frames = ParseAudioPacket(*h, pkt.data() + kHeaderSize);
  CHECK(frames.size() == 2);
  CHECK(frames[0].seq == 1000 && frames[0].size == cur.size() &&
        std::equal(cur.begin(), cur.end(), frames[0].data));
  CHECK(frames[1].seq == 999 && frames[1].size == prev.size() &&
        std::equal(prev.begin(), prev.end(), frames[1].data));

  // Sin el anterior, o si no caben los dos, va solo el actual.
  pkt = MakeAudioPacket(0, 1, cur.data(), cur.size(), nullptr, 0);
  h = ParseHeader(pkt.data(), pkt.size());
  frames = ParseAudioPacket(*h, pkt.data() + kHeaderSize);
  CHECK(frames.size() == 1 && frames[0].seq == 0);
  auto big = MakeData(kMaxPayload - 3, 3);
  pkt = MakeAudioPacket(5, 1, big.data(), big.size(), prev.data(), prev.size());
  h = ParseHeader(pkt.data(), pkt.size());
  CHECK(h && ParseAudioPacket(*h, pkt.data() + kHeaderSize).size() == 1);
  CHECK(MakeAudioPacket(5, 1, big.data(), big.size() + 1, nullptr, 0).empty());

  // Un tamaño que se sale del payload invalida el paquete entero.
  pkt = MakeAudioPacket(7, 1, cur.data(), cur.size(), prev.data(), prev.size());
  h = ParseHeader(pkt.data(), pkt.size());
  pkt[kHeaderSize + 1 + 2 + cur.size()] = 0xFF;
  CHECK(ParseAudioPacket(*h, pkt.data() + kHeaderSize).empty());
}


static void TestQualityRequest() {
  CHECK(static_cast<uint8_t>(PacketType::kQualityRequest) == 16);
  QualityRequest q = ProfileDefaults(Profile::kSaver);
  auto payload = EncodeQualityRequest(q);
  CHECK(payload.size() == kQualityRequestSize);
  auto pkt = MakeControlPacket(PacketType::kQualityRequest, payload.data(), payload.size(), 5);
  auto h = ParseHeader(pkt.data(), pkt.size());
  CHECK(h && h->type == PacketType::kQualityRequest);
  auto d = DecodeQualityRequest(pkt.data() + kHeaderSize, h->payload_size);
  CHECK(d && d->max_fps == 30 && d->max_bitrate_bps == 12'000'000 && d->scale_percent == 75 &&
        d->profile == Profile::kSaver);

  // Valores fuera de rango se acotan; perfil desconocido = equilibrado; escala 0 = 100 %.
  const uint8_t odd[kQualityRequestSize] = {3, 0, 1, 0, 0, 9};
  auto o = DecodeQualityRequest(odd, sizeof(odd));
  CHECK(o && o->max_fps == 10 && o->max_bitrate_bps == 1'000'000 && o->scale_percent == 100 &&
        o->profile == Profile::kBalanced);
  const uint8_t none[kQualityRequestSize] = {0, 0, 0, 0, 20, 0};
  auto n = DecodeQualityRequest(none, sizeof(none));
  CHECK(n && n->max_fps == 0 && n->max_bitrate_bps == 0 && n->scale_percent == 50);
  CHECK(!DecodeQualityRequest(none, 5));

  uint32_t w = 0, hh = 0;
  ScaledSize(2944, 1840, 75, &w, &hh);
  CHECK(w == 2208 && hh == 1380);
  ScaledSize(2944, 1840, 100, &w, &hh);
  CHECK(w == 2944 && hh == 1840);
  ScaledSize(1001, 601, 100, &w, &hh);
  CHECK(w == 1000 && hh == 600);
  ScaledSize(800, 600, 50, &w, &hh);
  CHECK(w == 640 && hh == 480);
}

int main() {
  TestAudioPacket();
  TestQualityRequest();
  TestBurstTypes();
  TestHeaderRoundtrip();
  TestHeaderRejects();
  TestRoundtripSizes();
  TestOutOfOrderAndDuplicates();
  TestLossRequestsKeyframe();
  TestGapCountsAsLoss();
  TestStreamInfoAndControl();
  TestStreamInfoExtension();
  TestHelloConfig();
  std::puts("dlp_tests: OK");
  return 0;
}
