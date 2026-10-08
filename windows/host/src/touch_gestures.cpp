#include "touch_gestures.h"

#include <cmath>
#include <cstdlib>

namespace touch {
namespace {

const Point* Find(const std::vector<Point>& points, uint8_t id) {
  for (const auto& p : points) {
    if (p.id == id) return &p;
  }
  return nullptr;
}

void Move(int x, int y, std::vector<MouseAction>* out) {
  MouseAction a;
  a.kind = MouseAction::kMove;
  a.x = x;
  a.y = y;
  out->push_back(a);
}

void Button(MouseAction::Kind kind, std::vector<MouseAction>* out) {
  MouseAction a;
  a.kind = kind;
  out->push_back(a);
}

void Delta(MouseAction::Kind kind, int delta, std::vector<MouseAction>* out) {
  MouseAction a;
  a.kind = kind;
  a.delta = delta;
  out->push_back(a);
}

}  // namespace

void MouseGestures::Update(const std::vector<Point>& points, uint64_t now_ms, std::vector<MouseAction>* out) {
  if (points.empty()) {
    Finish(now_ms, out);
    return;
  }
  switch (state_) {
    case State::kIdle:
      if (points.size() >= 2) {
        BeginTwo(points, now_ms);
        return;
      }
      state_ = State::kPending;
      primary_ = points[0].id;
      start_x_ = points[0].x;
      start_y_ = points[0].y;
      start_ms_ = now_ms;
      // Un segundo toque cerca del anterior cae en el mismo punto: Windows solo lo cuenta como doble clic
      // si los dos clics están a unos pocos píxeles, y un dedo nunca toca dos veces en el mismo sitio.
      if (has_last_tap_ && now_ms - last_tap_ms_ <= cfg_.double_tap_ms &&
          std::abs(start_x_ - last_tap_x_) <= 2 * cfg_.slop_px && std::abs(start_y_ - last_tap_y_) <= 2 * cfg_.slop_px) {
        start_x_ = last_tap_x_;
        start_y_ = last_tap_y_;
      }
      Move(start_x_, start_y_, out);
      return;
    case State::kPending: {
      if (points.size() >= 2) {
        BeginTwo(points, now_ms);
        return;
      }
      const Point* p = Find(points, primary_);
      if (!p) p = &points[0];
      if (std::abs(p->x - start_x_) > cfg_.slop_px || std::abs(p->y - start_y_) > cfg_.slop_px) {
        // Arrastre: el botón baja donde se apoyó el dedo, así lo que se arrastra es lo que se tocó.
        state_ = State::kDrag;
        primary_ = p->id;
        Button(MouseAction::kLeftDown, out);
        Move(p->x, p->y, out);
      }
      return;
    }
    case State::kDrag: {
      const Point* p = Find(points, primary_);
      if (p) Move(p->x, p->y, out);
      return;
    }
    case State::kHeld:
      return;
    case State::kTwo:
    case State::kScroll:
    case State::kZoom:
      TwoFingers(points, out);
      return;
  }
}

void MouseGestures::Tick(uint64_t now_ms, std::vector<MouseAction>* out) {
  if (state_ != State::kPending || now_ms - start_ms_ < cfg_.long_press_ms) return;
  // Dedo quieto: clic derecho en el sitio, y nada más hasta que se levante.
  state_ = State::kHeld;
  has_last_tap_ = false;
  Move(start_x_, start_y_, out);
  Button(MouseAction::kRightDown, out);
  Button(MouseAction::kRightUp, out);
}

void MouseGestures::Reset(std::vector<MouseAction>* out) {
  if (state_ == State::kDrag) Button(MouseAction::kLeftUp, out);
  state_ = State::kIdle;
  has_last_tap_ = false;
}

void MouseGestures::BeginTwo(const std::vector<Point>& points, uint64_t now_ms) {
  state_ = State::kTwo;
  a_ = points[0].id;
  b_ = points[1].id;
  cx0_ = last_cx_ = (points[0].x + points[1].x) / 2.0;
  cy0_ = last_cy_ = (points[0].y + points[1].y) / 2.0;
  dist0_ = zoom_dist_ = std::hypot(points[0].x - points[1].x, points[0].y - points[1].y);
  wheel_rest_ = hwheel_rest_ = 0;
  // Si el primer dedo llevaba un rato quieto, el clic de dos dedos cuenta desde que bajó el segundo.
  start_ms_ = now_ms;
  has_last_tap_ = false;
}

void MouseGestures::TwoFingers(const std::vector<Point>& points, std::vector<MouseAction>* out) {
  const Point* pa = Find(points, a_);
  const Point* pb = Find(points, b_);
  if (!pa || !pb) return;  // levantando los dedos: se decide al soltar el último
  const double cx = (pa->x + pb->x) / 2.0, cy = (pa->y + pb->y) / 2.0;
  const double dist = std::hypot(pa->x - pb->x, pa->y - pb->y);
  if (state_ == State::kTwo) {
    // Lo primero que pase el umbral decide el gesto: separar/juntar (zoom) o mover los dos (rueda).
    if (std::abs(dist - dist0_) > 3.0 * cfg_.slop_px) {
      state_ = State::kZoom;
      zoom_dist_ = dist0_;
      Move(static_cast<int>(cx0_), static_cast<int>(cy0_), out);  // el zoom se centra entre los dedos
    } else if (std::hypot(cx - cx0_, cy - cy0_) > cfg_.slop_px) {
      state_ = State::kScroll;
      last_cx_ = cx0_;
      last_cy_ = cy0_;
      Move(static_cast<int>(cx0_), static_cast<int>(cy0_), out);  // la rueda va a la ventana bajo los dedos
    } else {
      return;
    }
  }
  if (state_ == State::kScroll) {
    // Como en una pantalla táctil: los dedos bajan, el contenido baja (rueda hacia arriba).
    wheel_rest_ += (cy - last_cy_) * 120.0 / cfg_.scroll_px_per_notch;
    hwheel_rest_ += (last_cx_ - cx) * 120.0 / cfg_.scroll_px_per_notch;
    last_cx_ = cx;
    last_cy_ = cy;
    const int wheel = static_cast<int>(wheel_rest_);
    const int hwheel = static_cast<int>(hwheel_rest_);
    if (wheel != 0) {
      Delta(MouseAction::kWheel, wheel, out);
      wheel_rest_ -= wheel;
    }
    if (hwheel != 0) {
      Delta(MouseAction::kHWheel, hwheel, out);
      hwheel_rest_ -= hwheel;
    }
    return;
  }
  // Zoom: un paso cada vez que la distancia cambia zoom_step veces respecto al paso anterior.
  if (zoom_dist_ < 1) zoom_dist_ = 1;
  while (dist >= zoom_dist_ * cfg_.zoom_step) {
    zoom_dist_ *= cfg_.zoom_step;
    Delta(MouseAction::kZoom, 1, out);
  }
  while (dist <= zoom_dist_ / cfg_.zoom_step) {
    zoom_dist_ /= cfg_.zoom_step;
    Delta(MouseAction::kZoom, -1, out);
  }
}

void MouseGestures::Finish(uint64_t now_ms, std::vector<MouseAction>* out) {
  switch (state_) {
    case State::kPending:
      // Toque corto: clic izquierdo donde se apoyó.
      Button(MouseAction::kLeftDown, out);
      Button(MouseAction::kLeftUp, out);
      has_last_tap_ = true;
      last_tap_ms_ = now_ms;
      last_tap_x_ = start_x_;
      last_tap_y_ = start_y_;
      break;
    case State::kDrag:
      Button(MouseAction::kLeftUp, out);
      break;
    case State::kTwo:
      if (now_ms - start_ms_ <= cfg_.two_finger_tap_ms) {
        Move(static_cast<int>(cx0_), static_cast<int>(cy0_), out);
        Button(MouseAction::kRightDown, out);
        Button(MouseAction::kRightUp, out);
      }
      break;
    default:
      break;
  }
  state_ = State::kIdle;
}

}  // namespace touch
