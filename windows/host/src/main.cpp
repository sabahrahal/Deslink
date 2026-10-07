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
#include <string>
#include <vector>

#include "capture.h"
#include "converter.h"
#include "dlp/packet.h"
#include "encoder.h"
#include "gpu.h"
#include "history.h"
#include "udp.h"

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

struct Options {
  uint16_t port = dlp::kDefaultPort;
  int output = 0;
  UINT fps = 60;
  UINT bitrate_mbps = 50;
  VideoCodec codec = VideoCodec::kHevc;
  UINT pace_mbps = 100;  // reparte cada frame a esta tasa; 0 = de golpe (el anclaje USB pierde ráfagas)
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
    } else if (a == "--codec") {
      auto v = next("--codec"); if (!v) return false;
      o->codec = std::string(v) == "h264" ? VideoCodec::kH264 : VideoCodec::kHevc;
    } else {
      std::fprintf(stderr,
                   "Uso: desklink-host [--port 47800] [--output 0] [--fps 60] [--bitrate 50] "
                   "[--codec hevc|h264] [--pace Mbps]\n");
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

  Gpu gpu;
  DesktopCapture capture;
  Nv12Converter converter;
  MfEncoder encoder;
  UdpEndpoint udp;
  int exit_code = 1;

  std::atomic<uint32_t> next_frame_id{1};
  std::atomic<uint64_t> bytes_sent{0}, frames_encoded{0}, encode_latency_us{0}, keyframes_sent{0}, retransmits{0};
  PacketHistory history;

  do {
    if (!CreateGpuForOutput(opt.output, &gpu)) break;
    if (!capture.Init(gpu)) break;
    const UINT w = capture.width(), h = capture.height();
    std::printf("Captura: %ux%u a %u fps, %u Mbps, %s\n", w, h, opt.fps, opt.bitrate_mbps,
                opt.codec == VideoCodec::kHevc ? "HEVC" : "H.264");
    if (!converter.Init(gpu, w, h, 24)) break;

    dlp::StreamInfo info;
    info.width = static_cast<uint16_t>(w);
    info.height = static_cast<uint16_t>(h);
    info.fps = static_cast<uint16_t>(opt.fps);
    info.codec = opt.codec == VideoCodec::kHevc ? dlp::Codec::kHevc : dlp::Codec::kH264;
    const auto info_payload = dlp::EncodeStreamInfo(info);

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
    if (!encoder.Init(gpu, opt.codec, w, h, opt.fps, opt.bitrate_mbps * 1'000'000u, on_encoded)) {
      std::fprintf(stderr, "Error: no se pudo iniciar el codificador (arriba está el motivo).\n");
      break;
    }

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
      encoder.SetBitrate(current_bps);
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
            encoder.SetBitrate(current_bps);
          }
          udp.SetPeer(from);
          udp.SendTo(from, dlp::MakeControlPacket(dlp::PacketType::kStreamInfo, info_payload.data(),
                                                  info_payload.size(), NowUs()));
          last_keyframe_us = NowUs();
          encoder.RequestKeyframe();
          break;
        case dlp::PacketType::kKeyframeRequest: {
          // Un frame clave es varias veces más grande que uno normal: si la red pierde paquetes y
          // se piden uno tras otro, la propia petición mantiene la imagen congelada. Se limita a 1/s.
          const uint64_t now = NowUs();
          decrease_rate(now, "la tablet pide frame clave");
          if (now - last_keyframe_us.load() >= 1'000'000) {
            last_keyframe_us = now;
            encoder.RequestKeyframe();
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
              encoder.SetBitrate(current_bps);
              std::printf("Tasa adaptada a %.1f Mbps (enlace limpio)\n", current_bps / 1e6);
            }
          }
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
    if (!udp.Bind(opt.port, on_packet)) break;
    std::printf("Escuchando en UDP %u. Abre DeskLink en el móvil e indica la IP de este PC.\n",
                opt.port);

    const uint64_t min_interval_us = 1'000'000ull / opt.fps;
    uint64_t last_submit_us = 0;
    ID3D11Texture2D* last_nv12 = nullptr;
    bool dirty = false;      // hay una imagen convertida que aún no se envió
    int flush_left = 0;      // repeticiones pendientes para vaciar el codificador
    uint64_t captured = 0, last_stats_us = NowUs();
    uint64_t convert_us = 0, last_convert = 0, last_fed = 0, last_mailbox = 0;
    uint64_t last_bytes = 0, last_encoded = 0, last_latency = 0, last_captured = 0, last_keyframes = 0, last_retransmits = 0;
    bool failed = false;

    while (g_running) {
      ID3D11Texture2D* frame = nullptr;
      auto result = capture.Acquire(10, &frame);
      if (result == DesktopCapture::Result::kError) {
        failed = true;
        break;
      }

      if (udp.ExpirePeer(std::chrono::seconds(5))) {
        last_client_ip = 0;
        std::printf("Cliente desconectado (sin respuesta durante 5 s). Esperando cliente.\n");
      }

      // Cada imagen nueva del escritorio se convierte siempre (es barato) y queda como «última»;
      // así la última actualización antes de quedarse quieto nunca se pierde por el límite de fps.
      if (result == DesktopCapture::Result::kFrame) {
        const uint64_t conv_start = NowUs();
        ID3D11Texture2D* nv12 = converter.Convert(frame);
        convert_us += NowUs() - conv_start;
        capture.Release();
        if (!nv12) {
          failed = true;
          break;
        }
        last_nv12 = nv12;
        dirty = true;
        ++captured;
      } else {
        capture.Release();
      }

      // Se envía la última imagen si es nueva y toca por fps. Tras la última imagen nueva se repite
      // 2 veces: el codificador de hardware retiene un par de frames y, sin nuevas entradas, la
      // última actualización se quedaba dentro durante cientos de ms (imagen «pegada»).
      const uint64_t now = NowUs();
      if (udp.has_peer() && last_nv12 && now - last_submit_us >= min_interval_us &&
          (dirty || flush_left > 0 || encoder.keyframe_pending())) {
        encoder.Submit(last_nv12, now);
        last_submit_us = now;
        if (dirty) {
          dirty = false;
          flush_left = 2;
        } else if (flush_left > 0) {
          --flush_left;
        }
      }

      const uint64_t t = NowUs();
      if (t - last_stats_us >= 1'000'000) {
        const double secs = (t - last_stats_us) / 1e6;
        const uint64_t enc = frames_encoded - last_encoded;
        const uint64_t lat = encode_latency_us - last_latency;
        std::printf("capturados %.0f/s | codificados %.0f/s | %.1f Mbps | codificación %.1f ms | claves %llu | conv %.1f ms | buzón %.1f ms | errores de envío %llu | reenvíos %llu%s\n",
                    (captured - last_captured) / secs, enc / secs,
                    (bytes_sent - last_bytes) * 8 / secs / 1e6, enc ? lat / 1000.0 / enc : 0.0,
                    static_cast<unsigned long long>(keyframes_sent.load() - last_keyframes),
                    captured > last_captured ? (convert_us - last_convert) / 1000.0 / (captured - last_captured) : 0.0,
                    encoder.fed_frames > last_fed ? (encoder.mailbox_wait_us - last_mailbox) / 1000.0 / (encoder.fed_frames - last_fed) : 0.0,
                    static_cast<unsigned long long>(udp.send_errors()),
                    static_cast<unsigned long long>(retransmits.load() - last_retransmits),
                    udp.has_peer() ? "" : " | esperando cliente");
        last_stats_us = t;
        last_bytes = bytes_sent;
        last_encoded = frames_encoded;
        last_latency = encode_latency_us;
        last_captured = captured;
        last_keyframes = keyframes_sent;
        last_retransmits = retransmits;
        last_convert = convert_us;
        last_fed = encoder.fed_frames;
        last_mailbox = encoder.mailbox_wait_us;
      }
    }
    // Los hilos usan variables de este bloque: se paran antes de salir de él.
    encoder.Stop();
    udp.Stop();
    exit_code = failed ? 1 : 0;
  } while (false);

  encoder.Stop();
  udp.Stop();
  capture.Release();
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
