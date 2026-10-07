#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "dlp/packet.h"

namespace dlp {

struct Frame {
  uint32_t frame_id = 0;
  bool keyframe = false;
  uint64_t timestamp_us = 0;
  std::vector<uint8_t> data;
};

// Reensambla frames a partir de paquetes de vídeo. Sin colas: al completarse un
// frame se descartan todos los anteriores que sigan incompletos. Cada pérdida
// activa needs_keyframe() hasta que llegue un frame clave completo.
class FrameReassembler {
 public:
  // Procesa un paquete completo (cabecera + payload). Devuelve el frame si este
  // paquete lo completa.
  std::optional<Frame> Push(const uint8_t* packet, size_t size);

  bool needs_keyframe() const { return needs_keyframe_; }
  uint64_t frames_completed() const { return frames_completed_; }
  uint64_t frames_dropped() const { return frames_dropped_; }

 private:
  struct Pending {
    uint16_t packet_count = 0;
    uint16_t received = 0;
    bool keyframe = false;
    uint64_t timestamp_us = 0;
    size_t last_payload = 0;
    bool have_last = false;
    std::vector<bool> have;
    std::vector<uint8_t> data;
  };

  static constexpr size_t kMaxPending = 4;

  std::map<uint32_t, Pending> pending_;
  std::optional<uint32_t> last_delivered_;
  bool needs_keyframe_ = true;  // al empezar hace falta un frame clave
  uint64_t frames_completed_ = 0;
  uint64_t frames_dropped_ = 0;
};

}  // namespace dlp
