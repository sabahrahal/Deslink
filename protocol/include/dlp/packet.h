// DeskLink Protocol (DLP) v1: formato de paquetes. Ver docs/protocol-dlp.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
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
  kBurstData = 8,        // host -> cliente: paquete de relleno de la ráfaga (packet_index/packet_count)
  kDiscover = 12,        // cliente -> host: "¿hay alguien?"; el host responde STREAM_INFO sin empezar a enviar vídeo
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

// Bits de StreamInfo::host_flags.
enum HostFlags : uint8_t {
  kHostAcceptsHelloConfig = 1 << 0,  // el host aplica la configuración que la tablet manda en HELLO
};

struct StreamInfo {
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t fps = 0;
  Codec codec = Codec::kHevc;
  // Extensión opcional (los clientes antiguos solo leen los 7 primeros bytes).
  uint8_t host_flags = 0;
  std::string host_name;  // nombre del PC, UTF-8, como mucho kMaxHostName bytes
};

// Lo que la tablet pide en el payload de HELLO. 0 en un campo = "lo que diga el host".
// Un HELLO vacío (clientes antiguos y búsqueda) no cambia nada. Campos nuevos se añaden al final:
// el host ignora los bytes que no conoce y trata como 0 los que falten.
struct HelloConfig {
  uint16_t width = 0;         // tamaño pedido para el monitor virtual, en píxeles (horizontal)
  uint16_t height = 0;
  uint16_t fps = 0;
  uint16_t bitrate_mbps = 0;
  uint8_t codec = 0;          // 0 = el del host, 1 = H.264, 2 = HEVC
  uint8_t reserved = 0;
  uint16_t density_dpi = 0;   // informativo: densidad de la pantalla de la tablet
  uint16_t refresh_hz = 0;    // informativo: frecuencia máxima de la pantalla de la tablet

  bool operator==(const HelloConfig& o) const {
    return width == o.width && height == o.height && fps == o.fps && bitrate_mbps == o.bitrate_mbps &&
           codec == o.codec && density_dpi == o.density_dpi && refresh_hz == o.refresh_hz;
  }
  bool operator!=(const HelloConfig& o) const { return !(*this == o); }
};

constexpr size_t kStreamInfoSize = 7;
constexpr size_t kMaxHostName = 64;
constexpr size_t kHelloConfigSize = 14;
constexpr size_t kBurstRequestSize = 6;
constexpr size_t kRateReportSize = 4;
constexpr size_t kTouchContactSize = 7;
constexpr size_t kMaxTouchContacts = 10;

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

std::vector<uint8_t> EncodeStreamInfo(const StreamInfo& info);
std::optional<StreamInfo> DecodeStreamInfo(const uint8_t* payload, size_t size);

std::vector<uint8_t> EncodeHelloConfig(const HelloConfig& config);
// nullopt si el payload está vacío (HELLO antiguo o de búsqueda): no hay nada que aplicar.
std::optional<HelloConfig> DecodeHelloConfig(const uint8_t* payload, size_t size);

}  // namespace dlp
