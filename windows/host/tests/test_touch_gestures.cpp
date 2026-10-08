// Pruebas de los gestos del modo ratón (touch_gestures.h). Sin dependencias: se compila en cualquier sistema.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../src/touch_gestures.h"

using touch::MouseAction;
using touch::MouseGestures;
using touch::Point;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "%s:%d: falla %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

std::vector<Point> One(int x, int y, uint8_t id = 0) { return {Point{id, x, y}}; }
std::vector<Point> Two(int x1, int y1, int x2, int y2) { return {Point{0, x1, y1}, Point{1, x2, y2}}; }

int Count(const std::vector<MouseAction>& a, MouseAction::Kind k) {
  int n = 0;
  for (const auto& x : a) n += x.kind == k;
  return n;
}

int Sum(const std::vector<MouseAction>& a, MouseAction::Kind k) {
  int n = 0;
  for (const auto& x : a) n += x.kind == k ? x.delta : 0;
  return n;
}

void TapClicks() {
  MouseGestures g;
  std::vector<MouseAction> a;
  g.Update(One(100, 100), 0, &a);
  CHECK(a.size() == 1 && a[0].kind == MouseAction::kMove && a[0].x == 100 && a[0].y == 100);
  g.Update(One(103, 101), 40, &a);  // temblor por debajo del umbral
  g.Update({}, 80, &a);
  CHECK(Count(a, MouseAction::kLeftDown) == 1 && Count(a, MouseAction::kLeftUp) == 1);
  CHECK(Count(a, MouseAction::kRightDown) == 0);
}

void DoubleTapSnapsToFirstPoint() {
  MouseGestures g;
  std::vector<MouseAction> a;
  g.Update(One(100, 100), 0, &a);
  g.Update({}, 60, &a);
  a.clear();
  g.Update(One(110, 95), 200, &a);
  CHECK(!a.empty() && a[0].kind == MouseAction::kMove && a[0].x == 100 && a[0].y == 100);
  g.Update({}, 260, &a);
  CHECK(Count(a, MouseAction::kLeftDown) == 1);
  // Mucho después ya no se ajusta.
  a.clear();
  g.Update(One(110, 95), 2000, &a);
  CHECK(a[0].x == 110 && a[0].y == 95);
}

void DragHoldsLeftButton() {
  MouseGestures g;
  std::vector<MouseAction> a;
  g.Update(One(100, 100), 0, &a);
  g.Update(One(140, 100), 30, &a);
  CHECK(Count(a, MouseAction::kLeftDown) == 1);
  g.Update(One(200, 120), 60, &a);
  g.Tick(2000, &a);  // arrastrando no hay clic derecho
  CHECK(Count(a, MouseAction::kRightDown) == 0);
  g.Update({}, 90, &a);
  CHECK(Count(a, MouseAction::kLeftUp) == 1);
  CHECK(a[a.size() - 2].kind == MouseAction::kMove && a[a.size() - 2].x == 200);
}

void LongPressRightClicks() {
  MouseGestures g;
  std::vector<MouseAction> a;
  g.Update(One(300, 300), 0, &a);
  CHECK(g.needs_tick());
  g.Tick(300, &a);
  CHECK(Count(a, MouseAction::kRightDown) == 0);
  g.Tick(600, &a);
  CHECK(Count(a, MouseAction::kRightDown) == 1 && Count(a, MouseAction::kRightUp) == 1);
  CHECK(!g.needs_tick());
  g.Update(One(400, 300), 700, &a);  // moverse después no arrastra
  g.Update({}, 800, &a);
  CHECK(Count(a, MouseAction::kLeftDown) == 0);
}

void TwoFingerScroll() {
  MouseGestures g;
  std::vector<MouseAction> a;
  g.Update(Two(100, 100, 200, 100), 0, &a);
  g.Update(Two(100, 140, 200, 140), 30, &a);  // los dos dedos bajan 40 px
  g.Update(Two(100, 180, 200, 180), 60, &a);
  CHECK(Sum(a, MouseAction::kWheel) > 0);  // contenido baja: rueda hacia arriba
  CHECK(Count(a, MouseAction::kZoom) == 0);
  CHECK(Count(a, MouseAction::kLeftDown) == 0);
  g.Update({}, 90, &a);
  CHECK(Count(a, MouseAction::kRightDown) == 0);
  // Hacia la izquierda: rueda horizontal positiva (contenido a la derecha).
  a.clear();
  g.Update(Two(300, 100, 400, 100), 1000, &a);
  g.Update(Two(250, 100, 350, 100), 1030, &a);
  CHECK(Sum(a, MouseAction::kHWheel) > 0);
}

void PinchZooms() {
  MouseGestures g;
  std::vector<MouseAction> a;
  g.Update(Two(400, 300, 500, 300), 0, &a);
  g.Update(Two(350, 300, 550, 300), 30, &a);  // de 100 a 200 px: acercar
  CHECK(Sum(a, MouseAction::kZoom) >= 3);
  CHECK(Count(a, MouseAction::kWheel) == 0);
  a.clear();
  g.Update(Two(440, 300, 460, 300), 60, &a);  // juntar: alejar
  CHECK(Sum(a, MouseAction::kZoom) < 0);
  g.Update({}, 90, &a);
}

void TwoFingerTapRightClicks() {
  MouseGestures g;
  std::vector<MouseAction> a;
  g.Update(One(100, 100), 0, &a);
  g.Update(Two(100, 100, 160, 100), 20, &a);
  g.Update(One(160, 100, 1), 120, &a);  // los dedos se levantan de uno en uno
  g.Update({}, 150, &a);
  CHECK(Count(a, MouseAction::kRightDown) == 1);
  CHECK(Count(a, MouseAction::kLeftDown) == 0);
}

void ResetReleasesWithoutClick() {
  MouseGestures g;
  std::vector<MouseAction> a;
  g.Update(One(100, 100), 0, &a);
  g.Reset(&a);
  CHECK(Count(a, MouseAction::kLeftDown) == 0);
  a.clear();
  g.Update(One(100, 100), 0, &a);
  g.Update(One(200, 100), 10, &a);
  g.Reset(&a);
  CHECK(Count(a, MouseAction::kLeftUp) == 1);
}

}  // namespace

int main() {
  TapClicks();
  DoubleTapSnapsToFirstPoint();
  DragHoldsLeftButton();
  LongPressRightClicks();
  TwoFingerScroll();
  PinchZooms();
  TwoFingerTapRightClicks();
  ResetReleasesWithoutClick();
  if (failures) return 1;
  std::printf("touch_gestures: todo bien\n");
  return 0;
}
