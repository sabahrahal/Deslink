#include "touch.h"

#include <algorithm>
#include <cstdio>

namespace {
uint64_t NowMs() { return GetTickCount64(); }
}  // namespace

bool TouchInjector::Init(const RECT& rect, bool mouse) {
  rect_ = rect;
  mouse_ = mouse;
  if (mouse_) {
    ready_ = true;
    return true;
  }
  if (!InitializeTouchInjection(10, TOUCH_FEEDBACK_NONE)) {
    std::fprintf(stderr, "aviso: no se pudo activar el táctil (error %lu); se usará el ratón\n", GetLastError());
    mouse_ = true;
  }
  ready_ = true;
  return true;
}

void TouchInjector::InjectMouse(const std::vector<Contact>& contacts) {
  // MOUSEEVENTF_ABSOLUTE usa 0..65535 sobre el monitor principal: justo lo que manda la tablet.
  INPUT in{};
  in.type = INPUT_MOUSE;
  if (!contacts.empty()) {
    in.mi.dx = static_cast<LONG>(std::clamp(contacts[0].x, 0.0f, 1.0f) * 65535.0f);
    in.mi.dy = static_cast<LONG>(std::clamp(contacts[0].y, 0.0f, 1.0f) * 65535.0f);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    if (!mouse_down_) in.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
    mouse_down_ = true;
  } else if (mouse_down_) {
    in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    mouse_down_ = false;
  } else {
    return;
  }
  SendInput(1, &in, sizeof(in));
}

void TouchInjector::Update(const std::vector<Contact>& contacts) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ready_) return;
  last_update_ms_ = NowMs();
  if (mouse_) {
    InjectMouse(contacts);
    active_ = contacts;
    return;
  }
  // Dedos que estaban apoyados y ya no aparecen: suben.
  std::vector<uint8_t> released;
  for (const auto& a : active_) {
    const bool still = std::any_of(contacts.begin(), contacts.end(), [&](const Contact& c) { return c.id == a.id; });
    if (!still) released.push_back(a.id);
  }
  Inject(contacts, released);
  // Dedos nuevos: el siguiente Inject los tratará como "UPDATE", no como "DOWN".
  std::vector<Contact> next = contacts;
  active_ = std::move(next);
}

void TouchInjector::Inject(const std::vector<Contact>& contacts, const std::vector<uint8_t>& released) {
  std::vector<POINTER_TOUCH_INFO> infos;
  const int w = rect_.right - rect_.left;
  const int h = rect_.bottom - rect_.top;
  auto fill = [&](POINTER_TOUCH_INFO& t, uint8_t id, int x, int y, uint32_t flags, uint32_t pressure) {
    t = {};
    t.pointerInfo.pointerType = PT_TOUCH;
    t.pointerInfo.pointerId = id;
    t.pointerInfo.pointerFlags = flags;
    t.pointerInfo.ptPixelLocation.x = x;
    t.pointerInfo.ptPixelLocation.y = y;
    t.touchFlags = TOUCH_FLAG_NONE;
    t.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
    t.orientation = 90;
    t.pressure = pressure;
    t.rcContact = {x - 2, y - 2, x + 2, y + 2};
  };
  for (const auto& c : contacts) {
    const bool known = std::any_of(active_.begin(), active_.end(), [&](const Contact& a) { return a.id == c.id; });
    const int x = rect_.left + static_cast<int>(std::clamp(c.x, 0.0f, 1.0f) * (w - 1));
    const int y = rect_.top + static_cast<int>(std::clamp(c.y, 0.0f, 1.0f) * (h - 1));
    const uint32_t flags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT |
                           (known ? POINTER_FLAG_UPDATE : (POINTER_FLAG_DOWN | POINTER_FLAG_NEW));
    POINTER_TOUCH_INFO t;
    fill(t, c.id, x, y, flags, std::clamp<uint32_t>(c.pressure ? c.pressure : 512, 1, 1024));
    infos.push_back(t);
  }
  for (uint8_t id : released) {
    const auto it = std::find_if(active_.begin(), active_.end(), [&](const Contact& a) { return a.id == id; });
    if (it == active_.end()) continue;
    const int x = rect_.left + static_cast<int>(std::clamp(it->x, 0.0f, 1.0f) * (w - 1));
    const int y = rect_.top + static_cast<int>(std::clamp(it->y, 0.0f, 1.0f) * (h - 1));
    POINTER_TOUCH_INFO t;
    fill(t, id, x, y, POINTER_FLAG_UP, 0);
    infos.push_back(t);
  }
  if (infos.empty()) return;
  if (!InjectTouchInput(static_cast<UINT32>(infos.size()), infos.data())) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      std::fprintf(stderr, "aviso: Windows rechazó un toque (error %lu); se pasa al modo ratón\n", GetLastError());
    }
    mouse_ = true;
  }
}

void TouchInjector::ExpireIfSilent(uint64_t now_ms, uint64_t timeout_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ready_ || active_.empty() || now_ms - last_update_ms_ < timeout_ms) return;
  if (mouse_) {
    InjectMouse({});
    active_.clear();
    return;
  }
  std::vector<uint8_t> released;
  for (const auto& a : active_) released.push_back(a.id);
  Inject({}, released);
  active_.clear();
}

void TouchInjector::ReleaseAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ready_ || active_.empty()) return;
  if (mouse_) {
    InjectMouse({});
    active_.clear();
    return;
  }
  std::vector<uint8_t> released;
  for (const auto& a : active_) released.push_back(a.id);
  Inject({}, released);
  active_.clear();
}
