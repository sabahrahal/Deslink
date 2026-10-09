// Generado por tools/design/gen_tokens.py desde design/tokens.json. No editar a mano.
#pragma once

#include <windows.h>

namespace tokens {

struct Colors {
  COLORREF background;  // Fondo de pantalla
  COLORREF surface;  // Tarjetas
  COLORREF surface_secondary;  // Campos y opciones sin elegir
  COLORREF separator;  // Bordes y líneas
  COLORREF text;  // Texto y títulos
  COLORREF text_secondary;  // Explicaciones
  COLORREF accent;  // Botón principal, selección, enlaces
  COLORREF on_accent;  // Texto sobre el acento
  COLORREF success;  // Conectado
  COLORREF warning;  // Avisos
  COLORREF error;  // Errores
};

constexpr Colors kLight = {
    RGB(0xF5, 0xF5, 0xF7),  // background
    RGB(0xFF, 0xFF, 0xFF),  // surface
    RGB(0xEC, 0xEC, 0xF0),  // surface_secondary
    RGB(0xD2, 0xD2, 0xD7),  // separator
    RGB(0x1D, 0x1D, 0x1F),  // text
    RGB(0x5E, 0x5E, 0x63),  // text_secondary
    RGB(0x00, 0x66, 0xCC),  // accent
    RGB(0xFF, 0xFF, 0xFF),  // on_accent
    RGB(0x1E, 0x7D, 0x34),  // success
    RGB(0xA8, 0x4B, 0x00),  // warning
    RGB(0xD7, 0x00, 0x15),  // error
};

constexpr Colors kDark = {
    RGB(0x1C, 0x1C, 0x1E),  // background
    RGB(0x2C, 0x2C, 0x2E),  // surface
    RGB(0x3A, 0x3A, 0x3C),  // surface_secondary
    RGB(0x48, 0x48, 0x4A),  // separator
    RGB(0xF5, 0xF5, 0xF7),  // text
    RGB(0xAE, 0xAE, 0xB2),  // text_secondary
    RGB(0x40, 0x9C, 0xFF),  // accent
    RGB(0x00, 0x00, 0x00),  // on_accent
    RGB(0x30, 0xD1, 0x58),  // success
    RGB(0xFF, 0x9F, 0x0A),  // warning
    RGB(0xFF, 0x69, 0x61),  // error
};

// Texto (px a 96 ppp) y peso.
constexpr float kLargeTitlePx = 28;
constexpr int kLargeTitleWeight = 600;
constexpr float kTitlePx = 20;
constexpr int kTitleWeight = 600;
constexpr float kHeadlinePx = 17;
constexpr int kHeadlineWeight = 600;
constexpr float kSectionPx = 13;
constexpr int kSectionWeight = 600;
constexpr float kBodyPx = 15;
constexpr int kBodyWeight = 400;
constexpr float kNotePx = 13;
constexpr int kNoteWeight = 400;
constexpr float kCardRadius = 12;
constexpr float kControlRadius = 8;

}  // namespace tokens
