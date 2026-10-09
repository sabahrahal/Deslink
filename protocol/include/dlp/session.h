// Lo que decide el PC en cada sesión y cómo se lo cuenta a la tablet. El PC es la única fuente de verdad: la tablet
// dice lo que puede hacer (HELLO) y el PC decide resolución, fps, tasa, códec, calidad, sonido y táctil, y se lo manda
// en SESSION_CONFIG. Aquí está también la lógica que ajusta la imagen sola (tasa por WiFi, fps según la carga de la
// gráfica), sin nada de Windows, para poder probarla. Ver docs/protocol-dlp.md y docs/ajustes.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "dlp/packet.h"

namespace dlp {

// --- SESSION_CONFIG (host -> cliente) ----------------------------------------------------------------------------

enum class SessionInput : uint8_t { kTouch = 0, kMouse = 1, kViewOnly = 2 };

enum SessionFlags : uint8_t {
  kSessionAudioOnTablet = 1 << 0,  // el sonido del PC suena en la tablet
  kSessionBatteryAdapt = 1 << 1,   // la tablet puede pedir bajar fps y tasa si su batería no aguanta
};

// Por qué la imagen va ahora por debajo de lo elegido (para avisar al usuario).
enum class Reduced : uint8_t { kNone = 0, kWifi = 1, kBattery = 2, kLoad = 3 };

struct SessionConfig {
  Profile profile = Profile::kMaxQuality;  // calidad elegida en el PC
  uint8_t flags = 0;
  SessionInput input = SessionInput::kTouch;
  Reduced reduced = Reduced::kNone;
  uint16_t fps = 0;                 // fps a los que va ahora
  uint16_t bitrate_x100kbps = 0;    // tasa actual
  uint16_t max_bitrate_x100kbps = 0;  // tasa elegida (el techo)

  bool audio_on_tablet() const { return flags & kSessionAudioOnTablet; }
  bool battery_adapt() const { return flags & kSessionBatteryAdapt; }
  bool operator==(const SessionConfig& o) const {
    return profile == o.profile && flags == o.flags && input == o.input && reduced == o.reduced && fps == o.fps &&
           bitrate_x100kbps == o.bitrate_x100kbps && max_bitrate_x100kbps == o.max_bitrate_x100kbps;
  }
  bool operator!=(const SessionConfig& o) const { return !(*this == o); }
};

constexpr size_t kSessionConfigSize = 10;

std::vector<uint8_t> EncodeSessionConfig(const SessionConfig& config);
// nullopt si el payload es corto. Valores desconocidos se acotan (perfil -> equilibrado, táctil -> táctil).
std::optional<SessionConfig> DecodeSessionConfig(const uint8_t* payload, size_t size);

// --- Decisiones del PC ---------------------------------------------------------------------------------------------

// fps por defecto según la pantalla de la tablet: 120 si llega a 120 Hz, 90 si llega a 90, si no 60.
// user_fps > 0 manda (lo fijado en Configuración avanzada del PC).
uint32_t AutoFps(uint16_t tablet_refresh_hz, uint32_t user_fps);

// Tasa por defecto para ese tamaño y fps: ~0,08 bits por píxel en HEVC y 0,12 en H.264, entre 20 y 100 Mbps.
uint32_t AutoBitrateMbps(uint32_t width, uint32_t height, uint32_t fps, Codec codec);

// Códec: el fijado en el PC si la tablet lo decodifica; si no, HEVC salvo que la tablet diga que solo hace H.264.
// tablet_codec: lo que manda en HelloConfig::codec (0 = no lo dice, 1 = solo H.264, 2 = también HEVC).
Codec ResolveCodec(uint8_t user_codec, uint8_t tablet_codec);

// Topes de la sesión: los de la calidad elegida en el PC, y si «bajar si la batería no aguanta» está activado, lo
// que pida la tablet. La tablet nunca cambia la resolución (la escala es siempre la de la calidad del PC) ni baja la
// tasa por debajo del 60 % de la elegida: si hay que ahorrar, se bajan antes los fps.
struct Limits {
  uint32_t fps = 60;
  uint32_t bitrate_bps = 30'000'000;
  uint8_t scale_percent = 100;
  bool battery_reduced = false;
};
Limits CombineLimits(Profile host_profile, uint32_t target_fps, uint32_t target_bitrate_bps, bool battery_adapt,
                     const std::optional<QualityRequest>& tablet);

// --- Tasa por WiFi --------------------------------------------------------------------------------------------------

// Ajusta la tasa del codificador a lo que aguanta el enlace. Baja un 15 % solo si se pierde más del 2 % de los frames
// en un segundo (o la tablet pide un frame clave, como mucho cada 2 s), nunca por debajo del 50 % del máximo, y vuelve
// a subir un 10 % por segundo tras 3 s limpios. Por cable no se pierde nada y va siempre al máximo.
class RateController {
 public:
  void Reset(uint32_t max_bps, bool usb, uint64_t now_us);
  // Informe de la tablet (frames recibidos y perdidos en el último segundo). Devuelve true si cambió la tasa.
  bool OnReport(uint32_t received, uint32_t lost, uint64_t now_us);
  bool OnKeyframeRequest(uint64_t now_us);
  // Cambia el máximo (otra calidad o batería); vuelve al máximo nuevo si el enlace va limpio.
  bool SetMax(uint32_t max_bps, uint64_t now_us);
  // Por cable no se pierden frames, pero el túnel de adb tiene un tope (en algunas tablets ~20 Mbps): si el envío
  // pasa más del 80 % del segundo ocupado, la tasa baja al 80 % de lo que pasó de verdad y eso queda de techo. Con el
  // envío holgado (menos del 50 %) durante 30 s, el techo sube un 10 % para volver a probar. Una vez por segundo.
  bool OnLinkLoad(uint32_t link_bps, double busy_fraction, uint64_t now_us);

  uint32_t current_bps() const { return current_; }
  uint32_t max_bps() const { return max_; }
  uint32_t min_bps() const { return max_ / 2; }
  // La tasa está claramente por debajo del máximo por culpa del enlace.
  bool reduced() const { return current_ < max_ * 9ull / 10; }

  static constexpr uint32_t kStartBps = 20'000'000;  // un frame clave muy grande desborda el WiFi al conectar

 private:
  bool Decrease(uint64_t now_us);

  uint32_t max_ = 30'000'000;
  uint32_t current_ = 30'000'000;
  uint32_t ceiling_ = 30'000'000;
  uint32_t link_cap_ = UINT32_MAX;  // lo que aguanta el cable (OnLinkLoad); sin medir, sin tope
  uint64_t link_idle_since_us_ = 0;
  bool usb_ = false;
  uint64_t last_decrease_us_ = 0;
  uint64_t last_increase_us_ = 0;
  uint64_t last_relax_us_ = 0;
};

// --- fps según la carga de la gráfica -------------------------------------------------------------------------------

// Mira si el codificador da abasto: cada segundo recibe cuántos frames se le mandaron y cuántos se descartaron porque
// aún estaba con el anterior. No usa el retraso del codificador: ese incluye los frames que retiene por diseño y no
// dice nada de la carga (con él se bajaba a 30 fps sin motivo).
// Si se descarta más del 10 % durante 3 s, baja un escalón (120, 90, 60, 45, 30). Tras 10 s sin descartes prueba el
// escalón de arriba; si al subir vuelve a descartar enseguida, la siguiente prueba espera el doble (hasta 160 s).
class FpsGovernor {
 public:
  void Reset(uint32_t target_fps);
  // Una vez por segundo. Devuelve true si cambian los fps.
  bool Update(uint32_t submitted, uint32_t dropped);
  uint32_t fps() const { return fps_; }
  uint32_t target() const { return target_; }
  bool reduced() const { return fps_ < target_; }

 private:
  uint32_t target_ = 60;
  uint32_t fps_ = 60;
  int slow_seconds_ = 0;
  int clean_seconds_ = 0;
  int wait_seconds_ = 10;
  int since_up_ = -1;  // segundos desde la última subida (-1: no se está probando)
};

}  // namespace dlp
