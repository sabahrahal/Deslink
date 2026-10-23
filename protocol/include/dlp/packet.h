// Protocolo de Extendesk (DLP, por su nombre original «DeskLink Protocol») v1: formato de paquetes. Ver docs/protocol-dlp.md.
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
  kInputMode = 13,       // cliente -> host: u8 modo pedido (0 táctil, 1 ratón); host -> cliente: u8 pedido, u8 en uso
  kAudio = 14,           // host -> cliente: sonido del PC en Opus (frame_id = número de trozo de 10 ms), ver MakeAudioPacket
  kAudioControl = 15,    // cliente -> host: u8 1 = quiero el sonido, 0 = no (se repite cada segundo)
  kBurstData = 8,        // host -> cliente: paquete de relleno de la ráfaga (packet_index/packet_count)
  kDiscover = 12,        // cliente -> host: "¿hay alguien?"; el host responde STREAM_INFO sin empezar a enviar vídeo
  kQualityRequest = 16,  // cliente -> host: con «bajar si la batería no aguanta», lo que pide la batería de la tablet
  kSessionConfig = 17,   // host -> cliente: lo que eligió el PC (ver dlp/session.h); tras STREAM_INFO, al cambiar y cada 2 s
  kClientInfo = 18,      // cliente -> host: texto UTF-8 para el log del PC (modelo y decodificador de la tablet)
  kVideoFec = 19,        // host -> cliente: paridad de un grupo de paquetes VIDEO (ver AddFec)
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
  // Byte 5 (antes reservado, 0): en VIDEO y VIDEO_FEC, cuántos paquetes cubre cada paridad (0 = el frame va sin FEC).
  uint8_t fec_group = 0;
};

// Una paridad lleva delante la XOR de las longitudes (u16): puede pasar 2 bytes de kMaxPayload.
constexpr size_t kMaxFecPayload = kMaxPayload + 2;

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

// Lo que la tablet dice de sí misma en el payload de HELLO: el PC decide con esto y con sus ajustes (dlp/session.h).
// width x height es la pantalla nativa (en horizontal), codec lo que decodifica (1 = solo H.264, 2 = también HEVC) y
// refresh_hz su frecuencia máxima. fps y bitrate_mbps ya no se usan (las tablets de antes pedían ahí sus valores;
// ahora manda el PC). Un HELLO vacío (búsqueda) no cambia nada. Campos nuevos se añaden al final: el host ignora los
// bytes que no conoce y trata como 0 los que falten.
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
// RATE_REPORT con el retardo por etapas que mide la tablet: los 8 bytes de siempre más 5 u16 en décimas de ms (total,
// llegar, esperar reenvíos, pantalla y el peor total). Ver docs/protocol-dlp.md.
constexpr size_t kRateReportLatencySize = 18;
constexpr size_t kTouchContactSize = 7;
constexpr size_t kMaxTouchContacts = 10;
constexpr size_t kInputModeRequestSize = 1;  // cliente -> host
constexpr size_t kInputModeStateSize = 2;    // host -> cliente
enum class InputMode : uint8_t { kTouch = 0, kMouse = 1 };

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

constexpr size_t kQualityRequestSize = 6;

// Perfiles de batería. La tablet elige uno (y lo baja o sube sola según la carga por USB) y manda sus
// límites; el host los aplica como techo de sus propias opciones (--fps, --bitrate).
enum class Profile : uint8_t { kMaxQuality = 0, kBalanced = 1, kSaver = 2, kMinimum = 3 };

// Límites que pide la tablet. 0 en un campo = sin límite (lo que diga el host).
struct QualityRequest {
  uint16_t max_fps = 0;
  uint32_t max_bitrate_bps = 0;  // en el cable va en unidades de 100 kbps (u16)
  uint8_t scale_percent = 100;   // resolución de codificación respecto a la capturada (50-100)
  Profile profile = Profile::kBalanced;
};

// Valores de cada perfil (los mismos que PowerProfile.kt en la app).
QualityRequest ProfileDefaults(Profile profile);
const char* ProfileName(Profile profile);

std::vector<uint8_t> EncodeQualityRequest(const QualityRequest& request);
// Valida y acota: fps 0 o 10-240, escala 50-100 (0 cuenta como 100), perfil desconocido = equilibrado.
std::optional<QualityRequest> DecodeQualityRequest(const uint8_t* payload, size_t size);

// Tamaño de codificación para una captura de width x height al percent %: redondeado a par (lo exigen
// NV12 y los codificadores) y nunca por debajo de 640 de ancho (salvo que la captura sea menor).
void ScaledSize(uint32_t width, uint32_t height, uint8_t percent, uint32_t* out_width, uint32_t* out_height);

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

// Corrección de errores para WiFi: tras cada grupo de [group] paquetes de un frame (ya troceado con PacketizeFrame) se
// inserta un VIDEO_FEC con su paridad: la XOR de sus payloads (rellenos con ceros) y de sus longitudes. Con ella la
// tablet rehace un paquete perdido del grupo sin pedirlo y esperar una ida y vuelta. VIDEO_FEC: packet_index = número
// de grupo, packet_count = paquetes del frame, fec_group = group; payload = u16 XOR de longitudes + XOR de datos.
// Marca también los VIDEO con fec_group. group < 2 los deja como estaban.
void AddFec(std::vector<std::vector<uint8_t>>* packets, uint8_t group);

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

std::vector<uint8_t> EncodeHelloConfig(const HelloConfig& config);
// nullopt si el payload está vacío (HELLO antiguo o de búsqueda): no hay nada que aplicar.
std::optional<HelloConfig> DecodeHelloConfig(const uint8_t* payload, size_t size);

}  // namespace dlp
