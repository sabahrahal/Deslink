// DeskLink Protocol (DLP) v1: formato de paquetes. Ver docs/protocol-dlp.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace dlp {

constexpr uint16_t kMagic = 0x4C44;  // "DL" en little-endian
constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderSize = 24;
constexpr size_t kMaxPacketSize = 1200;
constexpr size_t kMaxPayload = kMaxPacketSize - kHeaderSize;
constexpr uint16_t kDefaultPort = 47800;

enum class PacketType : uint8_t {
  kVideo = 1,            // host -> cliente: un trozo de un frame codificado
  kHello = 2,            // cliente -> host: "estoy aquí, empieza a enviar"
  kKeyframeRequest = 3,  // cliente -> host: pide un frame clave
  kPing = 4,             // cliente -> host: medir RTT
  kPong = 5,             // host -> cliente: respuesta al ping (devuelve el timestamp)
  kStreamInfo = 6,       // host -> cliente: resolución, fps y códec
  kBurstRequest = 7,     // cliente -> host: pide una ráfaga de prueba (count u16, size u16, pace_mbps u16)
  kRateReport = 9,       // cliente -> host: frames recibidos y perdidos en el último segundo (u16, u16)
  kNack = 10,            // cliente -> host: reenvía estos paquetes de un frame (frame_id en la cabecera; payload: u16 por índice)
  kTouch = 11,           // cliente -> host: estado táctil completo (u8 n, n x {u8 id, u16 x, u16 y, u16 presión}); x,y en 0..65535
  kAudio = 12,           // host -> cliente: sonido del PC en Opus (frame_id = número de trozo de 10 ms), ver MakeAudioPacket
  kAudioControl = 13,    // cliente -> host: u8 1 = quiero el sonido, 0 = no (se repite cada segundo)
  kBurstData = 8,        // host -> cliente: paquete de relleno de la ráfaga (packet_index/packet_count)
};

enum Flags : uint8_t {
  kFlagKeyframe = 1 << 0,
  kFlagLastPacket = 1 << 1,
};

enum class Codec : uint8_t { kH264 = 1, kHevc = 2 };

struct Header {
  PacketType type = PacketType::kVideo;
  uint8_t flags = 0;
  uint32_t frame_id = 0;
  uint16_t packet_index = 0;
  uint16_t packet_count = 1;
  uint64_t timestamp_us = 0;  // reloj del emisor, en microsegundos
  uint16_t payload_size = 0;
};

struct StreamInfo {
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t fps = 0;
  Codec codec = Codec::kHevc;
};

constexpr size_t kStreamInfoSize = 7;
constexpr size_t kBurstRequestSize = 6;
constexpr size_t kRateReportSize = 4;
constexpr size_t kTouchContactSize = 7;
constexpr size_t kMaxTouchContacts = 10;

// Sonido: Opus a 48 kHz estéreo, un trozo cada 10 ms.
constexpr uint32_t kAudioSampleRate = 48000;
constexpr uint32_t kAudioChannels = 2;
constexpr uint32_t kAudioFrameSamples = 480;
constexpr size_t kAudioControlSize = 1;

// Un trozo de sonido dentro de un paquete AUDIO. `data` apunta dentro del paquete.
struct AudioFrameRef {
  uint32_t seq = 0;
  const uint8_t* data = nullptr;
  size_t size = 0;
};

// Escribe la cabecera en kHeaderSize bytes.
void WriteHeader(const Header& header, uint8_t* out);

// Devuelve nullopt si falta tamaño, el magic o la versión no coinciden, o el
// payload declarado no cabe en el paquete.
std::optional<Header> ParseHeader(const uint8_t* data, size_t size);

// Parte un frame codificado en paquetes listos para enviar. Devuelve vacío si el
// frame está vacío o necesita más de 65535 paquetes.
std::vector<std::vector<uint8_t>> PacketizeFrame(const uint8_t* data, size_t size,
                                                 uint32_t frame_id, bool keyframe,
                                                 uint64_t timestamp_us);

std::vector<uint8_t> MakeControlPacket(PacketType type, const uint8_t* payload,
                                       size_t payload_size, uint64_t timestamp_us);

// Paquete AUDIO con el trozo `seq` y, si cabe, también el anterior (`prev`, puede ser nulo): si se
// pierde un paquete por WiFi, el siguiente trae su trozo. Payload: u8 n (1 o 2) y n x {u16 tamaño, bytes};
// la entrada i es el trozo seq - i. Devuelve vacío si `cur` está vacío o no cabe.
std::vector<uint8_t> MakeAudioPacket(uint32_t seq, uint64_t timestamp_us, const uint8_t* cur, size_t cur_size,
                                     const uint8_t* prev, size_t prev_size);
// Trozos de un paquete AUDIO ya validado con ParseHeader; vacío si el payload no es válido.
std::vector<AudioFrameRef> ParseAudioPacket(const Header& header, const uint8_t* payload);

std::vector<uint8_t> EncodeStreamInfo(const StreamInfo& info);
std::optional<StreamInfo> DecodeStreamInfo(const uint8_t* payload, size_t size);

}  // namespace dlp
