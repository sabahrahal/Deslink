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

void OnSignal(int) { g_running = false; }

uint64_t NowUs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// Espera (durmiendo y luego girando) hasta el instante target_us.
void WaitUntil(uint64_t target_us) {
  for (;;) {
    const uint64_t now = NowUs();
    if (now >= target_us) return;
    if (target_us - now > 1500) {
      Sleep(1);
    } else {
      YieldProcessor();
    }
  }
}

// Envía los paquetes uno tras otro; con pace_mbps > 0 reparte el envío para no saturar
// los búferes de enlaces lentos o con ráfagas mal toleradas (p. ej. el anclaje USB).
template <typename SendFn>
void SendPaced(const std::vector<std::vector<uint8_t>>& packets, uint32_t pace_mbps, SendFn send) {
  const uint64_t start = NowUs();
  uint64_t bits = 0;
  for (const auto& p : packets) {
    if (pace_mbps > 0) WaitUntil(start + bits / pace_mbps);  // bits / (Mbit/s) = µs
    send(p);
    bits += p.size() * 8;
  }
}

// Última resolución del monitor virtual (la que el usuario dejó en Windows), para la próxima vez.
std::wstring SizeFilePath() {
  wchar_t base[MAX_PATH];
  if (!GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH)) return {};
  return std::wstring(base) + L"\\DeskLink\\virtual-size.txt";
}

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

struct Options {
  uint16_t port = dlp::kDefaultPort;
  int output = 0;
  UINT fps = 60;
  UINT bitrate_mbps = 50;
  VideoCodec codec = VideoCodec::kHevc;
  bool cursor = true;    // --no-cursor: no dibuja el cursor en la imagen
  bool mouse = false;    // --mouse: el táctil mueve el ratón (más compatible, un solo dedo)
  bool touch = true;     // --no-touch desactiva el control táctil
  UINT pace_mbps = 100;  // reparte cada frame a esta tasa; 0 = de golpe (el anclaje USB pierde ráfagas)
  bool virtual_display = false;  // --virtual: crea un monitor virtual (driver desklink-idd) y captura ese
  UINT virtual_width = 2944;     // --size: resolución del monitor virtual (por defecto, la Lenovo Idea Tab Pro)
  UINT virtual_height = 1840;
  bool size_given = false;       // si no se pasa --size, se usa la última resolución recordada
};

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
    } else if (a == "--no-cursor") {
      o->cursor = false;
    } else if (a == "--mouse") {
      o->mouse = true;
    } else if (a == "--no-touch") {
      o->touch = false;
    } else if (a == "--codec") {
      auto v = next("--codec"); if (!v) return false;
      o->codec = std::string(v) == "h264" ? VideoCodec::kH264 : VideoCodec::kHevc;
    } else {
      std::fprintf(stderr,
                   "Uso: desklink-host [--port 47800] [--output 0] [--fps 60] [--bitrate 50] "
                   "[--codec hevc|h264] [--pace Mbps] [--no-touch] [--mouse] [--no-cursor] "
                   "[--virtual [--size 2944x1840]]\n");
      return false;
    }
  }
  return o->fps > 0 && o->fps <= 240 && o->bitrate_mbps > 0;
}

}  // namespace

int Run(int argc, char** argv) {
  Options opt;
  if (!ParseArgs(argc, argv, &opt)) return 2;

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  MFStartup(MF_VERSION);
  timeBeginPeriod(1);  // temporizadores de 1 ms
  SetConsoleOutputCP(CP_UTF8);  // los mensajes llevan tildes
  SetProcessDPIAware();  // coordenadas en píxeles reales, para que los toques caigan donde toca

  VirtualDisplay virtual_display;  // se declara antes que el resto: el monitor se quita lo último
  std::unique_ptr<Pipeline> pipe;
  UdpEndpoint udp;
  int exit_code = 1;

  std::atomic<uint32_t> next_frame_id{1};
  std::atomic<uint64_t> bytes_sent{0}, frames_encoded{0}, encode_latency_us{0}, keyframes_sent{0}, retransmits{0}, touch_packets{0};
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

  auto on_encoded = [&](const uint8_t* data, size_t size, bool keyframe, uint64_t ts_us) {
    auto packets = dlp::PacketizeFrame(data, size, next_frame_id++, keyframe, ts_us);
    for (const auto& pkt : packets) {
      auto ph = dlp::ParseHeader(pkt.data(), pkt.size());
      if (ph) history.Put(ph->frame_id, ph->packet_index, pkt);
    }
    SendPaced(packets, opt.pace_mbps, [&](const std::vector<uint8_t>& p) { udp.SendToPeer(p); });
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
    const uint32_t max_bps = opt.bitrate_mbps * 1'000'000u;
    const uint32_t min_bps = std::max<uint32_t>(max_bps / 10, 3'000'000u);
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
    auto on_packet = [&](const uint8_t* data, size_t size, const sockaddr_in& from) {
      auto header = dlp::ParseHeader(data, size);
      if (!header) return;
      switch (header->type) {
        case dlp::PacketType::kHello:
          if (last_client_ip.exchange(from.sin_addr.s_addr) != from.sin_addr.s_addr) {
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            std::printf("Cliente conectado: %s:%u\n", ip, ntohs(from.sin_port));
            current_bps = std::min(max_bps, kStartBps);
            ceiling_bps = max_bps;
            last_decrease_us = last_increase_us = NowUs();
            enc_bitrate(current_bps);
          }
          udp.SetPeer(from);
          {
            std::lock_guard<std::mutex> lock(info_mutex);
            udp.SendTo(from, dlp::MakeControlPacket(dlp::PacketType::kStreamInfo, info_payload.data(),
                                                    info_payload.size(), NowUs()));
          }
          last_keyframe_us = NowUs();
          enc_keyframe();
          break;
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
        default:
          break;
      }
    };
    if (opt.touch) {
      // Los toques caen sobre el monitor que se captura (el virtual con --virtual).
      const RECT r = pipe->gpu.desktop_rect;
      touch_ready = touch.Init(r, opt.mouse);
      std::printf(touch_ready ? (opt.mouse ? "Táctil en modo ratón (pantalla de %ldx%ld en %ld,%ld).\n"
                                           : "Táctil activado (pantalla de %ldx%ld en %ld,%ld).\n")
                              : "Táctil NO disponible.\n",
                  r.right - r.left, r.bottom - r.top, r.left, r.top);
    }
    if (!udp.Bind(opt.port, on_packet)) break;
    std::printf("Escuchando en UDP %u. Abre DeskLink en el móvil e indica la IP de este PC.\n",
                opt.port);

    const uint64_t min_interval_us = 1'000'000ull / opt.fps;
    uint64_t last_submit_us = 0;
    uint64_t last_rect_check_us = 0;
    ID3D11Texture2D* last_nv12 = nullptr;
    bool dirty = false;      // hay una imagen convertida que aún no se envió
    int flush_left = 0;      // repeticiones pendientes para vaciar el codificador
    uint64_t captured = 0, last_stats_us = NowUs();
    uint64_t convert_us = 0, last_convert = 0, last_fed = 0, last_mailbox = 0;
    uint64_t last_bytes = 0, last_encoded = 0, last_latency = 0, last_captured = 0, last_keyframes = 0, last_retransmits = 0, last_touch = 0;
    bool failed = false;

    while (g_running) {
      ID3D11Texture2D* frame = nullptr;
      auto result = pipe->capture.Acquire(10, &frame);
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
        {
          std::lock_guard<std::mutex> lock(enc_mutex);
          enc_ptr = nullptr;
        }
        pipe.reset();
        pipe = build(50);
        if (!pipe) {
          std::fprintf(stderr, "Error: no se pudo reiniciar la captura con la nueva resolución.\n");
          failed = true;
          break;
        }
        {
          std::lock_guard<std::mutex> lock(enc_mutex);
          enc_ptr = pipe->encoder.get();
        }
        if (opt.virtual_display) SaveSize(pipe->w, pipe->h);
        if (touch_ready) touch.SetRect(pipe->gpu.desktop_rect);
        last_nv12 = nullptr;
        dirty = false;
        flush_left = 0;
        last_fed = last_mailbox = 0;
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
        std::printf("Cliente desconectado (sin respuesta durante 5 s). Esperando cliente.\n");
      }

      // Cada imagen nueva del escritorio se convierte siempre (es barato) y queda como «última»;
      // así la última actualización antes de quedarse quieto nunca se pierde por el límite de fps.
      if (result == DesktopCapture::Result::kFrame) {
        const uint64_t conv_start = NowUs();
        ID3D11Texture2D* nv12 = pipe->converter.Convert(frame);
        convert_us += NowUs() - conv_start;
        pipe->capture.Release();
        if (!nv12) {
          failed = true;
          break;
        }
        last_nv12 = nv12;
        dirty = true;
        ++captured;
      } else if (result == DesktopCapture::Result::kCursorMoved) {
        // Solo se movió el cursor: se redibuja sobre la última imagen.
        pipe->capture.Release();
        if (ID3D11Texture2D* nv12 = pipe->converter.Refresh()) {
          last_nv12 = nv12;
          dirty = true;
        }
      } else {
        pipe->capture.Release();
      }

      // Se envía la última imagen si es nueva y toca por fps. Tras la última imagen nueva se repite
      // 2 veces: el codificador de hardware retiene un par de frames y, sin nuevas entradas, la
      // última actualización se quedaba dentro durante cientos de ms (imagen «pegada»).
      const uint64_t now = NowUs();
      if (udp.has_peer() && last_nv12 && now - last_submit_us >= min_interval_us &&
          (dirty || flush_left > 0 || pipe->encoder->keyframe_pending())) {
        pipe->encoder->Submit(last_nv12, now);
        last_submit_us = now;
        if (dirty) {
          dirty = false;
          flush_left = 2;
        } else if (flush_left > 0) {
          --flush_left;
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
        const double secs = (t - last_stats_us) / 1e6;
        const uint64_t enc = frames_encoded - last_encoded;
        const uint64_t lat = encode_latency_us - last_latency;
        std::printf("capturados %.0f/s | codificados %.0f/s | %.1f Mbps | codificación %.1f ms | claves %llu | conv %.1f ms | buzón %.1f ms | errores de envío %llu | reenvíos %llu | toques %llu%s\n",
                    (captured - last_captured) / secs, enc / secs,
                    (bytes_sent - last_bytes) * 8 / secs / 1e6, enc ? lat / 1000.0 / enc : 0.0,
                    static_cast<unsigned long long>(keyframes_sent.load() - last_keyframes),
                    captured > last_captured ? (convert_us - last_convert) / 1000.0 / (captured - last_captured) : 0.0,
                    pipe->encoder->fed_frames > last_fed ? (pipe->encoder->mailbox_wait_us - last_mailbox) / 1000.0 / (pipe->encoder->fed_frames - last_fed) : 0.0,
                    static_cast<unsigned long long>(udp.send_errors()),
                    static_cast<unsigned long long>(retransmits.load() - last_retransmits),
                    static_cast<unsigned long long>(touch_packets.load() - last_touch),
                    udp.has_peer() ? "" : " | esperando cliente");
        last_stats_us = t;
        last_bytes = bytes_sent;
        last_encoded = frames_encoded;
        last_latency = encode_latency_us;
        last_captured = captured;
        last_keyframes = keyframes_sent;
        last_retransmits = retransmits;
        last_touch = touch_packets;
        last_convert = convert_us;
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
  DWORD pids[2];
  const bool own_console = GetConsoleProcessList(pids, 2) == 1;
  if (own_console || code != 0) {
    std::printf("\nPulsa Enter para salir.\n");
    std::getchar();
  }
  return code;
}
