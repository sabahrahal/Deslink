// DeskLink host (fase 1): captura una salida de Windows, la codifica por hardware
// y la envía por UDP al cliente Android que haya enviado HELLO.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmsystem.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio.h"
#include "capture.h"
#include "converter.h"
#include "dlp/packet.h"
#include "dlp/session.h"
#include "encoder.h"
#include "gpu.h"
#include "frame_sender.h"
#include "history.h"
#include "touch.h"
#include "udp.h"
#include "virtual_display.h"

namespace {

std::atomic<bool> g_running{true};
std::atomic<bool> g_replaced{false};  // otra ventana del host pidió el relevo: se sale sin «Pulsa Enter»

void OnSignal(int) { g_running = false; }

// Un solo host por sesión de Windows. Si ya hay otro abierto (otra consola, la app de bandeja, uno que quedó
// minimizado), se le pide que se cierre y este ocupa su lugar: antes el segundo moría con «bind al puerto 47800
// falló: 10048» y había que buscar y cerrar el primero a mano.
class SingleInstance {
 public:
  SingleInstance() {
    quit_ = CreateEventW(nullptr, TRUE, FALSE, L"Local\\DeskLinkHost.Quit");
    mutex_ = CreateMutexW(nullptr, FALSE, L"Local\\DeskLinkHost");
    if (!mutex_) return;
    DWORD r = WaitForSingleObject(mutex_, 0);
    if (r == WAIT_TIMEOUT) {
      std::printf("Ya había otro DeskLink abierto en este PC: lo cierro y sigo con este.\n");
      if (quit_) SetEvent(quit_);
      r = WaitForSingleObject(mutex_, 15000);  // el otro quita el monitor virtual y el túnel de adb
      if (r == WAIT_TIMEOUT) std::printf("El otro DeskLink no se cerró; lo intento igualmente.\n");
    }
    owned_ = r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
    if (quit_) ResetEvent(quit_);
    if (quit_) watcher_ = std::thread([this] {
      while (g_running) {
        if (WaitForSingleObject(quit_, 200) == WAIT_OBJECT_0) {
          std::printf("Se abrió otro DeskLink: le dejo el sitio.\n");
          g_replaced = true;
          g_running = false;
        }
      }
    });
  }
  ~SingleInstance() {
    g_running = false;
    if (watcher_.joinable()) watcher_.join();
    if (owned_) ReleaseMutex(mutex_);
    if (mutex_) CloseHandle(mutex_);
    if (quit_) CloseHandle(quit_);
  }

 private:
  HANDLE quit_ = nullptr;
  HANDLE mutex_ = nullptr;
  bool owned_ = false;
  std::thread watcher_;
};

uint64_t NowUs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Duerme us microsegundos sin ocupar la CPU. Con el temporizador de alta resolución (Windows 10 1803 o
// posterior) la espera es de ~0,5 ms; si no existe, Sleep(1) con timeBeginPeriod(1).
void SleepUs(uint64_t us) {
  struct Timer {
    HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    ~Timer() {
      if (h) CloseHandle(h);
    }
  };
  thread_local Timer timer;
  LARGE_INTEGER due;
  due.QuadPart = -static_cast<LONGLONG>(us) * 10;  // relativo, en unidades de 100 ns
  if (timer.h && SetWaitableTimer(timer.h, &due, 0, nullptr, nullptr, FALSE)) {
    WaitForSingleObject(timer.h, INFINITE);
  } else {
    Sleep(static_cast<DWORD>(std::max<uint64_t>(us / 1000, 1)));
  }
}

// Envía los paquetes uno tras otro; con pace_mbps > 0 reparte el envío para no saturar
// los búferes de enlaces lentos o con ráfagas mal toleradas (p. ej. el anclaje USB).
// Los paquetes salen en ráfagas de como mucho kBurstUs y entre ráfagas el hilo duerme: antes giraba
// esperando cada paquete (a 100 Mbps hay uno cada ~0,1 ms) y gastaba un núcleo entero mientras enviaba.
template <typename SendFn>
void SendPaced(const std::vector<std::vector<uint8_t>>& packets, uint32_t pace_mbps, SendFn send) {
  constexpr uint64_t kBurstUs = 500;
  const uint64_t start = NowUs();
  uint64_t bits = 0;
  for (const auto& p : packets) {
    if (pace_mbps > 0) {
      const uint64_t due = start + bits / pace_mbps;  // bits / (Mbit/s) = µs
      const uint64_t now = NowUs();
      if (due > now + kBurstUs) SleepUs(due - now);
    }
    send(p);
    bits += p.size() * 8;
  }
}

// Ajustes que se recuerdan entre sesiones, uno por fichero en %APPDATA%\DeskLink.
std::wstring ConfigPath(const wchar_t* name) {
  wchar_t base[MAX_PATH];
  if (!GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH)) return {};
  return std::wstring(base) + L"\\DeskLink\\" + name;
}

// Resolución que el usuario dejó en Configuración de pantalla de Windows para el monitor virtual, y la tablet para la
// que la eligió (su resolución nativa). Manda sobre la de la tablet mientras sea la misma tablet: así no hay dos
// sitios que se pisen (DeskLink ya no tiene ajuste de resolución propio).
struct SavedSize {
  bool ok = false;
  UINT w = 0, h = 0;
  UINT tablet_w = 0, tablet_h = 0;  // 0: guardada por una versión anterior, sin tablet
};

std::wstring SizeFilePath() { return ConfigPath(L"virtual-size.txt"); }

SavedSize& CachedSize() {
  static SavedSize saved = [] {
    SavedSize s;
    const std::wstring path = SizeFilePath();
    FILE* f = path.empty() ? nullptr : _wfopen(path.c_str(), L"r");
    if (!f) return s;
    unsigned a = 0, b = 0, c = 0, d = 0;
    const int n = std::fscanf(f, "%ux%u %ux%u", &a, &b, &c, &d);
    std::fclose(f);
    if (n >= 2 && a >= 640 && b >= 480 && a <= 7680 && b <= 4320) {
      s = {true, a, b, n == 4 ? c : 0, n == 4 ? d : 0};
    }
    return s;
  }();
  return saved;
}

bool LoadSavedSize(UINT* w, UINT* h) {
  const SavedSize& s = CachedSize();
  if (!s.ok) return false;
  *w = s.w;
  *h = s.h;
  return true;
}

void SaveSize(UINT w, UINT h, UINT tablet_w, UINT tablet_h) {
  CachedSize() = {true, w, h, tablet_w, tablet_h};
  const std::wstring path = SizeFilePath();
  if (path.empty()) return;
  CreateDirectoryW(path.substr(0, path.find_last_of(L'\\')).c_str(), nullptr);
  if (FILE* f = _wfopen(path.c_str(), L"w")) {
    std::fprintf(f, "%ux%u %ux%u\n", w, h, tablet_w, tablet_h);
    std::fclose(f);
  }
}

// Nombre del PC en UTF-8, para que la tablet lo muestre en la lista de PCs encontrados.
std::string ComputerNameUtf8() {
  wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
  DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
  if (!GetComputerNameW(name, &n)) return {};
  const int len = WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
  std::string out(len > 0 ? len : 0, '\0');
  if (len > 0) WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(n), out.data(), len, nullptr, nullptr);
  return out;
}

// Modo del táctil: «touch» (toques reales de Windows, por defecto) o «mouse». Lo cambia la tablet en marcha
// (paquete INPUT_MODE) o la app de escritorio escribiendo el fichero; --touch/--mouse lo fuerzan solo esa vez.
std::wstring InputModePath() { return ConfigPath(L"input-mode.txt"); }

TouchInjector::Mode LoadInputMode() {
  const std::wstring path = InputModePath();
  FILE* f = path.empty() ? nullptr : _wfopen(path.c_str(), L"r");
  if (!f) return TouchInjector::Mode::kTouch;
  char buf[16] = {};
  const bool mouse = std::fscanf(f, "%15s", buf) == 1 && std::strcmp(buf, "mouse") == 0;
  std::fclose(f);
  return mouse ? TouchInjector::Mode::kMouse : TouchInjector::Mode::kTouch;
}

void SaveInputMode(TouchInjector::Mode mode) {
  const std::wstring path = InputModePath();
  if (path.empty()) return;
  CreateDirectoryW(path.substr(0, path.find_last_of(L'\\')).c_str(), nullptr);
  if (FILE* f = _wfopen(path.c_str(), L"w")) {
    std::fprintf(f, "%s\n", mode == TouchInjector::Mode::kMouse ? "mouse" : "touch");
    std::fclose(f);
  }
}

// Coordenadas en píxeles reales en todos los monitores, aunque tengan escalas distintas (la tablet suele ir al
// 150-200 %): si no, Windows escala las posiciones del monitor virtual y los toques y el cursor caen desplazados.
void SetDpiAwareness() {
  using SetContextFn = BOOL(WINAPI*)(HANDLE);
  if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
    auto set = reinterpret_cast<SetContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (set && set(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4)))) return;  // PER_MONITOR_AWARE_V2
  }
  SetProcessDPIAware();
}

// El PC es la única fuente de verdad (docs/ajustes.md): la app de bandeja pasa aquí lo que eligió el usuario y lo
// cambia en marcha por la entrada estándar (@ajustes, ver ApplySettingsLine); la tablet solo dice lo que puede hacer
// (HELLO) y el host decide con dlp/session.h.
struct Options {
  uint16_t port = dlp::kDefaultPort;
  int output = 0;
  // Lo que se usa ahora (lo decide Decide() con lo elegido y lo que dice la tablet).
  UINT fps = 60;
  UINT bitrate_mbps = 30;
  VideoCodec codec = VideoCodec::kHevc;
  // Lo elegido en el PC. 0 = automático.
  UINT user_fps = 0;       // --fps auto|N
  UINT user_bitrate = 0;   // --bitrate auto|N (Mbps)
  uint8_t user_codec = 0;  // --codec auto|hevc|h264 (0, 2, 1 como en HelloConfig)
  bool audio_tablet = false;    // --audio-tablet: el sonido del PC suena en la tablet
  bool battery_adapt = false;   // --battery-adapt: la tablet puede pedir bajar fps y tasa si su batería no aguanta
  bool cursor = true;    // --no-cursor: no dibuja el cursor en la imagen
  int input_mode = -1;   // --touch (0) / --mouse (1) / --view-only (2); si no, el guardado (táctil por defecto)
  bool touch = true;     // --no-touch desactiva el control táctil
  UINT pace_mbps = 100;  // reparte cada frame a esta tasa; 0 = de golpe (el anclaje USB pierde ráfagas)
  UINT fec_group = 10;   // --fec: por WiFi, una paridad cada tantos paquetes (10 % más de datos); 0 = sin FEC
  bool virtual_display = false;  // --virtual: crea un monitor virtual (driver desklink-idd) y captura ese
  UINT virtual_width = 2944;     // --size: resolución del monitor virtual (por defecto, la Lenovo Idea Tab Pro)
  UINT virtual_height = 1840;
  bool size_given = false;       // si no se pasa --size, se usa la última resolución recordada
  UINT native_width = 0;         // tamaño nativo de la tablet (0: no se sabe): con él se crea el monitor virtual
  UINT native_height = 0;
  bool usb = true;               // --no-usb: sin cable USB (TCP por adb reverse)
  bool open_tablet = true;       // --no-open-tablet: no abrir DeskLink en la tablet al enchufarla
  std::wstring adb_path;         // --adb: ruta de adb.exe (si no, se busca)
  UINT refine = 4;               // --refine: repeticiones extra al quedarse quieta la imagen para afinar la calidad
  bool status = false;           // --status: líneas «@...» para la app de bandeja (desklink.exe) en vez de las estadísticas
  std::wstring stop_event;       // --stop-event: evento con nombre que, al activarse, cierra el host limpiamente
  bool audio = true;             // --no-audio: nunca envía el sonido del PC (aunque la tablet lo pida)
  UINT audio_kbps = 128;         // --audio-bitrate: kbps de Opus
  // --profile alta|equilibrada|ahorro: la calidad elegida en el PC (topes de fps, tasa y escala).
  dlp::Profile profile = dlp::Profile::kMaxQuality;
};

// Límites efectivos: la calidad del PC, lo que pida la batería de la tablet (si se permite) y los fps que aguanta
// la gráfica (governor_fps, 0 = sin tope).
struct Effective {
  UINT fps;
  UINT bitrate_mbps_x10;
  uint8_t scale_percent;
  bool battery_reduced;
  UINT limit_fps;  // fps sin el tope de la gráfica (el objetivo del gobernador)
};

Effective Apply(const Options& o, const std::optional<dlp::QualityRequest>& tablet, UINT governor_fps) {
  const dlp::Limits l = dlp::CombineLimits(o.profile, o.fps, o.bitrate_mbps * 1'000'000u, o.battery_adapt, tablet);
  Effective e;
  e.limit_fps = l.fps;
  e.fps = governor_fps ? std::min<UINT>(l.fps, governor_fps) : l.fps;
  e.bitrate_mbps_x10 = l.bitrate_bps / 100'000u;
  e.scale_percent = l.scale_percent;
  e.battery_reduced = l.battery_reduced;
  return e;
}

// Tamaño con el que se crea el monitor virtual: el nativo de la tablet si la resolución elegida cabe en él. Así
// Windows ofrece siempre de la nativa hacia abajo (el driver solo lista modos menores que el del monitor) y, como el
// tamaño no cambia, lo reconoce como el mismo monitor y conserva su posición y su escala.
void MonitorSize(const Options& o, UINT* w, UINT* h) {
  const bool fits = o.native_width >= o.virtual_width && o.native_height >= o.virtual_height;
  *w = fits ? o.native_width : o.virtual_width;
  *h = fits ? o.native_height : o.virtual_height;
}

// Hz del monitor virtual. Nunca menos de 60: los fps de la captura se reparten aparte (min_interval_us), y cambiar
// los Hz obliga a quitar y volver a poner el monitor, con lo que Windows lo apaga, lo cambia de sitio y pierde modos.
UINT MonitorHz(UINT fps) { return fps > 60 ? fps : 60; }

// Lo que se usa con lo elegido y lo que dijo la tablet: fps, códec, tamaño del monitor virtual y tasa.
// capture_w x capture_h: tamaño de la captura si no hay monitor virtual (duplicar un monitor).
void Decide(Options* o, const dlp::HelloConfig& tablet, UINT capture_w, UINT capture_h) {
  o->fps = dlp::AutoFps(tablet.refresh_hz, o->user_fps);
  o->codec = dlp::ResolveCodec(o->user_codec, tablet.codec) == dlp::Codec::kHevc ? VideoCodec::kHevc : VideoCodec::kH264;
  if (o->virtual_display && !o->size_given && tablet.width >= 640 && tablet.height >= 480 && tablet.width <= 7680 &&
      tablet.height <= 4320) {
    const SavedSize& saved = CachedSize();
    const bool chosen = saved.ok && saved.tablet_w == tablet.width && saved.tablet_h == tablet.height;
    o->virtual_width = chosen ? saved.w : tablet.width;
    o->virtual_height = chosen ? saved.h : tablet.height;
    o->native_width = tablet.width;
    o->native_height = tablet.height;
  }
  if (o->size_given) o->native_width = o->native_height = 0;
  const UINT w = o->virtual_display ? o->virtual_width : capture_w;
  const UINT h = o->virtual_display ? o->virtual_height : capture_h;
  o->bitrate_mbps = o->user_bitrate ? o->user_bitrate
                                    : dlp::AutoBitrateMbps(w ? w : 2944, h ? h : 1840, o->fps,
                                                           o->codec == VideoCodec::kHevc ? dlp::Codec::kHevc
                                                                                         : dlp::Codec::kH264);
}

const char* ProfileArg(dlp::Profile p) {
  switch (p) {
    case dlp::Profile::kMaxQuality: return "alta";
    case dlp::Profile::kBalanced: return "equilibrada";
    default: return "ahorro";
  }
}

bool ParseProfile(const std::string& p, dlp::Profile* out) {
  if (p == "alta" || p == "maxima") *out = dlp::Profile::kMaxQuality;
  else if (p == "equilibrada" || p == "equilibrado") *out = dlp::Profile::kBalanced;
  else if (p == "ahorro") *out = dlp::Profile::kSaver;
  else return false;
  return true;
}

// «auto» o un número; false si no es ninguno.
bool ParseAutoNumber(const std::string& v, UINT* out) {
  if (v == "auto" || v == "0") {
    *out = 0;
    return true;
  }
  const int n = std::atoi(v.c_str());
  if (n <= 0) return false;
  *out = static_cast<UINT>(n);
  return true;
}

bool ParseCodec(const std::string& v, uint8_t* out) {
  if (v == "auto") *out = 0;
  else if (v == "hevc") *out = 2;
  else if (v == "h264") *out = 1;
  else return false;
  return true;
}

const char* InputArg(int mode) { return mode == 2 ? "solo-ver" : mode == 1 ? "raton" : "tactil"; }

std::wstring Widen(const char* v) {
  const int n = MultiByteToWideChar(CP_ACP, 0, v, -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_ACP, 0, v, -1, w.data(), n);
  return w;
}

// Todo lo que depende de la resolución y de la GPU de la salida capturada. Cuando Windows cambia la
// resolución se tira entero y se vuelve a crear. El orden de los miembros importa: el codificador se destruye
// primero y la GPU la última.
struct Pipeline {
  Gpu gpu;
  DesktopCapture capture;
  Nv12Converter converter;
  std::unique_ptr<MfEncoder> encoder;
  UINT w = 0, h = 0;    // tamaño capturado
  UINT ew = 0, eh = 0;  // tamaño codificado (menor con los perfiles de ahorro)
  uint8_t scale_percent = 100;
};

bool ParseArgs(int argc, char** argv, Options* o) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Falta el valor de %s\n", name);
        return nullptr;
      }
      return argv[++i];
    };
    if (a == "--port") {
      auto v = next("--port"); if (!v) return false;
      o->port = static_cast<uint16_t>(std::atoi(v));
    } else if (a == "--output") {
      auto v = next("--output"); if (!v) return false;
      o->output = std::atoi(v);
    } else if (a == "--fps") {
      auto v = next("--fps"); if (!v) return false;
      if (!ParseAutoNumber(v, &o->user_fps) || o->user_fps > 240) {
        std::fprintf(stderr, "--fps debe ser auto o un número de 10 a 240\n");
        return false;
      }
    } else if (a == "--bitrate") {
      auto v = next("--bitrate"); if (!v) return false;
      if (!ParseAutoNumber(v, &o->user_bitrate) || o->user_bitrate > 1000) {
        std::fprintf(stderr, "--bitrate debe ser auto o los Mbps (1 a 1000)\n");
        return false;
      }
    } else if (a == "--audio-tablet") {
      o->audio_tablet = true;
    } else if (a == "--battery-adapt") {
      o->battery_adapt = true;
    } else if (a == "--view-only") {
      o->input_mode = 2;
    } else if (a == "--fec") {
      auto v = next("--fec"); if (!v) return false;
      o->fec_group = static_cast<UINT>(std::atoi(v));
    } else if (a == "--pace") {
      auto v = next("--pace"); if (!v) return false;
      o->pace_mbps = static_cast<UINT>(std::atoi(v));
    } else if (a == "--virtual") {
      o->virtual_display = true;
    } else if (a == "--size") {
      auto v = next("--size"); if (!v) return false;
      unsigned w = 0, h = 0;
      if (std::sscanf(v, "%ux%u", &w, &h) != 2 || w < 640 || h < 480 || w > 7680 || h > 4320) {
        std::fprintf(stderr, "--size debe ser ANCHOxALTO, p. ej. 2944x1840\n");
        return false;
      }
      o->virtual_width = w;
      o->virtual_height = h;
      o->size_given = true;
    } else if (a == "--no-usb") {
      o->usb = false;
    } else if (a == "--no-open-tablet") {
      o->open_tablet = false;
    } else if (a == "--adb") {
      auto v = next("--adb"); if (!v) return false;
      o->adb_path = Widen(v);
    } else if (a == "--status") {
      o->status = true;
    } else if (a == "--stop-event") {
      auto v = next("--stop-event"); if (!v) return false;
      o->stop_event = Widen(v);
    } else if (a == "--profile") {
      auto v = next("--profile"); if (!v) return false;
      if (!ParseProfile(v, &o->profile)) {
        std::fprintf(stderr, "--profile debe ser alta, equilibrada o ahorro\n");
        return false;
      }
    } else if (a == "--refine") {
      auto v = next("--refine"); if (!v) return false;
      o->refine = static_cast<UINT>(std::atoi(v));
    } else if (a == "--no-cursor") {
      o->cursor = false;
    } else if (a == "--mouse") {
      o->input_mode = 1;
    } else if (a == "--touch") {
      o->input_mode = 0;
    } else if (a == "--no-touch") {
      o->touch = false;
    } else if (a == "--no-audio") {
      o->audio = false;
    } else if (a == "--audio-bitrate") {
      auto v = next("--audio-bitrate"); if (!v) return false;
      o->audio_kbps = static_cast<UINT>(std::atoi(v));
    } else if (a == "--codec") {
      auto v = next("--codec"); if (!v) return false;
      if (!ParseCodec(v, &o->user_codec)) {
        std::fprintf(stderr, "--codec debe ser auto, hevc o h264\n");
        return false;
      }
    } else {
      std::fprintf(stderr,
                   "Uso: desklink-host [--port 47800] [--output 0] [--fps auto|60] [--bitrate auto|50] "
                   "[--codec auto|hevc|h264] [--profile alta|equilibrada|ahorro] [--audio-tablet] [--battery-adapt] "
                   "[--pace Mbps] [--fec 10] [--refine 4] [--no-touch] [--touch|--mouse|--view-only] [--no-cursor] [--no-audio] "
                   "[--audio-bitrate 128] [--virtual [--size 2944x1840]] [--no-usb] [--no-open-tablet] [--adb ruta\\adb.exe] [--status] "
                   "[--stop-event nombre]\n");
      return false;
    }
  }
  return o->refine <= 30 && o->audio_kbps >= 32 && o->audio_kbps <= 256 && o->fec_group <= 64;
}

// Una línea «@ajustes clave=valor ...» de la app de bandeja (entrada estándar): lo que el usuario cambió en la
// ventana del PC con la transmisión en marcha. Devuelve false si no es una línea de ajustes o trae algo inválido.
bool ApplySettingsLine(const std::string& line, Options* o) {
  if (line.rfind("@ajustes", 0) != 0) return false;
  Options n = *o;
  size_t pos = 8;
  while (pos < line.size()) {
    while (pos < line.size() && line[pos] == ' ') ++pos;
    size_t end = line.find(' ', pos);
    if (end == std::string::npos) end = line.size();
    const std::string kv = line.substr(pos, end - pos);
    pos = end;
    const size_t eq = kv.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
    bool ok = true;
    if (k == "calidad") ok = ParseProfile(v, &n.profile);
    else if (k == "fps") ok = ParseAutoNumber(v, &n.user_fps) && n.user_fps <= 240;
    else if (k == "mbps") ok = ParseAutoNumber(v, &n.user_bitrate) && n.user_bitrate <= 1000;
    else if (k == "codec") ok = ParseCodec(v, &n.user_codec);
    else if (k == "sonido") n.audio_tablet = v == "tablet";
    else if (k == "bateria") n.battery_adapt = v == "1";
    else if (k == "tactil") n.input_mode = v == "raton" ? 1 : v == "solo-ver" ? 2 : 0;
    else if (k == "abrir_tablet") n.open_tablet = v == "1";
    else if (k == "size") {
      unsigned w = 0, h = 0;
      if (v == "auto") {
        n.size_given = false;
      } else if (std::sscanf(v.c_str(), "%ux%u", &w, &h) == 2 && w >= 640 && h >= 480 && w <= 7680 && h <= 4320) {
        n.size_given = true;
        n.virtual_width = w;
        n.virtual_height = h;
      } else {
        ok = false;
      }
    }
    if (!ok) return false;
  }
  *o = n;
  return true;
}

}  // namespace

int Run(int argc, char** argv) {
  Options opt;
  if (!ParseArgs(argc, argv, &opt)) return 2;
  SetConsoleOutputCP(CP_UTF8);  // los mensajes llevan tildes
  SingleInstance instance;  // se destruye lo último: el siguiente host entra cuando este ya soltó todo

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  // La app de bandeja lee la salida por una tubería: sin búfer, cada línea le llega en cuanto se escribe.
  if (opt.status) std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (!opt.stop_event.empty()) {
    if (HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, opt.stop_event.c_str())) {
      std::thread([ev] {
        WaitForSingleObject(ev, INFINITE);
        g_running = false;
      }).detach();
    }
  }

  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  MFStartup(MF_VERSION);
  timeBeginPeriod(1);  // temporizadores de 1 ms
  SetDpiAwareness();  // coordenadas en píxeles reales, para que los toques caigan donde toca

  VirtualDisplay virtual_display;  // se declara antes que el resto: el monitor se quita lo último
  std::unique_ptr<Pipeline> pipe;
  UdpEndpoint udp;
  AdbReverse adb;
  AudioCapture audio;  // después de udp: se destruye antes (envía por él)
  int exit_code = 1;

  std::atomic<uint32_t> next_frame_id{1};
  std::atomic<uint64_t> bytes_sent{0}, frames_encoded{0}, encode_latency_us{0}, keyframes_sent{0}, retransmits{0}, touch_packets{0};
  std::atomic<uint64_t> packets_sent{0};
  // El envío se atrasó y tiró frames: la tasa lo lee con el siguiente informe de la tablet (RateController).
  std::atomic<bool> send_backlog{false};
  // Para --status: último informe de la tablet y RTT medido con PING del host (0 = la tablet no contesta).
  std::atomic<uint32_t> tablet_received{0}, tablet_lost{0};
  std::atomic<int> tablet_decoded{-1};  // frames que mostró la tablet el último segundo (-1: tablet antigua)
  std::string last_client_info;  // lo último que dijo CLIENT_INFO (solo lo toca el hilo que recibe)
  std::atomic<int> tablet_decode_x10{-1};  // décimas de ms que tarda la tablet en decodificar cada frame (-1: no lo dice)
  // Retardo por etapas que mide la tablet (RATE_REPORT de tablets nuevas), en décimas de ms; 0 = no lo sabe. Ver
  // LatencyStats en la app: total = de entrar al codificador a verse en pantalla; llegar = hasta tener el frame
  // entero en la tablet; reenvíos = esperando paquetes reclamados; pantalla = de decodificado a encendido.
  std::atomic<uint32_t> tablet_total_x10{0}, tablet_arrive_x10{0}, tablet_hold_x10{0}, tablet_screen_x10{0},
      tablet_worst_x10{0};
  std::atomic<bool> peer_tether{false};  // la tablet llega por su anclaje USB (red sobre el cable)
  std::atomic<uint64_t> rtt_us{0};
  PacketHistory history;
  TouchInjector touch;
  bool touch_ready = false;

  // El codificador lo toca también el hilo de recepción (tasa, frames clave): su puntero se publica
  // bajo este cerrojo y se retira antes de destruirlo.
  std::mutex enc_mutex;
  MfEncoder* enc_ptr = nullptr;
  auto enc_bitrate = [&](uint32_t bps) {
    std::lock_guard<std::mutex> lock(enc_mutex);
    if (enc_ptr) enc_ptr->SetBitrate(bps);
  };
  auto enc_keyframe = [&]() {
    std::lock_guard<std::mutex> lock(enc_mutex);
    if (enc_ptr) enc_ptr->RequestKeyframe();
  };
  std::mutex info_mutex;
  std::vector<uint8_t> info_payload;
  dlp::StreamInfo info_current;  // lo que dice info_payload (bajo info_mutex)
  const std::string host_name = ComputerNameUtf8();

  // Topes de la sesión (Apply): la calidad elegida en el PC, lo que pida la batería de la tablet (solo con
  // --battery-adapt) y los fps que aguanta la gráfica (fps_gov). Lo escribe el hilo de recepción (petición de la
  // batería) y el bucle principal (ajustes nuevos, gobernador); los campos de opt que lo deciden se cambian bajo
  // el mismo mutex.
  std::mutex quality_mutex;
  std::optional<dlp::QualityRequest> tablet_request;
  dlp::FpsGovernor fps_gov;
  std::atomic<uint64_t> min_interval_us{1'000'000ull / 60};
  std::atomic<bool> rescale_requested{false};
  auto current_quality = [&]() {
    std::lock_guard<std::mutex> lock(quality_mutex);
    return Apply(opt, tablet_request, fps_gov.fps());
  };

  // Lo que la tablet dice de sí misma en su HELLO (pantalla, Hz, códecs) y los ajustes que la app de bandeja manda
  // en marcha (@ajustes por la entrada estándar). El hilo que los recibe los deja aquí y el bucle principal decide
  // y, si hace falta, rehace el pipeline; mientras tanto no se responde a los HELLO para que la tablet no arranque
  // con el tamaño viejo.
  std::mutex config_mutex;
  dlp::HelloConfig requested_config;
  std::vector<std::string> pending_settings;
  std::atomic<uint32_t> config_seq{0};
  std::atomic<bool> reconfiguring{false};
  std::atomic<bool> rate_reset{false};
  std::atomic<bool> view_only{opt.input_mode == 2};
  std::atomic<uint32_t> rate_bps{0};
  std::atomic<bool> rate_reduced{false};
  if (opt.status) {
    // Ajustes en vivo desde la ventana del PC. Sin la app (consola) la entrada no trae nada y el hilo termina.
    std::thread([&config_mutex, &pending_settings, &config_seq] {
      HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
      std::string line;
      char buf[512];
      DWORD n = 0;
      while (g_running && in && in != INVALID_HANDLE_VALUE && ReadFile(in, buf, sizeof(buf), &n, nullptr) && n > 0) {
        for (DWORD i = 0; i < n; ++i) {
          if (buf[i] == '\r') continue;
          if (buf[i] != '\n') {
            line += buf[i];
            continue;
          }
          {
            std::lock_guard<std::mutex> lock(config_mutex);
            pending_settings.push_back(line);
          }
          ++config_seq;
          line.clear();
        }
      }
    }).detach();
  }

  // Los frames se envían desde su propio hilo (FrameSender): un envío lento ya no frena al codificador.
  FrameSender sender;
  sender.Start([&](const FrameSender::Frame& f) {
    auto packets = dlp::PacketizeFrame(f.data.data(), f.data.size(), next_frame_id++, f.keyframe, f.timestamp_us);
    for (const auto& pkt : packets) {
      auto ph = dlp::ParseHeader(pkt.data(), pkt.size());
      if (ph) history.Put(ph->frame_id, ph->packet_index, pkt);
    }
    if (udp.peer_is_usb()) {
      udp.SendBatchToPeer(packets);  // por cable no hay pérdidas: TCP ya regula el ritmo, sin --pace
    } else {
      // Por WiFi (o anclaje) una paridad por grupo: la tablet rehace un paquete perdido sin pedirlo. Los reenvíos
      // siguen para cuando se pierden dos del mismo grupo (o la paridad); el historial guarda solo los de vídeo.
      dlp::AddFec(&packets, static_cast<uint8_t>(opt.fec_group));
      SendPaced(packets, opt.pace_mbps, [&](const std::vector<uint8_t>& p) { udp.SendToPeer(p); });
    }
    bytes_sent += f.data.size();
    packets_sent += packets.size();
    if (f.keyframe) ++keyframes_sent;
  });

  auto on_encoded = [&](const uint8_t* data, size_t size, bool keyframe, uint64_t ts_us) {
    ++frames_encoded;
    const uint64_t now = NowUs();
    if (now > ts_us) encode_latency_us += now - ts_us;
    // El envío va más de 4 frames atrasado: se tiran y se pide un frame clave.
    if (sender.Push(data, size, keyframe, ts_us)) {
      send_backlog = true;
      enc_keyframe();
    }
  };

  // SESSION_CONFIG: lo que eligió el PC y por qué la imagen va por debajo, si es así (ver dlp/session.h).
  // Se manda tras STREAM_INFO, cuando cambia algo y cada 2 s.
  dlp::SessionConfig last_session{};
  std::mutex session_mutex;
  auto make_session = [&]() {
    dlp::SessionConfig c;
    std::lock_guard<std::mutex> lock(quality_mutex);
    const Effective eff = Apply(opt, tablet_request, fps_gov.fps());
    c.profile = opt.profile;
    c.flags = (opt.audio && opt.audio_tablet ? dlp::kSessionAudioOnTablet : 0) |
              (opt.battery_adapt ? dlp::kSessionBatteryAdapt : 0);
    c.input = view_only ? dlp::SessionInput::kViewOnly
              : touch_ready && touch.requested() == TouchInjector::Mode::kMouse ? dlp::SessionInput::kMouse
                                                                                 : dlp::SessionInput::kTouch;
    c.reduced = eff.battery_reduced ? dlp::Reduced::kBattery
                : fps_gov.reduced()  ? dlp::Reduced::kLoad
                : rate_reduced       ? dlp::Reduced::kWifi
                                     : dlp::Reduced::kNone;
    c.fps = static_cast<uint16_t>(eff.fps);
    const uint32_t now_bps = rate_bps.load() ? std::min<uint32_t>(rate_bps.load(), eff.bitrate_mbps_x10 * 100'000u)
                                             : eff.bitrate_mbps_x10 * 100'000u;
    c.bitrate_x100kbps = static_cast<uint16_t>(now_bps / 100'000u);
    c.max_bitrate_x100kbps = static_cast<uint16_t>(eff.bitrate_mbps_x10);
    return c;
  };
  // force: mandar aunque no haya cambiado (la tablet acaba de saludar o toca la repetición periódica).
  auto send_session = [&](const sockaddr_in* to, bool force) {
    const dlp::SessionConfig c = make_session();
    {
      std::lock_guard<std::mutex> lock(session_mutex);
      if (!force && c == last_session) return;
      last_session = c;
    }
    const auto payload = dlp::EncodeSessionConfig(c);
    const auto pkt = dlp::MakeControlPacket(dlp::PacketType::kSessionConfig, payload.data(), payload.size(), NowUs());
    if (to) {
      udp.SendTo(*to, pkt);
    } else if (udp.has_peer()) {
      udp.SendToPeer(pkt);
    }
  };

  // Crea la GPU, la captura, el conversor y el codificador para el tamaño actual de la salida.
  auto build = [&](int capture_attempts) -> std::unique_ptr<Pipeline> {
    auto p = std::make_unique<Pipeline>();
    if (opt.virtual_display) {
      if (!CreateGpuForDeviceName(virtual_display.gdi_name().c_str(), &p->gpu)) return nullptr;
    } else if (!CreateGpuForOutput(opt.output, &p->gpu)) {
      return nullptr;
    }
    // Un monitor recién activado (o recién cambiado de modo) puede tardar en dejarse capturar.
    bool captured_ok = false;
    for (int i = 0; i < capture_attempts && g_running && !(captured_ok = p->capture.Init(p->gpu)); ++i) Sleep(300);
    if (!captured_ok) return nullptr;
    p->w = p->capture.width();
    p->h = p->capture.height();
    const Effective eff = current_quality();
    uint32_t ew = 0, eh = 0;
    dlp::ScaledSize(p->w, p->h, eff.scale_percent, &ew, &eh);
    p->ew = ew;
    p->eh = eh;
    p->scale_percent = eff.scale_percent;
    if (ew != p->w || eh != p->h) {
      std::printf("Captura: %ux%u, se codifica a %ux%u (%u %%) a %u fps, %.1f Mbps, %s\n", p->w, p->h, ew, eh,
                  eff.scale_percent, eff.fps, eff.bitrate_mbps_x10 / 10.0, opt.codec == VideoCodec::kHevc ? "HEVC" : "H.264");
    } else {
      std::printf("Captura: %ux%u a %u fps, %.1f Mbps, %s\n", p->w, p->h, eff.fps, eff.bitrate_mbps_x10 / 10.0,
                  opt.codec == VideoCodec::kHevc ? "HEVC" : "H.264");
    }
    if (!p->converter.Init(p->gpu, p->w, p->h, ew, eh, 24)) return nullptr;
    // En el monitor virtual Windows ya pinta el cursor dentro de la imagen (el driver no tiene cursor por hardware),
    // y a la escala de ese monitor: dibujarlo otra vez salía doble, uno grande y otro pequeño desplazado.
    // Desktop Duplication solo lo deja fuera en un monitor de verdad.
    p->converter.SetDrawCursor(opt.cursor && !opt.virtual_display);
    p->encoder = std::make_unique<MfEncoder>();
    // El codificador se crea a los fps del host: con menos fps simplemente recibe menos frames (como con el
    // escritorio quieto), así un cambio de perfil no obliga a rehacerlo.
    if (!p->encoder->Init(p->gpu, opt.codec, ew, eh, opt.fps, eff.bitrate_mbps_x10 * 100'000u, on_encoded)) {
      std::fprintf(stderr, "Error: no se pudo iniciar el codificador (arriba está el motivo).\n");
      return nullptr;
    }
    dlp::StreamInfo info;
    info.width = static_cast<uint16_t>(ew);
    info.height = static_cast<uint16_t>(eh);
    info.fps = static_cast<uint16_t>(eff.fps);
    info.codec = opt.codec == VideoCodec::kHevc ? dlp::Codec::kHevc : dlp::Codec::kH264;
    info.host_flags = dlp::kHostAcceptsHelloConfig;
    info.host_name = host_name;
    {
      std::lock_guard<std::mutex> lock(info_mutex);
      info_payload = dlp::EncodeStreamInfo(info);
      info_current = info;
    }
    return p;
  };

  do {
    if (opt.virtual_display) {
      if (!opt.size_given && LoadSavedSize(&opt.virtual_width, &opt.virtual_height)) {
        std::printf("Resolución recordada del monitor virtual: %ux%u.\n", opt.virtual_width, opt.virtual_height);
        // La tablet para la que se eligió: el monitor se crea con su tamaño nativo antes de que salude.
        opt.native_width = CachedSize().tablet_w;
        opt.native_height = CachedSize().tablet_h;
      }
    }
    // Hasta que la tablet salude: 60 fps, el códec elegido (HEVC en automático) y la tasa para ese tamaño.
    {
      std::lock_guard<std::mutex> lock(quality_mutex);
      Decide(&opt, dlp::HelloConfig{}, 0, 0);
      fps_gov.Reset(Apply(opt, tablet_request, 0).limit_fps);
      min_interval_us = 1'000'000ull / Apply(opt, tablet_request, fps_gov.fps()).fps;
    }
    std::printf("Calidad %s · %s · sonido en %s · táctil: %s%s.\n", ProfileArg(opt.profile),
                opt.user_fps ? "fps fijos" : "fps automáticos", opt.audio_tablet ? "la tablet" : "el PC",
                InputArg(opt.input_mode < 0 ? 0 : opt.input_mode), opt.battery_adapt ? " · baja si la batería no aguanta" : "");
    if (opt.virtual_display) {
      UINT mw = 0, mh = 0;
      MonitorSize(opt, &mw, &mh);
      if (!virtual_display.Plug(mw, mh, MonitorHz(opt.fps), opt.virtual_width, opt.virtual_height)) break;
    }
    pipe = build(10);
    if (!pipe) break;
    if (!opt.virtual_display) {
      std::lock_guard<std::mutex> lock(quality_mutex);
      Decide(&opt, dlp::HelloConfig{}, pipe->w, pipe->h);  // la tasa automática, para el tamaño del monitor
    }
    {
      std::lock_guard<std::mutex> lock(enc_mutex);
      enc_ptr = pipe->encoder.get();
    }

    std::atomic<uint32_t> last_client_ip{0};
    std::atomic<uint64_t> last_keyframe_us{0};
    // Tasa según el enlace (dlp::RateController): por WiFi baja un 15 % si se pierde más del 2 %, nunca por debajo
    // de la mitad del máximo, y vuelve sola en segundos. Solo se toca desde el hilo de recepción; el máximo es el
    // de los topes de la sesión y rate_reset avisa de que hay que releerlo.
    dlp::RateController rate;
    rate.Reset(current_quality().bitrate_mbps_x10 * 100'000u, true, NowUs());
    // Lo que pasa de verdad por el enlace desde el último informe de la tablet (solo por cable, ver OnLinkLoad).
    uint64_t link_last_us = NowUs(), link_last_busy = sender.busy_us, link_last_bytes = sender.sent_bytes;
    uint64_t quality_last_sent = packets_sent, quality_last_resent = retransmits;
    auto publish_rate = [&](const char* why) {
      enc_bitrate(rate.current_bps());
      rate_bps = rate.current_bps();
      rate_reduced = rate.reduced();
      if (why) std::printf("Tasa adaptada a %.1f Mbps (%s)\n", rate.current_bps() / 1e6, why);
      send_session(nullptr, false);
    };
    // Dice a la tablet qué modo de táctil se pidió y cuál se usa (distintos si Windows no aceptó los toques).
    auto send_input_mode = [&](const sockaddr_in* to) {
      if (!touch_ready) return;
      const uint8_t p[dlp::kInputModeStateSize] = {static_cast<uint8_t>(touch.requested()),
                                                  static_cast<uint8_t>(touch.active())};
      const auto pkt = dlp::MakeControlPacket(dlp::PacketType::kInputMode, p, sizeof(p), NowUs());
      if (to) {
        udp.SendTo(*to, pkt);
      } else {
        udp.SendToPeer(pkt);
      }
    };
    auto on_packet = [&](const uint8_t* data, size_t size, const sockaddr_in& from) {
      auto header = dlp::ParseHeader(data, size);
      if (!header) return;
      if (rate_reset.exchange(false)) {
        // Cambió la tasa elegida o el codificador es nuevo: se vuelve a empezar con el máximo nuevo.
        rate.Reset(current_quality().bitrate_mbps_x10 * 100'000u, udp.peer_is_usb(), NowUs());
        publish_rate(nullptr);
      }
      switch (header->type) {
        case dlp::PacketType::kDiscover: {
          // Búsqueda desde la tablet: se presenta (resolución, nombre) sin tomarla como destino del vídeo.
          std::lock_guard<std::mutex> lock(info_mutex);
          udp.SendTo(from, dlp::MakeControlPacket(dlp::PacketType::kStreamInfo, info_payload.data(),
                                                  info_payload.size(), header->timestamp_us));
          break;
        }
        case dlp::PacketType::kHello:
          if (auto config = dlp::DecodeHelloConfig(data + dlp::kHeaderSize, header->payload_size)) {
            std::lock_guard<std::mutex> lock(config_mutex);
            if (*config != requested_config) {
              requested_config = *config;
              reconfiguring = true;
              ++config_seq;
            }
          }
          if (last_client_ip.exchange(from.sin_addr.s_addr) != from.sin_addr.s_addr) {
            const bool usb = UdpEndpoint::IsUsb(from);
            peer_tether = !usb && IsUsbTetherAddress(from.sin_addr);
            if (usb) {
              std::printf("Cliente conectado por cable USB.\n");
            } else if (peer_tether) {
              std::printf("Cliente conectado por el anclaje USB de la tablet.\n");
            } else {
              char ip[INET_ADDRSTRLEN] = {};
              inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
              std::printf("Cliente conectado: %s:%u\n", ip, ntohs(from.sin_port));
            }
            {
              // Cliente nuevo: lo que pidiera la batería de otra tablet no vale para esta.
              std::lock_guard<std::mutex> lock(quality_mutex);
              tablet_request.reset();
            }
            // Por cable no se pierden paquetes: se empieza ya a la tasa máxima.
            rate.Reset(current_quality().bitrate_mbps_x10 * 100'000u, usb, NowUs());
            publish_rate(nullptr);
          }
          udp.SetPeer(from);
          if (!reconfiguring) {
            {
              std::lock_guard<std::mutex> lock(info_mutex);
              udp.SendTo(from, dlp::MakeControlPacket(dlp::PacketType::kStreamInfo, info_payload.data(),
                                                      info_payload.size(), NowUs()));
            }
            send_session(&from, true);
          }
          send_input_mode(&from);
          last_keyframe_us = NowUs();
          enc_keyframe();
          break;
        case dlp::PacketType::kInputMode: {
          if (!touch_ready || header->payload_size < dlp::kInputModeRequestSize) break;
          const auto mode = data[dlp::kHeaderSize] == static_cast<uint8_t>(dlp::InputMode::kMouse)
                                ? TouchInjector::Mode::kMouse
                                : TouchInjector::Mode::kTouch;
          if (mode != touch.requested() || view_only) {
            touch.SetMode(mode);
            SaveInputMode(mode);
            view_only = false;
            std::printf("La tablet cambió el táctil a %s.\n", touch.active_name());
            // La app de bandeja guarda el cambio: el valor es uno solo, el del PC.
            if (opt.status) std::printf("@ajuste tactil=%s\n", mode == TouchInjector::Mode::kMouse ? "raton" : "tactil");
          }
          send_input_mode(&from);
          send_session(&from, true);
          break;
        }
        case dlp::PacketType::kKeyframeRequest: {
          // Un frame clave es varias veces más grande que uno normal: si la red pierde paquetes y
          // se piden uno tras otro, la propia petición mantiene la imagen congelada. Se limita a 1/s.
          const uint64_t now = NowUs();
          if (rate.OnKeyframeRequest(now)) publish_rate("la tablet pide frame clave");
          if (now - last_keyframe_us.load() >= 1'000'000) {
            last_keyframe_us = now;
            enc_keyframe();
          }
          break;
        }
        case dlp::PacketType::kRateReport: {
          if (header->payload_size < dlp::kRateReportSize) break;
          const uint8_t* q = data + dlp::kHeaderSize;
          const uint32_t lost = q[2] | (q[3] << 8);
          tablet_received = q[0] | (q[1] << 8);
          tablet_lost = lost;
          // Desde la 0.2.x la tablet añade los frames que sacó su decodificador.
          tablet_decoded = header->payload_size >= dlp::kRateReportSize + 2 ? (q[4] | (q[5] << 8)) : -1;
          // Desde la 0.2.46 también lo que tarda en decodificar cada frame (0 = no mostró ninguno).
          tablet_decode_x10 = header->payload_size >= dlp::kRateReportSize + 4 && (q[6] | q[7])
                                  ? (q[6] | (q[7] << 8))
                                  : -1;
          // Tablets nuevas: el retardo por etapas (con la hora del PC que va en el PONG).
          if (header->payload_size >= dlp::kRateReportLatencySize) {
            tablet_total_x10 = q[8] | (q[9] << 8);
            tablet_arrive_x10 = q[10] | (q[11] << 8);
            tablet_hold_x10 = q[12] | (q[13] << 8);
            tablet_screen_x10 = q[14] | (q[15] << 8);
            tablet_worst_x10 = q[16] | (q[17] << 8);
          }
          {
            // Por WiFi también se recorta si se reenvía mucho o el envío se atrasa, aunque no se pierdan frames.
            const uint64_t sent = packets_sent.load(), resent = retransmits.load();
            const bool backlog = send_backlog.exchange(false);
            if (rate.OnLinkQuality(static_cast<uint32_t>(std::min<uint64_t>(sent - quality_last_sent, UINT32_MAX)),
                                   static_cast<uint32_t>(std::min<uint64_t>(resent - quality_last_resent, UINT32_MAX)),
                                   backlog, NowUs())) {
              publish_rate(backlog ? "el envío se atrasa" : "muchos reenvíos");
            }
            quality_last_sent = sent;
            quality_last_resent = resent;
          }
          if (rate.OnReport(tablet_received, lost, NowUs())) {
            publish_rate(lost ? "la tablet pierde frames" : "enlace limpio");
          }
          {
            const uint64_t now = NowUs();
            const uint64_t busy = sender.busy_us - link_last_busy;
            const uint64_t bytes = sender.sent_bytes - link_last_bytes;
            const uint64_t span = now - link_last_us;
            link_last_us = now;
            link_last_busy += busy;
            link_last_bytes += bytes;
            if (udp.peer_is_usb() && span > 0 && busy > 0 &&
                rate.OnLinkLoad(static_cast<uint32_t>(std::min<uint64_t>(bytes * 8'000'000 / busy, UINT32_MAX)),
                                static_cast<double>(busy) / span, now)) {
              publish_rate("el cable no da para más");
            }
          }
          break;
        }
        case dlp::PacketType::kClientInfo: {
          // Qué tablet y qué decodificador: para leer el log sin tener que preguntar. Se repite; se escribe al cambiar.
          std::string text(reinterpret_cast<const char*>(data + dlp::kHeaderSize), header->payload_size);
          for (char& c : text) {
            if (static_cast<unsigned char>(c) < 0x20) c = ' ';
          }
          if (text.rfind("Sonido", 0) == 0) {
            std::printf("%s\n", text.c_str());  // lo que oye la tablet: cambia cada vez, se escribe siempre
          } else if (text != last_client_info) {
            last_client_info = text;
            std::printf("Tablet: %s\n", text.c_str());
          }
          break;
        }
        case dlp::PacketType::kQualityRequest: {
          // Solo cuenta con «bajar si la batería no aguanta» activado en el PC (CombineLimits lo ignora si no).
          auto q = dlp::DecodeQualityRequest(data + dlp::kHeaderSize, header->payload_size);
          if (!q) break;
          Effective before, after;
          {
            std::lock_guard<std::mutex> lock(quality_mutex);
            before = Apply(opt, tablet_request, fps_gov.fps());
            const bool same = tablet_request && q->max_fps == tablet_request->max_fps &&
                              q->max_bitrate_bps == tablet_request->max_bitrate_bps &&
                              q->scale_percent == tablet_request->scale_percent && q->profile == tablet_request->profile;
            tablet_request = *q;
            after = Apply(opt, tablet_request, fps_gov.fps());
            if (same) break;  // la tablet lo repite cada pocos segundos
            fps_gov.Reset(after.limit_fps);
            after = Apply(opt, tablet_request, fps_gov.fps());
          }
          if (after.fps != before.fps || after.bitrate_mbps_x10 != before.bitrate_mbps_x10) {
            std::printf("Batería de la tablet: %s · %u fps · %.1f Mbps\n", dlp::ProfileName(q->profile), after.fps,
                        after.bitrate_mbps_x10 / 10.0);
          }
          if (after.bitrate_mbps_x10 != before.bitrate_mbps_x10 &&
              rate.SetMax(after.bitrate_mbps_x10 * 100'000u, NowUs())) {
            publish_rate(nullptr);
          }
          send_session(nullptr, false);
          if (after.fps != before.fps) {
            min_interval_us = 1'000'000ull / after.fps;
            // Se avisa a la tablet (ajusta la frecuencia de su pantalla); el decodificador no se rehace.
            std::lock_guard<std::mutex> lock(info_mutex);
            info_current.fps = static_cast<uint16_t>(after.fps);
            info_payload = dlp::EncodeStreamInfo(info_current);
            udp.SendToPeer(dlp::MakeControlPacket(dlp::PacketType::kStreamInfo, info_payload.data(),
                                                  info_payload.size(), NowUs()));
          }
          if (after.scale_percent != before.scale_percent) rescale_requested = true;
          break;
        }
        case dlp::PacketType::kTouch: {
          if (!touch_ready || view_only || header->payload_size < 1) break;
          const uint8_t* q = data + dlp::kHeaderSize;
          const size_t n = std::min<size_t>({q[0], dlp::kMaxTouchContacts,
                                              (header->payload_size - 1u) / dlp::kTouchContactSize});
          std::vector<TouchInjector::Contact> contacts;
          for (size_t i = 0; i < n; ++i) {
            const uint8_t* c = q + 1 + i * dlp::kTouchContactSize;
            TouchInjector::Contact tc;
            tc.id = c[0];
            tc.x = (c[1] | (c[2] << 8)) / 65535.0f;
            tc.y = (c[3] | (c[4] << 8)) / 65535.0f;
            tc.pressure = static_cast<uint16_t>(c[5] | (c[6] << 8));
            contacts.push_back(tc);
          }
          if (touch_packets++ == 0) std::printf("Primer toque recibido de la tablet.\n");
          touch.Update(contacts);
          break;
        }
        case dlp::PacketType::kAudioControl:
          break;  // las tablets de antes pedían aquí el sonido; ahora se elige en el PC (--audio-tablet)
        case dlp::PacketType::kNack: {
          // La tablet reclama paquetes concretos de un frame: se reenvían (rápido y sin esperar a un frame clave).
          const uint8_t* q = data + dlp::kHeaderSize;
          const size_t n = std::min<size_t>(header->payload_size / 2, 128);
          std::vector<std::vector<uint8_t>> resend;
          for (size_t i = 0; i < n; ++i) {
            std::vector<uint8_t> pkt;
            const uint16_t index = static_cast<uint16_t>(q[2 * i] | (q[2 * i + 1] << 8));
            if (history.Get(header->frame_id, index, &pkt)) resend.push_back(std::move(pkt));
          }
          if (!resend.empty()) {
            SendPaced(resend, opt.pace_mbps, [&](const std::vector<uint8_t>& p) { udp.SendToPeer(p); });
            retransmits += resend.size();
          }
          break;
        }
        case dlp::PacketType::kBurstRequest: {
          // Ráfaga de prueba: count paquetes de size bytes, opcionalmente con tasa limitada.
          if (header->payload_size < dlp::kBurstRequestSize) break;
          const uint8_t* q = data + dlp::kHeaderSize;
          const uint32_t count = std::min<uint32_t>(std::max<uint32_t>(q[0] | (q[1] << 8), 1), 2000);
          const uint32_t size = std::min<uint32_t>(std::max<uint32_t>(q[2] | (q[3] << 8), 100), dlp::kMaxPacketSize);
          const uint32_t pace = q[4] | (q[5] << 8);
          std::vector<std::vector<uint8_t>> burst;
          burst.reserve(count);
          for (uint32_t i = 0; i < count; ++i) {
            dlp::Header bh;
            bh.type = dlp::PacketType::kBurstData;
            bh.packet_index = static_cast<uint16_t>(i);
            bh.packet_count = static_cast<uint16_t>(count);
            bh.timestamp_us = header->timestamp_us;
            bh.payload_size = static_cast<uint16_t>(size - dlp::kHeaderSize);
            std::vector<uint8_t> pkt(size, 0);
            dlp::WriteHeader(bh, pkt.data());
            burst.push_back(std::move(pkt));
          }
          SendPaced(burst, pace, [&](const std::vector<uint8_t>& p) { udp.SendTo(from, p); });
          break;
        }
        case dlp::PacketType::kPing: {
          // La hora del PC en el payload: la tablet calcula con ella la diferencia de relojes y, así, cuánto tarda
          // cada frame de punta a punta. Una tablet antigua solo lee la cabecera.
          uint8_t payload[8];
          const uint64_t now = NowUs();
          for (int i = 0; i < 8; ++i) payload[i] = static_cast<uint8_t>(now >> (8 * i));
          udp.SendTo(from, dlp::MakeControlPacket(dlp::PacketType::kPong, payload, sizeof(payload),
                                                  header->timestamp_us));
          break;
        }
        case dlp::PacketType::kPong: {
          // Respuesta a un PING del host (con --status): lleva de vuelta nuestro propio timestamp.
          const uint64_t now = NowUs();
          if (header->timestamp_us > 0 && header->timestamp_us <= now) rtt_us = now - header->timestamp_us;
          break;
        }
        default:
          break;
      }
    };
    if (opt.touch) {
      // Los toques caen sobre el monitor que se captura (el virtual con --virtual).
      const RECT r = pipe->gpu.desktop_rect;
      const auto mode = opt.input_mode == 0   ? TouchInjector::Mode::kTouch
                        : opt.input_mode == 1 ? TouchInjector::Mode::kMouse
                                              : LoadInputMode();
      touch_ready = touch.Init(r, mode);
      if (touch_ready) {
        std::printf("Táctil: %s (pantalla de %ldx%ld en %ld,%ld).\n", touch.active_name(), r.right - r.left,
                    r.bottom - r.top, r.left, r.top);
      } else {
        std::printf("Táctil NO disponible.\n");
      }
    }
    if (!udp.Bind(opt.port, on_packet, opt.usb, [&opt] { return StopAdbServer(opt.adb_path); })) break;
    std::printf("Escuchando en UDP %u. Abre DeskLink en la tablet y pulsa Conectar (por cable USB no hace falta IP ni anclaje).\n",
                opt.port);
    if (opt.usb) {
    adb.set_open_app(opt.open_tablet);
    adb.Start(opt.port, opt.adb_path);
  }
    if (opt.status) std::printf("@listo puerto=%u\n", opt.port);

    // Cada trozo Opus sale en su propio paquete, con el trozo anterior repetido: por WiFi, si se pierde
    // un paquete, el siguiente lo trae. Son ~2 x 128 kbps, nada al lado del vídeo.
    uint32_t audio_seq = 0;
    std::vector<uint8_t> audio_prev;
    auto on_audio = [&](const uint8_t* frame, size_t size, uint64_t ts_us) {
      udp.SendToPeer(dlp::MakeAudioPacket(audio_seq++, ts_us, frame, size, audio_prev.data(), audio_prev.size()));
      audio_prev.assign(frame, frame + size);
    };
    auto update_audio = [&]() {
      const bool want = opt.audio && opt.audio_tablet && udp.has_peer();
      if (want == audio.running()) return;
      if (want) {
        audio_prev.clear();
        audio.Start(on_audio, opt.audio_kbps * 1000u);
      } else {
        audio.Stop();
        std::printf("Sonido: detenido.\n");
      }
    };

    constexpr uint64_t kRefineIntervalUs = 100'000;  // separación de las repeticiones de afinado
    uint64_t last_submit_us = 0;
    uint64_t last_rect_check_us = 0;
    ID3D11Texture2D* last_nv12 = nullptr;
    bool dirty = false;      // la imagen guardada (o el cursor) cambió y aún no se convirtió ni envió
    uint64_t dirty_since_us = 0;  // cuándo llegó el primer cambio aún sin enviar (lo que espera por el límite de fps)
    uint64_t capture_wait_us = 0, capture_wait_n = 0, latency_capture_wait = 0, latency_capture_n = 0;
    int flush_left = 0;      // repeticiones pendientes para vaciar el codificador
    UINT refine_left = 0;    // repeticiones lentas pendientes para afinar la calidad de la imagen quieta
    uint64_t captured = 0, composed = 0, last_composed = 0, last_stats_us = NowUs();
    uint64_t convert_us = 0, last_convert = 0, last_fed = 0, last_mailbox = 0, last_submitted = 0, last_dropped = 0;
    uint64_t last_encode_us = 0, last_encode_count = 0, last_sender_dropped = sender.dropped;
    uint64_t latency_audio = 0, latency_audio_loud = 0;
    uint64_t latency_ticks = 0, latency_fed = 0, latency_encoded = 0, latency_sent = 0, latency_mailbox = 0,
             latency_encode_us = 0, latency_busy = 0;
    TouchInjector::Mode last_input_mode = touch.active();
    uint64_t last_bytes = 0, last_encoded = 0, last_latency = 0, last_captured = 0, last_keyframes = 0, last_retransmits = 0, last_touch = 0;
    bool failed = false;
    uint32_t applied_seq = 0;
    uint32_t session_ticks = 0;

    // Rehace el pipeline (y el monitor virtual si cambia su modo) con la configuración actual de opt.
    // Si el monitor no acepta el modo nuevo se vuelve a fallback_w x fallback_h.
    // resize: solo cambia la resolución elegida, con el mismo monitor (se cambia el modo sin quitarlo).
    auto rebuild = [&](bool replug, bool resize, UINT fallback_w, UINT fallback_h) -> bool {
      {
        std::lock_guard<std::mutex> lock(enc_mutex);
        enc_ptr = nullptr;
      }
      pipe.reset();
      sender.Clear();
      if (replug) {
        virtual_display.Unplug();
        Sleep(500);
        UINT mw = 0, mh = 0;
        MonitorSize(opt, &mw, &mh);
        if (!virtual_display.Plug(mw, mh, MonitorHz(opt.fps), opt.virtual_width, opt.virtual_height)) {
          std::fprintf(stderr, "No se pudo poner el monitor virtual a %ux%u: vuelvo a %ux%u.\n", opt.virtual_width,
                       opt.virtual_height, fallback_w, fallback_h);
          opt.virtual_width = fallback_w;
          opt.virtual_height = fallback_h;
          virtual_display.Unplug();
          Sleep(500);
          MonitorSize(opt, &mw, &mh);
          if (!virtual_display.Plug(mw, mh, MonitorHz(opt.fps), opt.virtual_width, opt.virtual_height)) return false;
        }
      } else if (resize) {
        virtual_display.SetMode(opt.virtual_width, opt.virtual_height, MonitorHz(opt.fps));
      }
      pipe = build(50);
      if (!pipe) return false;
      {
        std::lock_guard<std::mutex> lock(enc_mutex);
        enc_ptr = pipe->encoder.get();
      }
      if (touch_ready) touch.SetRect(pipe->gpu.desktop_rect);
      last_nv12 = nullptr;
      dirty = false;
      flush_left = 0;
      refine_left = 0;
      last_fed = last_mailbox = last_submitted = last_dropped = last_encode_us = last_encode_count = 0;
      latency_fed = latency_encoded = latency_mailbox = latency_encode_us = 0;
      return true;
    };

    while (g_running) {
      if (config_seq.load() != applied_seq) {
        // La tablet saludó (su pantalla, Hz y códecs) o el usuario cambió algo en la ventana del PC: se decide de
        // nuevo y, si cambia el tamaño, los fps del monitor o el códec, se rehace el pipeline.
        dlp::HelloConfig caps;
        std::vector<std::string> lines;
        {
          std::lock_guard<std::mutex> lock(config_mutex);
          caps = requested_config;
          lines.swap(pending_settings);
          applied_seq = config_seq.load();
        }
        const Options before = opt;
        Effective eff_before, eff_after;
        {
          std::lock_guard<std::mutex> lock(quality_mutex);
          eff_before = Apply(opt, tablet_request, fps_gov.fps());
          for (const auto& line : lines) {
            if (!ApplySettingsLine(line, &opt)) std::printf("Ajustes no válidos de la ventana: %s\n", line.c_str());
          }
          Decide(&opt, caps, pipe->w, pipe->h);
          adb.set_open_app(opt.open_tablet);
          fps_gov.Reset(Apply(opt, tablet_request, 0).limit_fps);
          eff_after = Apply(opt, tablet_request, fps_gov.fps());
          min_interval_us = 1'000'000ull / eff_after.fps;
        }
        if (!lines.empty()) {
          std::printf("Ajustes del PC: calidad %s, fps %s, %s Mbps, sonido en %s, táctil: %s%s.\n", ProfileArg(opt.profile),
                      opt.user_fps ? std::to_string(opt.user_fps).c_str() : "auto",
                      opt.user_bitrate ? std::to_string(opt.user_bitrate).c_str() : "auto",
                      opt.audio_tablet ? "la tablet" : "el PC", InputArg(opt.input_mode < 0 ? 0 : opt.input_mode),
                      opt.battery_adapt ? ", baja si la batería no aguanta" : "");
        }
        // Táctil elegido en el PC.
        if (opt.input_mode != before.input_mode && opt.input_mode >= 0) {
          view_only = opt.input_mode == 2;
          if (touch_ready && opt.input_mode < 2) {
            const auto mode = opt.input_mode == 1 ? TouchInjector::Mode::kMouse : TouchInjector::Mode::kTouch;
            touch.SetMode(mode);
            SaveInputMode(mode);
          }
          if (view_only) touch.ReleaseAll();
        }
        // El monitor solo se quita y se vuelve a poner si cambia su tamaño (otra tablet). Los Hz (más de 60 fps) se
        // cambian como la resolución, con el mismo monitor: al quitarlo Windows olvidaba su resolución y su escala.
        UINT mw_before = 0, mh_before = 0, mw_after = 0, mh_after = 0;
        MonitorSize(before, &mw_before, &mh_before);
        MonitorSize(opt, &mw_after, &mh_after);
        const bool replug = opt.virtual_display && (mw_after != mw_before || mh_after != mh_before);
        const bool resize = opt.virtual_display && !replug &&
                            (opt.virtual_width != before.virtual_width || opt.virtual_height != before.virtual_height ||
                             MonitorHz(opt.fps) != MonitorHz(before.fps));
        const bool rebuild_needed = replug || resize || opt.fps != before.fps || opt.codec != before.codec ||
                                    eff_after.scale_percent != pipe->scale_percent;
        if (eff_after.bitrate_mbps_x10 != eff_before.bitrate_mbps_x10) rate_reset = true;
        if (rebuild_needed) {
          std::printf("Monitor %s%ux%u a %u fps, %u Mbps, %s.\n",
                      opt.virtual_display ? "virtual de " : "(captura de la pantalla actual) ",
                      opt.virtual_display ? opt.virtual_width : pipe->w,
                      opt.virtual_display ? opt.virtual_height : pipe->h, opt.fps, opt.bitrate_mbps,
                      opt.codec == VideoCodec::kHevc ? "HEVC" : "H.264");
          if (!rebuild(replug, resize, before.virtual_width, before.virtual_height)) {
            std::fprintf(stderr, "Error: no se pudo aplicar la configuración nueva.\n");
            failed = true;
            break;
          }
          rate_reset = true;  // codificador nuevo: la tasa vuelve a empezar
        } else if (eff_after.fps != eff_before.fps) {
          // Solo cambian los fps (otra calidad): la tablet ajusta su pantalla y el decodificador sigue.
          std::lock_guard<std::mutex> lock(info_mutex);
          info_current.fps = static_cast<uint16_t>(eff_after.fps);
          info_payload = dlp::EncodeStreamInfo(info_current);
        }
        if (config_seq.load() == applied_seq) reconfiguring = false;
        if (udp.has_peer()) {
          {
            std::lock_guard<std::mutex> lock(info_mutex);
            udp.SendToPeer(dlp::MakeControlPacket(dlp::PacketType::kStreamInfo, info_payload.data(),
                                                  info_payload.size(), NowUs()));
          }
          send_session(nullptr, true);
          if (touch_ready) send_input_mode(nullptr);
        }
        if (rebuild_needed) enc_keyframe();
        continue;
      }

      ID3D11Texture2D* frame = nullptr;
      // Sin nada pendiente se espera más a la siguiente imagen: sin cliente o con el escritorio quieto el
      // bucle apenas despierta. Con algo pendiente, justo hasta el siguiente hueco de envío.
      UINT timeout_ms = udp.has_peer() ? 16 : 100;
      if (udp.has_peer() && pipe->converter.has_image()) {
        uint64_t due = 0;
        if (dirty || flush_left > 0 || pipe->encoder->keyframe_pending()) {
          due = last_submit_us + min_interval_us.load();
        } else if (refine_left > 0) {
          due = last_submit_us + kRefineIntervalUs;
        }
        if (due) {
          const uint64_t t = NowUs();
          timeout_ms = due > t ? static_cast<UINT>(std::min<uint64_t>((due - t + 999) / 1000, 16)) : 0;
        }
      }
      auto result = pipe->capture.Acquire(timeout_ms, &frame);
      // Cambió la escala del perfil de batería: se rehace todo con el tamaño de codificación nuevo.
      if (rescale_requested.exchange(false) && current_quality().scale_percent != pipe->scale_percent) {
        pipe->capture.Release();
        std::printf("Cambia la escala de codificación: reinicio la captura.\n");
        if (!rebuild(false, false, opt.virtual_width, opt.virtual_height)) {
          std::fprintf(stderr, "Error: no se pudo reiniciar la captura con la escala nueva.\n");
          failed = true;
          break;
        }
        rate_reset = true;  // codificador nuevo: tasa desde abajo
        if (udp.has_peer()) {
          std::lock_guard<std::mutex> lock(info_mutex);
          udp.SendToPeer(dlp::MakeControlPacket(dlp::PacketType::kStreamInfo, info_payload.data(),
                                                info_payload.size(), NowUs()));
        }
        enc_keyframe();
        continue;
      }
      if (result == DesktopCapture::Result::kLost) {
        // Windows corta la captura al cambiar de modo, al salir la pantalla de UAC o al bloquear la
        // sesión. Primero se intenta reabrirla tal cual; si el tamaño cambió (el usuario cambió la
        // resolución) se recrea todo el pipeline con el tamaño nuevo y se avisa a la tablet.
        bool reopened = false;
        for (int i = 0; i < 50 && g_running && !(reopened = pipe->capture.Init(pipe->gpu)); ++i) Sleep(200);
        if (!reopened) {
          std::fprintf(stderr, "Error: no se pudo recuperar la captura de pantalla.\n");
          failed = true;
          break;
        }
        if (pipe->capture.width() == pipe->w && pipe->capture.height() == pipe->h) {
          std::printf("Captura recuperada.\n");
          enc_keyframe();
          continue;
        }
        std::printf("La resolución cambió a %ux%u: reinicio la captura.\n", pipe->capture.width(),
                    pipe->capture.height());
        if (!rebuild(false, false, opt.virtual_width, opt.virtual_height)) {
          std::fprintf(stderr, "Error: no se pudo reiniciar la captura con la nueva resolución.\n");
          failed = true;
          break;
        }
        if (opt.virtual_display) {
          // La eligió el usuario en Windows: se recuerda para esta tablet y manda sobre su resolución nativa.
          dlp::HelloConfig caps;
          {
            std::lock_guard<std::mutex> lock(config_mutex);
            caps = requested_config;
          }
          SaveSize(pipe->w, pipe->h, caps.width, caps.height);
          std::lock_guard<std::mutex> lock(quality_mutex);
          opt.virtual_width = pipe->w;
          opt.virtual_height = pipe->h;
        }
        if (udp.has_peer()) {
          std::lock_guard<std::mutex> lock(info_mutex);
          udp.SendToPeer(dlp::MakeControlPacket(dlp::PacketType::kStreamInfo, info_payload.data(),
                                                info_payload.size(), NowUs()));
        }
        enc_keyframe();
        continue;
      }
      if (result == DesktopCapture::Result::kError) {
        failed = true;
        break;
      }

      touch.ExpireIfSilent(GetTickCount64(), 700);
      update_audio();
      if (udp.ExpirePeer(std::chrono::seconds(5))) {
        touch.ReleaseAll();
        last_client_ip = 0;
        rtt_us = 0;
        tablet_received = tablet_lost = 0;
        tablet_decoded = -1;
        tablet_total_x10 = 0;
        std::printf("Cliente desconectado (sin respuesta durante 5 s). Esperando cliente.\n");
      }

      // Cada imagen nueva del escritorio solo se copia (hay que hacerlo antes de soltarla) y queda como
      // «última»; así la última actualización antes de quedarse quieto nunca se pierde por el límite de fps.
      // El cursor y la conversión a NV12 se hacen solo cuando el frame va al codificador: con el escritorio
      // a 120-144 Hz, el cursor moviéndose o sin cliente conectado ya no se convierten imágenes que nadie ve.
      if (result == DesktopCapture::Result::kFrame) {
        pipe->converter.Store(frame);
        pipe->capture.Release();
        if (!dirty) dirty_since_us = NowUs();
        dirty = true;
        ++captured;
      } else {
        pipe->capture.Release();
        // Solo se movió el cursor: se redibujará sobre la última imagen.
        if (result == DesktopCapture::Result::kCursorMoved && opt.cursor && !opt.virtual_display &&
            pipe->converter.has_image()) {
          if (!dirty) dirty_since_us = NowUs();
          dirty = true;
        }
      }

      // Se envía la última imagen si es nueva y toca por fps. Tras la última imagen nueva se repite
      // 2 veces: el codificador de hardware retiene un par de frames y, sin nuevas entradas, la
      // última actualización se quedaba dentro durante cientos de ms (imagen «pegada»). Después se
      // repite unas pocas veces más, despacio: con CBR el codificador gasta esos bits en afinar la imagen
      // quieta (texto nítido tras desplazar o arrastrar una ventana) en vez de dejarla como salió en movimiento.
      const uint64_t now = NowUs();
      const bool refine_due = refine_left > 0 && now - last_submit_us >= kRefineIntervalUs;
      if (udp.has_peer() && pipe->converter.has_image() && now - last_submit_us >= min_interval_us.load() &&
          (dirty || flush_left > 0 || refine_due || pipe->encoder->keyframe_pending())) {
        if (dirty || !last_nv12) {
          const uint64_t conv_start = NowUs();
          ID3D11Texture2D* nv12 = pipe->converter.Compose();
          convert_us += NowUs() - conv_start;
          if (!nv12) {
            failed = true;
            break;
          }
          last_nv12 = nv12;
          ++composed;
        }
        pipe->encoder->Submit(last_nv12, now);
        last_submit_us = now;
        if (dirty) {
          if (now > dirty_since_us) capture_wait_us += now - dirty_since_us;
          ++capture_wait_n;
          dirty = false;
          flush_left = 2;
          refine_left = opt.refine;
        } else if (flush_left > 0) {
          --flush_left;
        } else if (refine_left > 0) {
          --refine_left;
        }
      }

      // Si el usuario mueve el monitor virtual en Configuración, el cursor dibujado y el táctil siguen su sitio.
      if (now - last_rect_check_us >= 1'000'000) {
        last_rect_check_us = now;
        DXGI_OUTPUT_DESC od{};
        if (SUCCEEDED(pipe->gpu.output->GetDesc(&od)) &&
            (od.DesktopCoordinates.left != pipe->gpu.desktop_rect.left ||
             od.DesktopCoordinates.top != pipe->gpu.desktop_rect.top)) {
          pipe->gpu.desktop_rect = od.DesktopCoordinates;
          pipe->converter.SetOrigin(od.DesktopCoordinates.left, od.DesktopCoordinates.top);
          if (touch_ready) touch.SetRect(od.DesktopCoordinates);
          std::printf("La pantalla se movió a %ld,%ld.\n", od.DesktopCoordinates.left, od.DesktopCoordinates.top);
        }
      }

      const uint64_t t = NowUs();
      if (t - last_stats_us >= 1'000'000) {
        // Si Windows rechazó los toques y se pasó a ratón, la tablet lo sabe (y puede avisar).
        if (touch_ready && touch.active() != last_input_mode) {
          last_input_mode = touch.active();
          if (udp.has_peer()) send_input_mode(nullptr);
        }
        const double secs = (t - last_stats_us) / 1e6;
        const uint64_t enc = frames_encoded - last_encoded;
        const uint64_t lat = encode_latency_us - last_latency;
        // Fluidez: si el codificador no da abasto con estos fps (descarta frames), se bajan un escalón, y vuelven a
        // subir solos cuando hay holgura (dlp::FpsGovernor).
        adb.set_busy(udp.has_peer());
        // Cada 5 s con la tablet conectada y la imagen moviéndose, dónde se va el tiempo de cada frame, de punta a punta.
        if (udp.has_peer() && ++latency_ticks % 5 == 0) {
          const uint64_t fed = pipe->encoder->fed_frames - latency_fed;
          const uint64_t encoded = pipe->encoder->encode_count - latency_encoded;
          const uint64_t sent = sender.sent_frames - latency_sent;
          if (fed >= 25 && encoded && sent) {
            const uint64_t waits = capture_wait_n - latency_capture_n;
            const double capture_ms = waits ? (capture_wait_us - latency_capture_wait) / 1000.0 / waits : 0.0;
            const double mailbox_ms = (pipe->encoder->mailbox_wait_us - latency_mailbox) / 1000.0 / fed;
            const double encode_ms = (pipe->encoder->encode_us - latency_encode_us) / 1000.0 / encoded;
            const double send_ms = (sender.busy_us - latency_busy) / 1000.0 / sent;
            std::printf("Latencia: espera de captura %.1f ms · en cola %.1f ms · codificador %.1f ms · envío %.1f ms · "
                        "ida y vuelta %.1f ms",
                        capture_ms, mailbox_ms, encode_ms, send_ms, rtt_us.load() / 1000.0);
            if (tablet_decode_x10 >= 0) std::printf(" · decodificar en la tablet %.1f ms", tablet_decode_x10 / 10.0);
            std::printf(" (%llu frames en 5 s).\n", static_cast<unsigned long long>(sent));
            // La tablet mide desde que el frame entra al codificador (con la hora del PC del PONG) hasta que se enciende
            // en su pantalla. La red es lo que queda de «llegar» al quitar lo que el PC tarda en cola, codificar y enviar.
            if (const uint32_t total = tablet_total_x10.load()) {
              const double arrive_ms = tablet_arrive_x10 / 10.0;
              const double net_ms = std::max(0.0, arrive_ms - mailbox_ms - encode_ms - send_ms);
              std::printf("Retardo de punta a punta: %.1f ms (peor %.1f ms) = espera de captura %.1f + llegar a la tablet "
                          "%.1f (de ello, red ~%.1f) + esperar reenvíos %.1f + decodificar %.1f + pantalla %.1f.\n",
                          capture_ms + total / 10.0, capture_ms + tablet_worst_x10 / 10.0, capture_ms, arrive_ms,
                          net_ms, tablet_hold_x10 / 10.0, tablet_decode_x10 >= 0 ? tablet_decode_x10 / 10.0 : 0.0,
                          tablet_screen_x10 / 10.0);
            }
          }
          latency_capture_wait = capture_wait_us;
          latency_capture_n = capture_wait_n;
          // Con el sonido en la tablet, cuánto se le manda y cuánto suena de verdad en el PC.
          if (audio.running()) {
            std::printf("Sonido: enviados %llu trozos de 10 ms en 5 s, %llu con algo audible.\n",
                        static_cast<unsigned long long>(audio.frames() - latency_audio),
                        static_cast<unsigned long long>(audio.loud_frames() - latency_audio_loud));
          }
          latency_audio = audio.frames();
          latency_audio_loud = audio.loud_frames();
          latency_fed = pipe->encoder->fed_frames;
          latency_encoded = pipe->encoder->encode_count;
          latency_sent = sender.sent_frames;
          latency_mailbox = pipe->encoder->mailbox_wait_us;
          latency_encode_us = pipe->encoder->encode_us;
          latency_busy = sender.busy_us;
        }
        if (const uint64_t sd = sender.dropped - last_sender_dropped) {
          last_sender_dropped += sd;
          std::printf("Envío atrasado: el enlace no da abasto, tiro %llu frames y pido uno clave.\n",
                      static_cast<unsigned long long>(sd));
        }
        const uint64_t submitted = pipe->encoder->submitted_frames - last_submitted;
        const uint64_t dropped = pipe->encoder->dropped_frames - last_dropped;
        if (udp.has_peer()) {
          UINT new_fps = 0;
          bool slower = false;
          {
            std::lock_guard<std::mutex> lock(quality_mutex);
            if (fps_gov.Update(static_cast<uint32_t>(submitted), static_cast<uint32_t>(dropped))) {
              new_fps = Apply(opt, tablet_request, fps_gov.fps()).fps;
              slower = fps_gov.reduced();
              min_interval_us = 1'000'000ull / new_fps;
            }
          }
          if (new_fps) {
            const uint64_t mft_n = pipe->encoder->encode_count - last_encode_count;
            const uint64_t fed_n = pipe->encoder->fed_frames - last_fed;
            std::printf("Fluidez: %u fps (%s; último segundo: %llu de %llu frames descartados, total %.1f ms: "
                        "en cola %.1f ms, codificador %.1f ms).\n",
                        new_fps, slower ? "la gráfica no llega a más" : "vuelve a subir",
                        static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(submitted),
                        enc ? lat / 1000.0 / enc : 0.0,
                        fed_n ? (pipe->encoder->mailbox_wait_us - last_mailbox) / 1000.0 / fed_n : 0.0,
                        mft_n ? (pipe->encoder->encode_us - last_encode_us) / 1000.0 / mft_n : 0.0);
            std::lock_guard<std::mutex> lock(info_mutex);
            info_current.fps = static_cast<uint16_t>(new_fps);
            info_payload = dlp::EncodeStreamInfo(info_current);
            udp.SendToPeer(dlp::MakeControlPacket(dlp::PacketType::kStreamInfo, info_payload.data(),
                                                  info_payload.size(), NowUs()));
          }
          // Se repite cada 2 s por si se perdió (UDP) y se manda en cuanto cambia.
          send_session(nullptr, (++session_ticks % 2) == 0);
        }
        if (opt.status) {
          char tablet_fps_text[16] = "-";
          if (tablet_decoded.load() >= 0) std::snprintf(tablet_fps_text, sizeof(tablet_fps_text), "%d", tablet_decoded.load());
          // Una línea por segundo para la app de bandeja: clave=valor separados por espacios.
          const bool peer = udp.has_peer();
          char ip[INET_ADDRSTRLEN] = "-";
          if (peer && !udp.peer_is_usb()) {
            in_addr a{};
            a.s_addr = last_client_ip.load();
            inet_ntop(AF_INET, &a, ip, sizeof(ip));
          }
          // PING del host: la tablet que lo conteste con PONG da el RTT visto desde el PC.
          if (peer) udp.SendToPeer(dlp::MakeControlPacket(dlp::PacketType::kPing, nullptr, 0, NowUs()));
          const dlp::SessionConfig session = make_session();
          const char* reduced = session.reduced == dlp::Reduced::kWifi      ? "wifi"
                                : session.reduced == dlp::Reduced::kBattery ? "bateria"
                                : session.reduced == dlp::Reduced::kLoad    ? "carga"
                                                                            : "no";
          std::printf("@estado enlace=%s ip=%s capturados=%.1f codificados=%.1f mbps=%.1f cod_ms=%.1f rtt_ms=%.1f "
                      "recibidos=%u perdidos=%u reenvios=%llu size=%ux%u fps_max=%u fps_obj=%u mbps_obj=%.1f "
                      "codec=%s reducido=%s adb=%d adb_pendientes=%d enviados=%.1f descartados=%.1f tablet_fps=%s\n",
                      !peer ? "ninguno" : udp.peer_is_usb() ? "usb" : peer_tether ? "anclaje" : "wifi", ip,
                      (captured - last_captured) / secs, enc / secs, (bytes_sent - last_bytes) * 8 / secs / 1e6,
                      enc ? lat / 1000.0 / enc : 0.0, rtt_us.load() / 1000.0, tablet_received.load(),
                      tablet_lost.load(), static_cast<unsigned long long>(retransmits.load() - last_retransmits),
                      pipe->w, pipe->h, opt.fps, session.fps, session.bitrate_x100kbps / 10.0,
                      opt.codec == VideoCodec::kHevc ? "hevc" : "h264", reduced, opt.usb ? adb.ready() : -1,
                      opt.usb ? adb.unauthorized() : 0, submitted / secs, dropped / secs, tablet_fps_text);
        } else {
          std::printf("capturados %.0f/s | codificados %.0f/s | %.1f Mbps | codificación %.1f ms | claves %llu | conv %.1f ms | buzón %.1f ms | errores de envío %llu | reenvíos %llu | toques %llu%s\n",
                      (captured - last_captured) / secs, enc / secs,
                      (bytes_sent - last_bytes) * 8 / secs / 1e6, enc ? lat / 1000.0 / enc : 0.0,
                      static_cast<unsigned long long>(keyframes_sent.load() - last_keyframes),
                      composed > last_composed ? (convert_us - last_convert) / 1000.0 / (composed - last_composed) : 0.0,
                      pipe->encoder->fed_frames > last_fed ? (pipe->encoder->mailbox_wait_us - last_mailbox) / 1000.0 / (pipe->encoder->fed_frames - last_fed) : 0.0,
                      static_cast<unsigned long long>(udp.send_errors()),
                      static_cast<unsigned long long>(retransmits.load() - last_retransmits),
                      static_cast<unsigned long long>(touch_packets.load() - last_touch),
                      udp.has_peer() ? "" : " | esperando cliente");
        }
        last_stats_us = t;
        last_bytes = bytes_sent;
        last_encoded = frames_encoded;
        last_latency = encode_latency_us;
        last_captured = captured;
        last_keyframes = keyframes_sent;
        last_retransmits = retransmits;
        last_touch = touch_packets;
        last_convert = convert_us;
        last_composed = composed;
        last_fed = pipe->encoder->fed_frames;
        last_mailbox = pipe->encoder->mailbox_wait_us;
        last_encode_us = pipe->encoder->encode_us;
        last_encode_count = pipe->encoder->encode_count;
        last_submitted = pipe->encoder->submitted_frames;
        last_dropped = pipe->encoder->dropped_frames;
      }
    }
    // Los hilos usan variables de este bloque: se paran antes de salir de él.
    {
      std::lock_guard<std::mutex> lock(enc_mutex);
      enc_ptr = nullptr;
    }
    if (pipe && pipe->encoder) pipe->encoder->Stop();
    sender.Stop();
    audio.Stop();
    adb.Stop();
    udp.Stop();
    exit_code = failed ? 1 : 0;
  } while (false);

  udp.Stop();
  pipe.reset();
  timeEndPeriod(1);
  MFShutdown();
  CoUninitialize();
  WSACleanup();
  return exit_code;
}

// Si se abrió con doble clic, la consola desaparece al terminar: se espera una tecla
// para poder leer el resultado o el error.
int main(int argc, char** argv) {
  const int code = Run(argc, argv);
  // Lanzado por la app de bandeja: nadie va a pulsar Enter.
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--status") == 0) return code;
  }
  DWORD pids[2];
  const bool own_console = GetConsoleProcessList(pids, 2) == 1;
  if (!g_replaced && (own_console || code != 0)) {
    std::printf("\nPulsa Enter para salir.\n");
    std::getchar();
  }
  return code;
}
