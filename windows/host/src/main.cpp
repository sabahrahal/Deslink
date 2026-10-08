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
#include <string>
#include <thread>
#include <vector>

#include "capture.h"
#include "converter.h"
#include "dlp/packet.h"
#include "encoder.h"
#include "gpu.h"
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

// Última resolución del monitor virtual (la que el usuario dejó en Windows), para la próxima vez.
std::wstring SizeFilePath() { return ConfigPath(L"virtual-size.txt"); }

bool LoadSavedSize(UINT* w, UINT* h) {
  const std::wstring path = SizeFilePath();
  if (path.empty()) return false;
  FILE* f = _wfopen(path.c_str(), L"r");
  if (!f) return false;
  unsigned a = 0, b = 0;
  const bool ok = std::fscanf(f, "%ux%u", &a, &b) == 2 && a >= 640 && b >= 480 && a <= 7680 && b <= 4320;
  std::fclose(f);
  if (ok) {
    *w = a;
    *h = b;
  }
  return ok;
}

void SaveSize(UINT w, UINT h) {
  const std::wstring path = SizeFilePath();
  if (path.empty()) return;
  CreateDirectoryW(path.substr(0, path.find_last_of(L'\\')).c_str(), nullptr);
  if (FILE* f = _wfopen(path.c_str(), L"w")) {
    std::fprintf(f, "%ux%u\n", w, h);
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

struct Options {
  uint16_t port = dlp::kDefaultPort;
  int output = 0;
  UINT fps = 60;
  UINT bitrate_mbps = 50;
  VideoCodec codec = VideoCodec::kHevc;
  bool cursor = true;    // --no-cursor: no dibuja el cursor en la imagen
  int input_mode = -1;   // --touch (0) / --mouse (1) solo para esta vez; si no, el guardado (táctil por defecto)
  bool touch = true;     // --no-touch desactiva el control táctil
  UINT pace_mbps = 100;  // reparte cada frame a esta tasa; 0 = de golpe (el anclaje USB pierde ráfagas)
  bool virtual_display = false;  // --virtual: crea un monitor virtual (driver desklink-idd) y captura ese
  UINT virtual_width = 2944;     // --size: resolución del monitor virtual (por defecto, la Lenovo Idea Tab Pro)
  UINT virtual_height = 1840;
  bool size_given = false;       // si no se pasa --size, se usa la última resolución recordada
  bool usb = true;               // --no-usb: sin cable USB (TCP por adb reverse)
  std::wstring adb_path;         // --adb: ruta de adb.exe (si no, se busca)
  UINT refine = 4;               // --refine: repeticiones extra al quedarse quieta la imagen para afinar la calidad
  bool status = false;           // --status: líneas «@...» para la app de bandeja (desklink.exe) en vez de las estadísticas
  std::wstring stop_event;       // --stop-event: evento con nombre que, al activarse, cierra el host limpiamente
};

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
  UINT w = 0, h = 0;
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
      o->fps = static_cast<UINT>(std::atoi(v));
    } else if (a == "--bitrate") {
      auto v = next("--bitrate"); if (!v) return false;
      o->bitrate_mbps = static_cast<UINT>(std::atoi(v));
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
    } else if (a == "--adb") {
      auto v = next("--adb"); if (!v) return false;
      o->adb_path = Widen(v);
    } else if (a == "--status") {
      o->status = true;
    } else if (a == "--stop-event") {
      auto v = next("--stop-event"); if (!v) return false;
      o->stop_event = Widen(v);
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
    } else if (a == "--codec") {
      auto v = next("--codec"); if (!v) return false;
      o->codec = std::string(v) == "h264" ? VideoCodec::kH264 : VideoCodec::kHevc;
    } else {
      std::fprintf(stderr,
                   "Uso: desklink-host [--port 47800] [--output 0] [--fps 60] [--bitrate 50] "
                   "[--codec hevc|h264] [--pace Mbps] [--refine 4] [--no-touch] [--touch|--mouse] [--no-cursor] "
                   "[--virtual [--size 2944x1840]] [--no-usb] [--adb ruta\\adb.exe] [--status] [--stop-event nombre]\n");
      return false;
    }
  }
  return o->fps > 0 && o->fps <= 240 && o->bitrate_mbps > 0 && o->refine <= 30;
}

}  // namespace

int Run(int argc, char** argv) {
  Options opt;
  if (!ParseArgs(argc, argv, &opt)) return 2;
  SetConsoleOutputCP(CP_UTF8);  // los mensajes llevan tildes
  SingleInstance instance;  // se destruye lo último: el siguiente host entra cuando este ya soltó todo
  const Options cli = opt;  // lo que se pidió al arrancar; la tablet puede cambiarlo con su HELLO

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
  int exit_code = 1;

  std::atomic<uint32_t> next_frame_id{1};
  std::atomic<uint64_t> bytes_sent{0}, frames_encoded{0}, encode_latency_us{0}, keyframes_sent{0}, retransmits{0}, touch_packets{0};
  // Para --status: último informe de la tablet y RTT medido con PING del host (0 = la tablet no contesta).
  std::atomic<uint32_t> tablet_received{0}, tablet_lost{0};
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
  const std::string host_name = ComputerNameUtf8();

  // Configuración que pide la tablet en su HELLO (resolución, fps, tasa, códec). El hilo de recepción la deja
  // aquí y el bucle principal la aplica rehaciendo el pipeline; mientras tanto no se responde a los HELLO
  // para que la tablet no arranque con el tamaño viejo.
  std::mutex config_mutex;
  dlp::HelloConfig requested_config;
  std::atomic<uint32_t> config_seq{0};
  std::atomic<bool> reconfiguring{false};
  std::atomic<uint32_t> max_bps_setting{opt.bitrate_mbps * 1'000'000u};
  std::atomic<bool> rate_reset{false};

  auto on_encoded = [&](const uint8_t* data, size_t size, bool keyframe, uint64_t ts_us) {
    auto packets = dlp::PacketizeFrame(data, size, next_frame_id++, keyframe, ts_us);
    for (const auto& pkt : packets) {
      auto ph = dlp::ParseHeader(pkt.data(), pkt.size());
      if (ph) history.Put(ph->frame_id, ph->packet_index, pkt);
    }
    if (udp.peer_is_usb()) {
      udp.SendBatchToPeer(packets);  // por cable no hay pérdidas: TCP ya regula el ritmo, sin --pace
    } else {
      SendPaced(packets, opt.pace_mbps, [&](const std::vector<uint8_t>& p) { udp.SendToPeer(p); });
    }
    bytes_sent += size;
    ++frames_encoded;
    if (keyframe) ++keyframes_sent;
    const uint64_t now = NowUs();
    if (now > ts_us) encode_latency_us += now - ts_us;
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
    std::printf("Captura: %ux%u a %u fps, %u Mbps, %s\n", p->w, p->h, opt.fps, opt.bitrate_mbps,
                opt.codec == VideoCodec::kHevc ? "HEVC" : "H.264");
    if (!p->converter.Init(p->gpu, p->w, p->h, 24)) return nullptr;
    p->converter.SetDrawCursor(opt.cursor);
    p->encoder = std::make_unique<MfEncoder>();
    if (!p->encoder->Init(p->gpu, opt.codec, p->w, p->h, opt.fps, opt.bitrate_mbps * 1'000'000u, on_encoded)) {
      std::fprintf(stderr, "Error: no se pudo iniciar el codificador (arriba está el motivo).\n");
      return nullptr;
    }
    dlp::StreamInfo info;
    info.width = static_cast<uint16_t>(p->w);
    info.height = static_cast<uint16_t>(p->h);
    info.fps = static_cast<uint16_t>(opt.fps);
    info.codec = opt.codec == VideoCodec::kHevc ? dlp::Codec::kHevc : dlp::Codec::kH264;
    info.host_flags = dlp::kHostAcceptsHelloConfig;
    info.host_name = host_name;
    {
      std::lock_guard<std::mutex> lock(info_mutex);
      info_payload = dlp::EncodeStreamInfo(info);
    }
    return p;
  };

  do {
    if (opt.virtual_display) {
      if (!opt.size_given && LoadSavedSize(&opt.virtual_width, &opt.virtual_height)) {
        std::printf("Resolución recordada del monitor virtual: %ux%u.\n", opt.virtual_width, opt.virtual_height);
      }
      if (!virtual_display.Plug(opt.virtual_width, opt.virtual_height, opt.fps)) break;
    }
    pipe = build(10);
    if (!pipe) break;
    {
      std::lock_guard<std::mutex> lock(enc_mutex);
      enc_ptr = pipe->encoder.get();
    }
    if (opt.virtual_display) SaveSize(pipe->w, pipe->h);

    std::atomic<uint32_t> last_client_ip{0};
    std::atomic<uint64_t> last_keyframe_us{0};
    // Tasa adaptativa (AIMD): baja un 30 % ante pérdidas (informe de la tablet o petición de frame
    // clave) y recuerda un techo por debajo de la tasa que falló; sube un 10 % cada 2 s tras 8 s limpios y el
    // techo se relaja 1 Mbps cada 20 s limpios. Solo se toca desde el hilo de recepción.
    // El máximo puede cambiarlo la tablet (max_bps_setting); se relee en cada paquete.
    uint32_t max_bps = max_bps_setting.load();
    uint32_t min_bps = std::max<uint32_t>(max_bps / 10, 3'000'000u);
    uint32_t current_bps = max_bps;
    uint32_t ceiling_bps = max_bps;
    uint64_t last_decrease_us = 0;
    uint64_t last_relax_us = 0;
    uint64_t last_increase_us = 0;
    constexpr uint32_t kStartBps = 20'000'000;  // un frame clave de 50 Mbps desborda el enlace al conectar
    auto decrease_rate = [&](uint64_t now, const char* why) {
      if (now - last_decrease_us < 1'000'000) return;  // un solo recorte por segundo
      last_decrease_us = now;
      last_relax_us = now;
      ceiling_bps = std::max<uint32_t>(static_cast<uint32_t>(current_bps * 0.9), min_bps);
      const uint32_t target = std::max<uint32_t>(static_cast<uint32_t>(current_bps * 0.7), min_bps);
      if (target == current_bps) return;
      current_bps = target;
      enc_bitrate(current_bps);
      std::printf("Tasa adaptada a %.1f Mbps (%s)\n", current_bps / 1e6, why);
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
        // La tablet cambió la tasa máxima: se vuelve a empezar desde abajo con el máximo nuevo.
        max_bps = max_bps_setting.load();
        min_bps = std::max<uint32_t>(max_bps / 10, 3'000'000u);
        current_bps = udp.peer_is_usb() ? max_bps : std::min(max_bps, kStartBps);
        ceiling_bps = max_bps;
        last_decrease_us = last_increase_us = NowUs();
        enc_bitrate(current_bps);
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
            if (usb) {
              std::printf("Cliente conectado por cable USB.\n");
            } else {
              char ip[INET_ADDRSTRLEN] = {};
              inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
              std::printf("Cliente conectado: %s:%u\n", ip, ntohs(from.sin_port));
            }
            // Por cable no se pierden paquetes: se empieza ya a la tasa máxima.
            current_bps = usb ? max_bps : std::min(max_bps, kStartBps);
            ceiling_bps = max_bps;
            last_decrease_us = last_increase_us = NowUs();
            enc_bitrate(current_bps);
          }
          udp.SetPeer(from);
          if (!reconfiguring) {
            std::lock_guard<std::mutex> lock(info_mutex);
            udp.SendTo(from, dlp::MakeControlPacket(dlp::PacketType::kStreamInfo, info_payload.data(),
                                                    info_payload.size(), NowUs()));
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
          if (mode != touch.requested()) {
            touch.SetMode(mode);
            SaveInputMode(mode);
            std::printf("La tablet cambió el táctil a %s.\n", touch.active_name());
          }
          send_input_mode(&from);
          break;
        }
        case dlp::PacketType::kKeyframeRequest: {
          // Un frame clave es varias veces más grande que uno normal: si la red pierde paquetes y
          // se piden uno tras otro, la propia petición mantiene la imagen congelada. Se limita a 1/s.
          const uint64_t now = NowUs();
          decrease_rate(now, "la tablet pide frame clave");
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
          const uint64_t now = NowUs();
          if (lost > 0) {
            decrease_rate(now, "la tablet pierde frames");
          } else {
            if (now - last_relax_us >= 20'000'000 && ceiling_bps < max_bps) {
              ceiling_bps = std::min<uint32_t>(ceiling_bps + 1'000'000, max_bps);
              last_relax_us = now;
            }
            if (now - last_decrease_us >= 8'000'000 && now - last_increase_us >= 2'000'000 && current_bps < ceiling_bps) {
              last_increase_us = now;
              current_bps = std::min<uint32_t>(static_cast<uint32_t>(current_bps * 1.1) + 1, ceiling_bps);
              enc_bitrate(current_bps);
              std::printf("Tasa adaptada a %.1f Mbps (enlace limpio)\n", current_bps / 1e6);
            }
          }
          break;
        }
        case dlp::PacketType::kTouch: {
          if (!touch_ready || header->payload_size < 1) break;
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
        case dlp::PacketType::kPing:
          udp.SendTo(from, dlp::MakeControlPacket(dlp::PacketType::kPong, nullptr, 0,
                                                  header->timestamp_us));
          break;
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
      const auto mode = opt.input_mode >= 0 ? static_cast<TouchInjector::Mode>(opt.input_mode) : LoadInputMode();
      touch_ready = touch.Init(r, mode);
      if (touch_ready) {
        std::printf("Táctil: %s (pantalla de %ldx%ld en %ld,%ld).\n", touch.active_name(), r.right - r.left,
                    r.bottom - r.top, r.left, r.top);
      } else {
        std::printf("Táctil NO disponible.\n");
      }
    }
    if (!udp.Bind(opt.port, on_packet, opt.usb)) break;
    std::printf("Escuchando en UDP %u. Abre DeskLink en la tablet y pulsa Conectar (por cable USB no hace falta IP ni anclaje).\n",
                opt.port);
    if (opt.usb) adb.Start(opt.port, opt.adb_path);
    if (opt.status) std::printf("@listo puerto=%u\n", opt.port);

    const uint64_t min_interval_us = 1'000'000ull / opt.fps;
    constexpr uint64_t kRefineIntervalUs = 100'000;  // separación de las repeticiones de afinado
    uint64_t last_submit_us = 0;
    uint64_t last_rect_check_us = 0;
    ID3D11Texture2D* last_nv12 = nullptr;
    bool dirty = false;      // la imagen guardada (o el cursor) cambió y aún no se convirtió ni envió
    int flush_left = 0;      // repeticiones pendientes para vaciar el codificador
    UINT refine_left = 0;    // repeticiones lentas pendientes para afinar la calidad de la imagen quieta
    uint64_t captured = 0, composed = 0, last_composed = 0, last_stats_us = NowUs();
    uint64_t convert_us = 0, last_convert = 0, last_fed = 0, last_mailbox = 0;
    TouchInjector::Mode last_input_mode = touch.active();
    uint64_t last_bytes = 0, last_encoded = 0, last_latency = 0, last_captured = 0, last_keyframes = 0, last_retransmits = 0, last_touch = 0;
    bool failed = false;
    uint32_t applied_seq = 0;

    // Rehace el pipeline (y el monitor virtual si cambia su modo) con la configuración actual de opt.
    // Si el monitor no acepta el modo nuevo se vuelve a fallback_w x fallback_h.
    auto rebuild = [&](bool replug, UINT fallback_w, UINT fallback_h) -> bool {
      {
        std::lock_guard<std::mutex> lock(enc_mutex);
        enc_ptr = nullptr;
      }
      pipe.reset();
      if (replug) {
        virtual_display.Unplug();
        Sleep(500);
        if (!virtual_display.Plug(opt.virtual_width, opt.virtual_height, opt.fps)) {
          std::fprintf(stderr, "No se pudo poner el monitor virtual a %ux%u: vuelvo a %ux%u.\n", opt.virtual_width,
                       opt.virtual_height, fallback_w, fallback_h);
          opt.virtual_width = fallback_w;
          opt.virtual_height = fallback_h;
          virtual_display.Unplug();
          Sleep(500);
          if (!virtual_display.Plug(opt.virtual_width, opt.virtual_height, opt.fps)) return false;
        }
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
      last_fed = last_mailbox = 0;
      return true;
    };

    while (g_running) {
      if (config_seq.load() != applied_seq) {
        // La tablet pidió otra resolución, fps, tasa o códec en su HELLO. 0 = lo que se dijo al arrancar.
        dlp::HelloConfig want;
        {
          std::lock_guard<std::mutex> lock(config_mutex);
          want = requested_config;
          applied_seq = config_seq.load();
        }
        const Options before = opt;
        opt.fps = want.fps > 0 && want.fps <= 240 ? want.fps : cli.fps;
        opt.bitrate_mbps = want.bitrate_mbps > 0 && want.bitrate_mbps <= 1000 ? want.bitrate_mbps : cli.bitrate_mbps;
        opt.codec = want.codec == 1 ? VideoCodec::kH264 : want.codec == 2 ? VideoCodec::kHevc : cli.codec;
        // --size al arrancar manda sobre la tablet; si no, el monitor virtual toma la resolución de la tablet.
        if (opt.virtual_display && !opt.size_given && want.width >= 640 && want.height >= 480 &&
            want.width <= 7680 && want.height <= 4320) {
          opt.virtual_width = want.width;
          opt.virtual_height = want.height;
        }
        const bool replug = opt.virtual_display &&
                            (opt.virtual_width != before.virtual_width || opt.virtual_height != before.virtual_height ||
                             opt.fps != before.fps);
        const bool rebuild_needed = replug || opt.fps != before.fps || opt.codec != before.codec;
        if (opt.bitrate_mbps != before.bitrate_mbps) {
          max_bps_setting = opt.bitrate_mbps * 1'000'000u;
          rate_reset = true;
        }
        if (rebuild_needed) {
          std::printf("La tablet pide %s%ux%u a %u fps, %u Mbps, %s.\n",
                      opt.virtual_display ? "un monitor de " : "(captura de la pantalla actual) ",
                      opt.virtual_display ? opt.virtual_width : pipe->w,
                      opt.virtual_display ? opt.virtual_height : pipe->h, opt.fps, opt.bitrate_mbps,
                      opt.codec == VideoCodec::kHevc ? "HEVC" : "H.264");
          if (!rebuild(replug, before.virtual_width, before.virtual_height)) {
            std::fprintf(stderr, "Error: no se pudo aplicar la configuración de la tablet.\n");
            failed = true;
            break;
          }
          if (opt.virtual_display) SaveSize(pipe->w, pipe->h);
          if (opt.bitrate_mbps == before.bitrate_mbps) rate_reset = true;  // codificador nuevo: tasa desde abajo
        }
        if (config_seq.load() == applied_seq) reconfiguring = false;
        if (udp.has_peer()) {
          std::lock_guard<std::mutex> lock(info_mutex);
          udp.SendToPeer(dlp::MakeControlPacket(dlp::PacketType::kStreamInfo, info_payload.data(),
                                                info_payload.size(), NowUs()));
        }
        enc_keyframe();
        continue;
      }

      ID3D11Texture2D* frame = nullptr;
      // Sin nada pendiente se espera más a la siguiente imagen: sin cliente o con el escritorio quieto el
      // bucle apenas despierta. Con algo pendiente, justo hasta el siguiente hueco de envío.
      UINT timeout_ms = udp.has_peer() ? 16 : 100;
      if (udp.has_peer() && pipe->converter.has_image()) {
        uint64_t due = 0;
        if (dirty || flush_left > 0 || pipe->encoder->keyframe_pending()) {
          due = last_submit_us + min_interval_us;
        } else if (refine_left > 0) {
          due = last_submit_us + kRefineIntervalUs;
        }
        if (due) {
          const uint64_t t = NowUs();
          timeout_ms = due > t ? static_cast<UINT>(std::min<uint64_t>((due - t + 999) / 1000, 16)) : 0;
        }
      }
      auto result = pipe->capture.Acquire(timeout_ms, &frame);
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
        if (!rebuild(false, opt.virtual_width, opt.virtual_height)) {
          std::fprintf(stderr, "Error: no se pudo reiniciar la captura con la nueva resolución.\n");
          failed = true;
          break;
        }
        if (opt.virtual_display) SaveSize(pipe->w, pipe->h);
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
      if (udp.ExpirePeer(std::chrono::seconds(5))) {
        touch.ReleaseAll();
        last_client_ip = 0;
        rtt_us = 0;
        tablet_received = tablet_lost = 0;
        std::printf("Cliente desconectado (sin respuesta durante 5 s). Esperando cliente.\n");
      }

      // Cada imagen nueva del escritorio solo se copia (hay que hacerlo antes de soltarla) y queda como
      // «última»; así la última actualización antes de quedarse quieto nunca se pierde por el límite de fps.
      // El cursor y la conversión a NV12 se hacen solo cuando el frame va al codificador: con el escritorio
      // a 120-144 Hz, el cursor moviéndose o sin cliente conectado ya no se convierten imágenes que nadie ve.
      if (result == DesktopCapture::Result::kFrame) {
        pipe->converter.Store(frame);
        pipe->capture.Release();
        dirty = true;
        ++captured;
      } else {
        pipe->capture.Release();
        // Solo se movió el cursor: se redibujará sobre la última imagen.
        if (result == DesktopCapture::Result::kCursorMoved && opt.cursor && pipe->converter.has_image()) dirty = true;
      }

      // Se envía la última imagen si es nueva y toca por fps. Tras la última imagen nueva se repite
      // 2 veces: el codificador de hardware retiene un par de frames y, sin nuevas entradas, la
      // última actualización se quedaba dentro durante cientos de ms (imagen «pegada»). Después se
      // repite unas pocas veces más, despacio: con CBR el codificador gasta esos bits en afinar la imagen
      // quieta (texto nítido tras desplazar o arrastrar una ventana) en vez de dejarla como salió en movimiento.
      const uint64_t now = NowUs();
      const bool refine_due = refine_left > 0 && now - last_submit_us >= kRefineIntervalUs;
      if (udp.has_peer() && pipe->converter.has_image() && now - last_submit_us >= min_interval_us &&
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
        if (opt.status) {
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
          std::printf("@estado enlace=%s ip=%s capturados=%.1f codificados=%.1f mbps=%.1f cod_ms=%.1f rtt_ms=%.1f "
                      "recibidos=%u perdidos=%u reenvios=%llu size=%ux%u fps_max=%u adb=%d adb_pendientes=%d\n",
                      !peer ? "ninguno" : udp.peer_is_usb() ? "usb" : "wifi", ip,
                      (captured - last_captured) / secs, enc / secs, (bytes_sent - last_bytes) * 8 / secs / 1e6,
                      enc ? lat / 1000.0 / enc : 0.0, rtt_us.load() / 1000.0, tablet_received.load(),
                      tablet_lost.load(), static_cast<unsigned long long>(retransmits.load() - last_retransmits),
                      pipe->w, pipe->h, opt.fps, opt.usb ? adb.ready() : -1, opt.usb ? adb.unauthorized() : 0);
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
      }
    }
    // Los hilos usan variables de este bloque: se paran antes de salir de él.
    {
      std::lock_guard<std::mutex> lock(enc_mutex);
      enc_ptr = nullptr;
    }
    if (pipe && pipe->encoder) pipe->encoder->Stop();
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
