// Pruebas de lo que decide el PC (dlp/session.h): SESSION_CONFIG, valores automáticos y ajustes solos de tasa y fps.
#include <cstdio>
#include <cstdlib>

#include "dlp/session.h"

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FALLO %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                                        \
    }                                                                      \
  } while (0)

using namespace dlp;

static void TestSessionConfigRoundtrip() {
  CHECK(static_cast<uint8_t>(PacketType::kSessionConfig) == 17);
  SessionConfig c;
  c.profile = Profile::kBalanced;
  c.flags = kSessionAudioOnTablet | kSessionBatteryAdapt;
  c.input = SessionInput::kViewOnly;
  c.reduced = Reduced::kWifi;
  c.fps = 120;
  c.bitrate_x100kbps = 412;
  c.max_bitrate_x100kbps = 520;
  const auto payload = EncodeSessionConfig(c);
  CHECK(payload.size() == kSessionConfigSize);
  // Mismos bytes que comprueba la tablet (android/.../SessionConfigTest.kt).
  CHECK((payload == std::vector<uint8_t>{1, 3, 2, 1, 120, 0, 0x9C, 0x01, 0x08, 0x02}));
  const auto pkt = MakeControlPacket(PacketType::kSessionConfig, payload.data(), payload.size(), 9);
  const auto h = ParseHeader(pkt.data(), pkt.size());
  CHECK(h && h->type == PacketType::kSessionConfig);
  const auto d = DecodeSessionConfig(pkt.data() + kHeaderSize, h->payload_size);
  CHECK(d && *d == c && d->audio_on_tablet() && d->battery_adapt());

  // Valores desconocidos se acotan; corto = nada.
  const uint8_t odd[kSessionConfigSize] = {9, 0, 7, 9, 60, 0, 0, 0, 0, 0};
  const auto o = DecodeSessionConfig(odd, sizeof(odd));
  CHECK(o && o->profile == Profile::kBalanced && o->input == SessionInput::kTouch && o->reduced == Reduced::kNone &&
        o->fps == 60 && !o->audio_on_tablet());
  CHECK(!DecodeSessionConfig(odd, kSessionConfigSize - 1));
}

static void TestAutoValues() {
  CHECK(AutoFps(144, 0) == 120);
  CHECK(AutoFps(120, 0) == 120);
  CHECK(AutoFps(90, 0) == 90);
  CHECK(AutoFps(60, 0) == 60);
  CHECK(AutoFps(0, 0) == 60);
  CHECK(AutoFps(144, 30) == 30);  // lo fijado en el PC manda

  // Idea Tab Pro (2944x1840): 26 Mbps a 60 fps y 52 a 120 en HEVC; H.264 necesita más.
  CHECK(AutoBitrateMbps(2944, 1840, 60, Codec::kHevc) == 26);
  CHECK(AutoBitrateMbps(2944, 1840, 120, Codec::kHevc) == 52);
  CHECK(AutoBitrateMbps(2944, 1840, 60, Codec::kH264) == 39);
  CHECK(AutoBitrateMbps(1280, 800, 60, Codec::kHevc) == 20);  // nunca por debajo de 20
  CHECK(AutoBitrateMbps(7680, 4320, 240, Codec::kH264) == 100);  // ni por encima de 100

  CHECK(ResolveCodec(0, 2) == Codec::kHevc);
  CHECK(ResolveCodec(0, 0) == Codec::kHevc);
  CHECK(ResolveCodec(0, 1) == Codec::kH264);  // la tablet no decodifica HEVC
  CHECK(ResolveCodec(2, 1) == Codec::kH264);  // aunque el PC pida HEVC
  CHECK(ResolveCodec(1, 2) == Codec::kH264);
}

static void TestCombineLimits() {
  // Calidad alta: lo automático tal cual, a resolución completa.
  Limits l = CombineLimits(Profile::kMaxQuality, 120, 52'000'000, false, std::nullopt);
  CHECK(l.fps == 120 && l.bitrate_bps == 52'000'000 && l.scale_percent == 100 && !l.battery_reduced);
  // Equilibrada: tope de 60 fps y 30 Mbps.
  l = CombineLimits(Profile::kBalanced, 120, 52'000'000, false, std::nullopt);
  CHECK(l.fps == 60 && l.bitrate_bps == 30'000'000);
  // Ahorro elegido a mano en el PC sí baja la escala.
  l = CombineLimits(Profile::kSaver, 60, 26'000'000, false, std::nullopt);
  CHECK(l.fps == 30 && l.bitrate_bps == 12'000'000 && l.scale_percent == 75);

  // La batería de la tablet pide Mínimo: sin «bajar si la batería no aguanta» se ignora.
  const QualityRequest minimum = ProfileDefaults(Profile::kMinimum);
  l = CombineLimits(Profile::kMaxQuality, 60, 26'000'000, false, minimum);
  CHECK(l.fps == 60 && l.bitrate_bps == 26'000'000 && l.scale_percent == 100 && !l.battery_reduced);
  // Con ello activado: bajan los fps, la tasa no baja del 60 % y la resolución no se toca nunca.
  l = CombineLimits(Profile::kMaxQuality, 60, 26'000'000, true, minimum);
  CHECK(l.fps == 30 && l.bitrate_bps == 15'600'000 && l.scale_percent == 100 && l.battery_reduced);
  // Una petición igual o mejor que la calidad del PC no cambia nada.
  l = CombineLimits(Profile::kBalanced, 60, 26'000'000, true, ProfileDefaults(Profile::kBalanced));
  CHECK(l.fps == 60 && l.bitrate_bps == 26'000'000 && !l.battery_reduced);
}

static void TestRateControllerWifi() {
  RateController r;
  uint64_t t = 1'000'000;
  r.Reset(50'000'000, false, t);
  CHECK(r.current_bps() == 25'000'000);  // por WiFi empieza por debajo (20 Mbps, pero nunca bajo la mitad)
  // Enlace limpio: sube un 10 % por segundo hasta el máximo.
  for (int i = 0; i < 20; ++i) r.OnReport(60, 0, t += 1'000'000);
  CHECK(r.current_bps() == 50'000'000 && !r.reduced());

  // Una pérdida suelta (1 de 60) no recorta: la arregla el reenvío.
  CHECK(!r.OnReport(59, 1, t += 1'000'000));
  CHECK(r.current_bps() == 50'000'000);
  // Más del 2 %: recorta un 15 %.
  CHECK(r.OnReport(50, 10, t += 1'000'000));
  CHECK(r.current_bps() == 42'500'000 && r.reduced());
  // Muchas pérdidas seguidas nunca bajan del 50 % del máximo.
  for (int i = 0; i < 30; ++i) r.OnReport(30, 30, t += 1'000'000);
  CHECK(r.current_bps() == 25'000'000);
  // Vuelve sola: en menos de 20 s limpios está otra vez arriba.
  for (int i = 0; i < 20; ++i) r.OnReport(60, 0, t += 1'000'000);
  CHECK(r.current_bps() == 50'000'000 && !r.reduced());

  // Peticiones de frame clave: como mucho un recorte cada 2 s.
  CHECK(r.OnKeyframeRequest(t += 1'000'000));
  CHECK(!r.OnKeyframeRequest(t += 500'000));
  CHECK(r.current_bps() == 42'500'000);
}

static void TestRateControllerUsbAndMax() {
  RateController r;
  uint64_t t = 1'000'000;
  r.Reset(50'000'000, true, t);
  CHECK(r.current_bps() == 50'000'000);  // por cable, al máximo desde el principio
  CHECK(!r.OnReport(10, 50, t += 1'000'000));  // y no se recorta
  CHECK(r.current_bps() == 50'000'000);

  RateController w;
  w.Reset(50'000'000, false, t);
  for (int i = 0; i < 20; ++i) w.OnReport(60, 0, t += 1'000'000);
  // Baja el máximo (otra calidad): la tasa se acota.
  CHECK(w.SetMax(30'000'000, t += 1'000'000));
  CHECK(w.current_bps() == 30'000'000 && w.min_bps() == 15'000'000);
  // Sube el máximo con el enlace limpio: va directo.
  CHECK(w.SetMax(60'000'000, t += 4'000'000));
  CHECK(w.current_bps() == 60'000'000);
}

static void TestRateControllerUsbLink() {
  RateController r;
  uint64_t t = 1'000'000;
  r.Reset(40'000'000, true, t);
  // Envío holgado: no cambia nada.
  CHECK(!r.OnLinkLoad(23'000'000, 0.4, t += 1'000'000));
  CHECK(r.current_bps() == 40'000'000);
  // El cable pasa 23 Mbps y va saturado: baja al 80 % de eso (no por debajo de la mitad del máximo).
  CHECK(r.OnLinkLoad(23'000'000, 0.95, t += 1'000'000));
  CHECK(r.current_bps() == 20'000'000 && r.reduced());
  r.Reset(26'000'000, true, t);
  CHECK(r.OnLinkLoad(23'000'000, 0.95, t += 1'000'000));
  CHECK(r.current_bps() == 18'400'000);
  // Con el enlace limpio no vuelve a subir por encima de lo que aguanta el cable.
  for (int i = 0; i < 20; ++i) {
    r.OnReport(60, 0, t += 1'000'000);
    r.OnLinkLoad(18'400'000, 0.6, t);
  }
  CHECK(r.current_bps() == 18'400'000);
  // Tras 30 s holgados prueba un 10 % más.
  for (int i = 0; i < 40; ++i) {
    r.OnReport(60, 0, t += 1'000'000);
    r.OnLinkLoad(18'400'000, 0.3, t);
  }
  CHECK(r.current_bps() > 18'400'000 && r.current_bps() <= 26'000'000);
  // Otra calidad con más tasa no se salta el tope del cable.
  CHECK(r.OnLinkLoad(23'000'000, 0.95, t += 1'000'000));
  CHECK(r.SetMax(40'000'000, t += 1'000'000));
  CHECK(r.current_bps() == 20'000'000);
}

static void TestFpsGovernor() {
  FpsGovernor g;
  g.Reset(120);
  // El codificador se lleva todos los frames: no cambia, por mucho retraso que tenga.
  for (int i = 0; i < 20; ++i) CHECK(!g.Update(120, 0));
  CHECK(g.fps() == 120 && !g.reduced());
  // Algún descarte suelto (menos del 10 %) no cuenta.
  for (int i = 0; i < 5; ++i) CHECK(!g.Update(120, 5));
  CHECK(g.fps() == 120);
  // No da abasto (descarta un 25 %) 3 s seguidos: baja a 90.
  CHECK(!g.Update(120, 30));
  CHECK(!g.Update(120, 30));
  CHECK(g.Update(120, 30));
  CHECK(g.fps() == 90 && g.reduced());
  // Escritorio quieto: no cuenta ni para bajar ni para subir.
  CHECK(!g.Update(2, 2));
  // 10 s limpios: prueba a subir a 120.
  for (int i = 0; i < 9; ++i) CHECK(!g.Update(90, 0));
  CHECK(g.Update(90, 0));
  CHECK(g.fps() == 120);
  // Vuelve a no dar abasto enseguida: baja, y la siguiente prueba espera el doble (20 s).
  CHECK(!g.Update(120, 40));
  CHECK(!g.Update(120, 40));
  CHECK(g.Update(120, 40));
  CHECK(g.fps() == 90);
  for (int i = 0; i < 19; ++i) CHECK(!g.Update(90, 0));
  CHECK(g.Update(90, 0));
  CHECK(g.fps() == 120);
  // Nunca baja de 30.
  for (int i = 0; i < 60; ++i) g.Update(100, 90);
  CHECK(g.fps() == 30);
}

int main() {
  TestSessionConfigRoundtrip();
  TestAutoValues();
  TestCombineLimits();
  TestRateControllerWifi();
  TestRateControllerUsbAndMax();
  TestRateControllerUsbLink();
  TestFpsGovernor();
  std::puts("dlp_session_tests: OK");
  return 0;
}
