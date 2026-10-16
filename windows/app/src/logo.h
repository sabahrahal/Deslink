// Generado por tools/design/gen_logo.py. No editar a mano.
// El icono de DeskLink (los dos eslabones) para dibujarlo con GDI+ a cualquier tamaño.
#pragma once

namespace logo {

enum Op { kMove, kLine, kCubic, kClose };
struct Cmd {
  Op op;
  float p[6];  // kMove/kLine: x, y; kCubic: x1, y1, x2, y2, x, y
};

constexpr float kWidth = 1034;
constexpr float kHeight = 662;

// Eslabón de atrás (tokens::kLogoBack).
constexpr Cmd kBack[] = {
    {kMove, {348.0f, 284.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kLine, {348.0f, 176.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kCubic, {348.0f, 78.8f, 426.8f, 0.0f, 524.0f, 0.0f}},
    {kLine, {858.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kCubic, {955.2f, 0.0f, 1034.0f, 78.8f, 1034.0f, 176.0f}},
    {kLine, {1034.0f, 328.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kCubic, {1034.0f, 425.2f, 955.2f, 504.0f, 858.0f, 504.0f}},
    {kLine, {717.0f, 504.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kLine, {717.0f, 381.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kLine, {851.0f, 381.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kCubic, {877.51f, 381.0f, 899.0f, 359.51f, 899.0f, 333.0f}},
    {kLine, {899.0f, 178.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kCubic, {899.0f, 151.49f, 877.51f, 130.0f, 851.0f, 130.0f}},
    {kLine, {536.0f, 130.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kCubic, {509.49f, 130.0f, 488.0f, 151.49f, 488.0f, 178.0f}},
    {kLine, {488.0f, 284.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kClose, {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
};

// Eslabón de delante (tokens::kLogoFront); el segundo trazo es el hueco (relleno alterno).
constexpr Cmd kFront[] = {
    {kMove, {0.0f, 374.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kCubic, {0.0f, 288.4f, 69.4f, 219.0f, 155.0f, 219.0f}},
    {kLine, {509.0f, 219.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kCubic, {553.0f, 219.0f, 588.67f, 234.67f, 616.0f, 266.0f}},
    {kLine, {806.0f, 481.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kCubic, {819.33f, 496.33f, 836.0f, 504.0f, 856.0f, 504.0f}},
    {kLine, {731.0f, 504.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kCubic, {701.67f, 504.0f, 676.67f, 492.67f, 656.0f, 470.0f}},
    {kLine, {614.0f, 424.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kLine, {614.0f, 507.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kCubic, {614.0f, 592.6f, 544.6f, 662.0f, 459.0f, 662.0f}},
    {kLine, {155.0f, 662.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kCubic, {69.4f, 662.0f, 0.0f, 592.6f, 0.0f, 507.0f}},
    {kClose, {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kMove, {167.0f, 340.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kCubic, {148.22f, 340.0f, 133.0f, 355.22f, 133.0f, 374.0f}},
    {kLine, {133.0f, 507.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kCubic, {133.0f, 525.78f, 148.22f, 541.0f, 167.0f, 541.0f}},
    {kLine, {452.0f, 541.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kCubic, {470.78f, 541.0f, 486.0f, 525.78f, 486.0f, 507.0f}},
    {kLine, {486.0f, 340.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kClose, {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
};

// Sombra: degradado vertical de tokens::kLogoFrontShadow (arriba) a tokens::kLogoFront (abajo).
constexpr Cmd kShadow[] = {
    {kMove, {486.0f, 340.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kLine, {533.0f, 340.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kLine, {614.0f, 428.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kLine, {614.0f, 514.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kLine, {486.0f, 514.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {kClose, {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
};
constexpr float kShadowTop = 340;
constexpr float kShadowBottom = 514;

}  // namespace logo
