// EDID mínimo (1.4, 128 bytes) del monitor virtual. Lleva el nombre «Extendesk» (para que el host lo
// encuentre) y el modo pedido escrito como número de serie, «2944x1840@60», para recuperarlo al
// analizar la descripción del monitor (IddCx no dice a qué monitor pertenece cada EDID).
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace extendesk {

struct Mode {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t refresh = 0;
};

using Edid = std::array<uint8_t, 128>;

inline void WriteTextDescriptor(uint8_t* d, uint8_t tag, const char* text) {
  std::memset(d, 0, 18);
  d[3] = tag;
  size_t n = std::strlen(text);
  if (n > 13) n = 13;
  std::memcpy(d + 5, text, n);
  if (n < 13) {
    d[5 + n] = 0x0A;
    for (size_t i = n + 1; i < 13; ++i) d[5 + i] = 0x20;
  }
}

inline Edid BuildEdid(const Mode& m) {
  Edid e{};
  const uint8_t header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
  std::memcpy(e.data(), header, 8);
  e[8] = 0x11;  // fabricante «DLK»
  e[9] = 0x8B;
  e[10] = 0x01;  // código de producto
  e[11] = 0x00;
  e[12] = 0x01;  // número de serie binario
  e[16] = 1;     // semana
  e[17] = 36;    // año 2026
  e[18] = 1;     // EDID 1.4
  e[19] = 4;
  e[20] = 0xA5;  // digital, 8 bits por color, DisplayPort
  e[21] = 0;     // tamaño desconocido
  e[22] = 0;
  e[23] = 0x78;  // gamma 2.2
  e[24] = 0x06;  // sRGB, la primera descripción es el modo preferido
  const uint8_t chroma[10] = {0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54};  // sRGB
  std::memcpy(e.data() + 25, chroma, 10);
  for (int i = 38; i < 54; ++i) e[i] = 0x01;  // sin tiempos estándar

  // Modo preferido (borrado reducido). El reloj de píxel del EDID no pasa de 655 MHz: si el modo
  // pedido no cabe, aquí se baja la frecuencia; los modos reales los da el driver, no este bloque.
  const uint32_t hblank = 80, vblank = 30;
  const uint32_t htotal = m.width + hblank, vtotal = m.height + vblank;
  uint32_t hz = m.refresh ? m.refresh : 60;
  while (hz > 30 && static_cast<uint64_t>(htotal) * vtotal * hz / 10000 > 0xFFFF) hz /= 2;
  const uint32_t clock = static_cast<uint32_t>(static_cast<uint64_t>(htotal) * vtotal * hz / 10000);
  uint8_t* d = e.data() + 54;
  d[0] = clock & 0xFF;
  d[1] = (clock >> 8) & 0xFF;
  d[2] = m.width & 0xFF;
  d[3] = hblank & 0xFF;
  d[4] = static_cast<uint8_t>(((m.width >> 8) & 0x0F) << 4 | ((hblank >> 8) & 0x0F));
  d[5] = m.height & 0xFF;
  d[6] = vblank & 0xFF;
  d[7] = static_cast<uint8_t>(((m.height >> 8) & 0x0F) << 4 | ((vblank >> 8) & 0x0F));
  d[8] = 48;  // hsync offset
  d[9] = 32;  // hsync ancho
  d[10] = (3 << 4) | 5;  // vsync offset 3, ancho 5
  d[11] = 0;
  d[17] = 0x1E;  // digital separado, sincronías positivas

  char serial[16];
  std::snprintf(serial, sizeof(serial), "%ux%u@%u", m.width, m.height, m.refresh);
  WriteTextDescriptor(e.data() + 72, 0xFC, "Extendesk");
  WriteTextDescriptor(e.data() + 90, 0xFF, serial);
  std::memset(e.data() + 108, 0, 18);
  e[108 + 3] = 0x10;  // descriptor vacío

  e[126] = 0;  // sin extensiones
  uint8_t sum = 0;
  for (int i = 0; i < 127; ++i) sum = static_cast<uint8_t>(sum + e[i]);
  e[127] = static_cast<uint8_t>(0x100 - sum);
  return e;
}

// Lee el modo escrito en el número de serie. Devuelve false si el EDID no es de Extendesk.
inline bool ParseEdidMode(const uint8_t* data, size_t size, Mode* out) {
  if (size < 128) return false;
  for (int off = 54; off <= 108; off += 18) {
    const uint8_t* d = data + off;
    if (d[0] != 0 || d[1] != 0 || d[3] != 0xFF) continue;
    char text[14] = {};
    std::memcpy(text, d + 5, 13);
    unsigned w = 0, h = 0, r = 0;
    if (std::sscanf(text, "%ux%u@%u", &w, &h, &r) != 3) return false;
    if (w < 640 || h < 480 || w > 7680 || h > 4320 || r < 24 || r > 240) return false;
    out->width = w;
    out->height = h;
    out->refresh = r;
    return true;
  }
  return false;
}

}  // namespace extendesk
