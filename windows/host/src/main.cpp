// DeskLink host (fase 1): captura una salida de Windows, la codifica por hardware
// y la envía por UDP al cliente Android que haya enviado HELLO.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmsystem.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "capture.h"
#include "converter.h"
#include "dlp/packet.h"
#include "encoder.h"
#include "gpu.h"
#include "udp.h"

namespace {

std::atomic<bool> g_running{true};

void OnSignal(int) { g_running = false; }

uint64_t NowUs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

struct Options {
  uint16_t port = dlp::kDefaultPort;
  int output = 0;
  UINT fps = 60;
  UINT bitrate_mbps = 50;
  VideoCodec codec = VideoCodec::kHevc;
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
    } else if (a == "--codec") {
      auto v = next("--codec"); if (!v) return false;
      o->codec = std::string(v) == "h264" ? VideoCodec::kH264 : VideoCodec::kHevc;
    } else {
      std::fprintf(stderr,
                   "Uso: desklink-host [--port 47800] [--output 0] [--fps 60] [--bitrate 50] "
                   "[--codec hevc|h264]\n");
      return false;
    }
  }
  return o->fps > 0 && o->fps <= 240 && o->bitrate_mbps > 0;
}

}  // namespace

int main(int argc, char** argv) {
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
  std::atomic<uint64_t> bytes_sent{0}, frames_encoded{0}, encode_latency_us{0};

  do {
    if (!CreateGpuForOutput(opt.output, &gpu)) break;
    if (!capture.Init(gpu)) break;
    const UINT w = capture.width(), h = capture.height();
    std::printf("Captura: %ux%u a %u fps, %u Mbps, %s\n", w, h, opt.fps, opt.bitrate_mbps,
                opt.codec == VideoCodec::kHevc ? "HEVC" : "H.264");
    if (!converter.Init(gpu, w, h)) break;

    dlp::StreamInfo info;
    info.width = static_cast<uint16_t>(w);
    info.height = static_cast<uint16_t>(h);
    info.fps = static_cast<uint16_t>(opt.fps);
    info.codec = opt.codec == VideoCodec::kHevc ? dlp::Codec::kHevc : dlp::Codec::kH264;
    const auto info_payload = dlp::EncodeStreamInfo(info);

    auto on_encoded = [&](const uint8_t* data, size_t size, bool keyframe, uint64_t ts_us) {
      auto packets = dlp::PacketizeFrame(data, size, next_frame_id++, keyframe, ts_us);
      for (const auto& p : packets) udp.SendToPeer(p);
      bytes_sent += size;
      ++frames_encoded;
      const uint64_t now = NowUs();
      if (now > ts_us) encode_latency_us += now - ts_us;
    };
    if (!encoder.Init(gpu, opt.codec, w, h, opt.fps, opt.bitrate_mbps * 1'000'000u, on_encoded)) break;

    auto on_packet = [&](const uint8_t* data, size_t size, const sockaddr_in& from) {
      auto header = dlp::ParseHeader(data, size);
      if (!header) return;
      switch (header->type) {
        case dlp::PacketType::kHello:
          udp.SetPeer(from);
          udp.SendTo(from, dlp::MakeControlPacket(dlp::PacketType::kStreamInfo, info_payload.data(),
                                                  info_payload.size(), NowUs()));
          encoder.RequestKeyframe();
          break;
        case dlp::PacketType::kKeyframeRequest:
          encoder.RequestKeyframe();
          break;
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
    uint64_t captured = 0, last_stats_us = NowUs();
    uint64_t last_bytes = 0, last_encoded = 0, last_latency = 0, last_captured = 0;
    bool failed = false;

    while (g_running) {
      ID3D11Texture2D* frame = nullptr;
      auto result = capture.Acquire(50, &frame);
      if (result == DesktopCapture::Result::kError) {
        failed = true;
        break;
      }

      const uint64_t now = NowUs();
      if (result == DesktopCapture::Result::kFrame && udp.has_peer() &&
          now - last_submit_us >= min_interval_us) {
        ID3D11Texture2D* nv12 = converter.Convert(frame);
        capture.Release();
        if (!nv12) {
          failed = true;
          break;
        }
        last_nv12 = nv12;
        last_submit_us = now;
        ++captured;
        encoder.Submit(nv12, now);
      } else {
        capture.Release();
        // Pantalla quieta pero un cliente pidió un frame clave: se recodifica el último frame.
        if (result == DesktopCapture::Result::kTimeout && last_nv12 && encoder.keyframe_pending()) {
          encoder.Submit(last_nv12, NowUs());
        }
      }

      const uint64_t t = NowUs();
      if (t - last_stats_us >= 1'000'000) {
        const double secs = (t - last_stats_us) / 1e6;
        const uint64_t enc = frames_encoded - last_encoded;
        const uint64_t lat = encode_latency_us - last_latency;
        std::printf("capturados %.0f/s | codificados %.0f/s | %.1f Mbps | codificación %.1f ms%s\n",
                    (captured - last_captured) / secs, enc / secs,
                    (bytes_sent - last_bytes) * 8 / secs / 1e6, enc ? lat / 1000.0 / enc : 0.0,
                    udp.has_peer() ? "" : " | esperando cliente");
        last_stats_us = t;
        last_bytes = bytes_sent;
        last_encoded = frames_encoded;
        last_latency = encode_latency_us;
        last_captured = captured;
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
