#pragma once

// Gestos del modo ratón: convierte los dedos de la tablet en acciones de ratón para las apps que no
// entienden el táctil (o cuando Windows no acepta toques inyectados). Sin dependencias de Windows, para
// poder probarlo en cualquier sistema (ver tests/test_touch_gestures.cpp).
//
//  - Un toque corto: clic izquierdo (dos seguidos en el mismo sitio: doble clic).
//  - Un dedo que se mueve: arrastrar con el botón izquierdo.
//  - Un dedo quieto un rato: clic derecho.
//  - Dos dedos que se mueven juntos: rueda (vertical y horizontal).
//  - Dos dedos que se separan o se juntan: zoom (Ctrl + rueda).
//  - Dos dedos que tocan y se levantan sin moverse: clic derecho.

#include <cstdint>
#include <vector>

namespace touch {

struct Point {
  uint8_t id = 0;
  int x = 0, y = 0;  // píxeles del escritorio
};

struct MouseAction {
  enum Kind : uint8_t { kMove, kLeftDown, kLeftUp, kRightDown, kRightUp, kWheel, kHWheel, kZoom };
  Kind kind = kMove;
  int x = 0, y = 0;  // kMove
  int delta = 0;     // kWheel/kHWheel: unidades de rueda (120 = una muesca); kZoom: +1 acercar, -1 alejar
};

struct GestureConfig {
  int slop_px = 12;                  // movimiento por debajo del cual un dedo se considera quieto
  uint64_t long_press_ms = 550;      // dedo quieto este tiempo: clic derecho
  uint64_t two_finger_tap_ms = 350;  // dos dedos que se levantan antes de esto sin moverse: clic derecho
  uint64_t double_tap_ms = 450;      // segundo toque dentro de este tiempo: cae en el mismo punto (doble clic)
  int scroll_px_per_notch = 40;      // píxeles de desplazamiento de los dedos por muesca de rueda
  double zoom_step = 1.15;           // cuánto tienen que separarse o juntarse los dedos por paso de zoom
};

class MouseGestures {
 public:
  explicit MouseGestures(const GestureConfig& config = {}) : cfg_(config) {}

  void set_config(const GestureConfig& config) { cfg_ = config; }

  // Estado completo de los dedos apoyados (vacío: ninguno). Añade a `out` lo que haya que inyectar.
  void Update(const std::vector<Point>& points, uint64_t now_ms, std::vector<MouseAction>* out);
  // Para lo que depende del tiempo (pulsación larga). Llamar a menudo mientras needs_tick().
  void Tick(uint64_t now_ms, std::vector<MouseAction>* out);
  // Suelta lo que esté pulsado y vuelve al reposo.
  void Reset(std::vector<MouseAction>* out);
  bool needs_tick() const { return state_ == State::kPending; }

 private:
  enum class State { kIdle, kPending, kDrag, kHeld, kTwo, kScroll, kZoom };

  void BeginTwo(const std::vector<Point>& points, uint64_t now_ms);
  void TwoFingers(const std::vector<Point>& points, std::vector<MouseAction>* out);
  void Finish(uint64_t now_ms, std::vector<MouseAction>* out);

  GestureConfig cfg_;
  State state_ = State::kIdle;
  uint8_t primary_ = 0;
  int start_x_ = 0, start_y_ = 0;
  uint64_t start_ms_ = 0;
  // Dos dedos: los dos primeros que se apoyaron.
  uint8_t a_ = 0, b_ = 0;
  double cx0_ = 0, cy0_ = 0, dist0_ = 0;     // al empezar
  double last_cx_ = 0, last_cy_ = 0;         // último centro usado para la rueda
  double zoom_dist_ = 0;                     // distancia del último paso de zoom
  double wheel_rest_ = 0, hwheel_rest_ = 0;  // fracciones de muesca pendientes
  // Último toque, para que el doble clic caiga en el mismo punto.
  uint64_t last_tap_ms_ = 0;
  int last_tap_x_ = 0, last_tap_y_ = 0;
  bool has_last_tap_ = false;
};

}  // namespace touch
