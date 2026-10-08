#include "touch.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iterator>

namespace {

uint64_t NowMs() { return GetTickCount64(); }

// Cada cuánto se repite el estado de los dedos quietos. Windows cancela un toque inyectado que deja de
// recibir actualizaciones, y sin ellas tampoco reconoce el «mantener pulsado» (clic derecho).
constexpr uint64_t kKeepAliveMs = 33;
constexpr uint32_t kMaxContacts = 10;

// API de punteros sintéticos (Windows 10 1809). Se carga en tiempo de ejecución: con _WIN32_WINNT=0x0A00 el
// SDK no la declara, y así el host sigue arrancando en Windows anteriores (usa InjectTouchInput).
struct SyntheticPointerTypeInfo {
  POINTER_INPUT_TYPE type;
  union {
    POINTER_TOUCH_INFO touchInfo;
    POINTER_PEN_INFO penInfo;
  };
};
using CreateSyntheticFn = HANDLE(WINAPI*)(POINTER_INPUT_TYPE, ULONG, int);
using InjectSyntheticFn = BOOL(WINAPI*)(HANDLE, const SyntheticPointerTypeInfo*, UINT32);
using DestroySyntheticFn = void(WINAPI*)(HANDLE);
constexpr int kPointerFeedbackNone = 3;  // POINTER_FEEDBACK_NONE

struct SyntheticApi {
  CreateSyntheticFn create = nullptr;
  InjectSyntheticFn inject = nullptr;
  DestroySyntheticFn destroy = nullptr;
  SyntheticApi() {
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
      create = reinterpret_cast<CreateSyntheticFn>(GetProcAddress(user32, "CreateSyntheticPointerDevice"));
      inject = reinterpret_cast<InjectSyntheticFn>(GetProcAddress(user32, "InjectSyntheticPointerInput"));
      destroy = reinterpret_cast<DestroySyntheticFn>(GetProcAddress(user32, "DestroySyntheticPointerDevice"));
    }
  }
  bool available() const { return create && inject && destroy; }
};

const SyntheticApi& Synthetic() {
  static const SyntheticApi api;
  return api;
}

const char* ErrorHint(DWORD error) {
  switch (error) {
    case ERROR_ACCESS_DENIED:
      return " (la ventana de delante es de administrador: abre el host como administrador para poder tocarla)";
    case ERROR_INVALID_PARAMETER:
      return " (Windows no acepta el toque en esa posición)";
    case ERROR_NOT_READY:
      return " (el táctil de Windows no está listo)";
    default:
      return "";
  }
}

}  // namespace

TouchInjector::~TouchInjector() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
    ReleaseLocked();
    StopBackend();
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

bool TouchInjector::Init(const RECT& rect, Mode mode) {
  SetRect(rect);
  SetMode(mode);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ready_ = true;
  }
  if (!worker_.joinable()) worker_ = std::thread([this] { Worker(); });
  return true;
}

void TouchInjector::SetRect(const RECT& rect) {
  std::lock_guard<std::mutex> lock(mutex_);
  rect_ = rect;
  // Umbrales en píxeles del monitor: proporcionales a su tamaño, así valen igual a 1080p que a 3K.
  const int w = std::max<int>(rect.right - rect.left, 1);
  touch::GestureConfig cfg;
  cfg.slop_px = std::max(10, w / 200);
  cfg.scroll_px_per_notch = std::max(30, w / 60);
  gestures_.set_config(cfg);
}

void TouchInjector::SetMode(Mode mode) {
  std::lock_guard<std::mutex> lock(mutex_);
  ReleaseLocked();
  StopBackend();
  requested_ = mode;
  if (mode == Mode::kTouch && (StartBackend(Backend::kSynthetic) || StartBackend(Backend::kLegacy))) {
    active_mode_ = Mode::kTouch;
  } else {
    if (mode == Mode::kTouch) std::fprintf(stderr, "aviso: este Windows no permite inyectar toques; se usará el modo ratón\n");
    active_mode_ = Mode::kMouse;
  }
}

const char* TouchInjector::active_name() const {
  if (active_mode_ == Mode::kMouse) return "ratón";
  return backend_ == Backend::kSynthetic ? "táctil directo" : "táctil directo (InjectTouchInput)";
}

bool TouchInjector::StartBackend(Backend backend) {
  if (backend == Backend::kSynthetic) {
    const auto& api = Synthetic();
    if (!api.available()) return false;
    synthetic_ = api.create(PT_TOUCH, kMaxContacts, kPointerFeedbackNone);
    if (!synthetic_) {
      std::fprintf(stderr, "aviso: no se pudo crear el táctil sintético (error %lu)\n", GetLastError());
      return false;
    }
  } else if (backend == Backend::kLegacy) {
    // InitializeTouchInjection vale para todo el proceso: basta con una vez.
    static bool initialized = false;
    if (!initialized && !InitializeTouchInjection(kMaxContacts, TOUCH_FEEDBACK_NONE)) {
      std::fprintf(stderr, "aviso: no se pudo activar InjectTouchInput (error %lu)\n", GetLastError());
      return false;
    }
    initialized = true;
  }
  backend_ = backend;
  return true;
}

void TouchInjector::StopBackend() {
  // Destruir el dispositivo sintético cancela los toques que tuviera apoyados.
  if (synthetic_) Synthetic().destroy(synthetic_);
  synthetic_ = nullptr;
  backend_ = Backend::kNone;
  for (auto& s : slots_) s = {};
}

touch::Point TouchInjector::ToPixels(const Contact& c) const {
  const int w = rect_.right - rect_.left;
  const int h = rect_.bottom - rect_.top;
  touch::Point p;
  p.id = c.id;
  p.x = rect_.left + static_cast<int>(std::clamp(c.x, 0.0f, 1.0f) * (w - 1) + 0.5f);
  p.y = rect_.top + static_cast<int>(std::clamp(c.y, 0.0f, 1.0f) * (h - 1) + 0.5f);
  return p;
}

bool TouchInjector::InjectTouch(const std::vector<Contact>& contacts) {
  // Cada dedo de la tablet ocupa una ranura fija (0..9) mientras está apoyado: Windows quiere ids de puntero
  // pequeños y estables, y los de Android pueden crecer.
  std::vector<POINTER_TOUCH_INFO> frame;
  bool freed[kMaxContacts] = {};
  auto fill = [&](uint32_t slot, uint32_t flags) {
    const Slot& s = slots_[slot];
    POINTER_TOUCH_INFO t{};
    t.pointerInfo.pointerType = PT_TOUCH;
    t.pointerInfo.pointerId = slot;
    t.pointerInfo.pointerFlags = flags;
    t.pointerInfo.ptPixelLocation.x = s.x;
    t.pointerInfo.ptPixelLocation.y = s.y;
    t.touchFlags = TOUCH_FLAG_NONE;
    t.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
    t.orientation = 90;
    t.pressure = s.pressure;
    t.rcContact = {s.x - 4, s.y - 4, s.x + 4, s.y + 4};
    frame.push_back(t);
  };
  // Dedos que estaban apoyados y ya no aparecen: suben (en su última posición).
  for (uint32_t i = 0; i < kMaxContacts; ++i) {
    if (!slots_[i].used) continue;
    const bool still = std::any_of(contacts.begin(), contacts.end(), [&](const Contact& c) { return c.id == slots_[i].id; });
    if (still) continue;
    fill(i, POINTER_FLAG_UP);
    slots_[i].used = false;
    freed[i] = true;
  }
  for (const auto& c : contacts) {
    uint32_t slot = kMaxContacts;
    bool known = false;
    for (uint32_t i = 0; i < kMaxContacts; ++i) {
      if (slots_[i].used && slots_[i].id == c.id) {
        slot = i;
        known = true;
        break;
      }
    }
    if (!known) {
      // Una ranura que sube en este mismo frame no puede volver a bajar en él.
      for (uint32_t i = 0; i < kMaxContacts && slot == kMaxContacts; ++i) {
        if (!slots_[i].used && !freed[i]) slot = i;
      }
      if (slot == kMaxContacts) continue;  // más de 10 dedos
    }
    Slot& s = slots_[slot];
    const touch::Point p = ToPixels(c);
    s.used = true;
    s.id = c.id;
    s.x = p.x;
    s.y = p.y;
    s.pressure = std::clamp<uint32_t>(c.pressure ? c.pressure : 512, 1, 1024);
    fill(slot, POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | (known ? POINTER_FLAG_UPDATE : POINTER_FLAG_DOWN));
  }
  if (frame.empty()) return true;
  last_inject_ms_ = NowMs();
  return SendFrame(frame);
}

bool TouchInjector::SendFrame(const std::vector<POINTER_TOUCH_INFO>& frame) {
  if (backend_ == Backend::kSynthetic) {
    std::vector<SyntheticPointerTypeInfo> infos(frame.size());
    for (size_t i = 0; i < frame.size(); ++i) {
      infos[i] = {};
      infos[i].type = PT_TOUCH;
      infos[i].touchInfo = frame[i];
    }
    return Synthetic().inject(synthetic_, infos.data(), static_cast<UINT32>(infos.size())) != FALSE;
  }
  if (backend_ == Backend::kLegacy) {
    return InjectTouchInput(static_cast<UINT32>(frame.size()), frame.data()) != FALSE;
  }
  return false;
}

void TouchInjector::FallBack(DWORD error) {
  const Backend failed = backend_;
  StopBackend();
  if (failed == Backend::kSynthetic) {
    std::fprintf(stderr, "aviso: Windows rechazó un toque sintético (error %lu%s); se prueba InjectTouchInput\n", error,
                 ErrorHint(error));
    if (StartBackend(Backend::kLegacy)) return;
  } else {
    std::fprintf(stderr, "aviso: Windows rechazó un toque (error %lu%s)\n", error, ErrorHint(error));
  }
  std::fprintf(stderr, "aviso: el táctil directo no funciona en este PC; se pasa al modo ratón\n");
  active_mode_ = Mode::kMouse;
}

void TouchInjector::UpdateLocked(const std::vector<Contact>& contacts) {
  last_contacts_ = contacts;
  if (active_mode_ == Mode::kTouch) {
    // Si el método actual falla se prueba el siguiente con el mismo estado (para el nuevo, los dedos bajan).
    while (backend_ != Backend::kNone) {
      if (InjectTouch(contacts)) return;
      FallBack(GetLastError());
    }
    active_mode_ = Mode::kMouse;
  }
  std::vector<touch::Point> points;
  points.reserve(contacts.size());
  for (const auto& c : contacts) points.push_back(ToPixels(c));
  std::vector<touch::MouseAction> actions;
  gestures_.Update(points, NowMs(), &actions);
  SendMouse(actions);
}

void TouchInjector::ReleaseLocked() {
  if (active_mode_ == Mode::kTouch) {
    if (std::any_of(std::begin(slots_), std::end(slots_), [](const Slot& s) { return s.used; })) UpdateLocked({});
  } else {
    // Al desconectar no se cuenta como toque: se suelta lo pulsado sin hacer clic.
    std::vector<touch::MouseAction> actions;
    gestures_.Reset(&actions);
    SendMouse(actions);
  }
  last_contacts_.clear();
}

void TouchInjector::SendMouse(const std::vector<touch::MouseAction>& actions) {
  if (actions.empty()) return;
  // Con MOUSEEVENTF_VIRTUALDESK, 0..65535 cubre el escritorio virtual entero (todos los monitores).
  const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int vw = std::max(GetSystemMetrics(SM_CXVIRTUALSCREEN), 2), vh = std::max(GetSystemMetrics(SM_CYVIRTUALSCREEN), 2);
  std::vector<INPUT> inputs;
  auto mouse = [&](DWORD flags, LONG dx = 0, LONG dy = 0, DWORD data = 0) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.mouseData = data;
    inputs.push_back(in);
  };
  auto key = [&](WORD vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    inputs.push_back(in);
  };
  for (const auto& a : actions) {
    switch (a.kind) {
      case touch::MouseAction::kMove:
        mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK,
              static_cast<LONG>((a.x - vx) * 65535.0 / (vw - 1) + 0.5), static_cast<LONG>((a.y - vy) * 65535.0 / (vh - 1) + 0.5));
        break;
      case touch::MouseAction::kLeftDown: mouse(MOUSEEVENTF_LEFTDOWN); break;
      case touch::MouseAction::kLeftUp: mouse(MOUSEEVENTF_LEFTUP); break;
      case touch::MouseAction::kRightDown: mouse(MOUSEEVENTF_RIGHTDOWN); break;
      case touch::MouseAction::kRightUp: mouse(MOUSEEVENTF_RIGHTUP); break;
      case touch::MouseAction::kWheel: mouse(MOUSEEVENTF_WHEEL, 0, 0, static_cast<DWORD>(a.delta)); break;
      case touch::MouseAction::kHWheel: mouse(MOUSEEVENTF_HWHEEL, 0, 0, static_cast<DWORD>(a.delta)); break;
      case touch::MouseAction::kZoom:
        key(VK_CONTROL, false);
        mouse(MOUSEEVENTF_WHEEL, 0, 0, static_cast<DWORD>(a.delta > 0 ? WHEEL_DELTA : -WHEEL_DELTA));
        key(VK_CONTROL, true);
        break;
    }
  }
  SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

void TouchInjector::Update(const std::vector<Contact>& contacts) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) return;
    last_update_ms_ = NowMs();
    UpdateLocked(contacts);
  }
  if (!contacts.empty()) wake_.notify_one();
}

void TouchInjector::ExpireIfSilent(uint64_t now_ms, uint64_t timeout_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ready_ || last_contacts_.empty() || now_ms - last_update_ms_ < timeout_ms) return;
  ReleaseLocked();
}

void TouchInjector::ReleaseAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (ready_) ReleaseLocked();
}

void TouchInjector::Worker() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stop_) {
    const bool touching = active_mode_ == Mode::kTouch && !last_contacts_.empty();
    const bool timing = active_mode_ == Mode::kMouse && gestures_.needs_tick();
    if (!touching && !timing) {
      // Sin dedos apoyados el hilo duerme hasta el siguiente toque: no gasta nada en reposo.
      wake_.wait(lock);
      continue;
    }
    wake_.wait_for(lock, std::chrono::milliseconds(touching ? kKeepAliveMs : 16));
    if (stop_) break;
    const uint64_t now = NowMs();
    if (active_mode_ == Mode::kTouch) {
      if (!last_contacts_.empty() && now - last_inject_ms_ >= kKeepAliveMs) {
        const std::vector<Contact> same = last_contacts_;
        UpdateLocked(same);
      }
    } else if (gestures_.needs_tick()) {
      std::vector<touch::MouseAction> actions;
      gestures_.Tick(now, &actions);
      SendMouse(actions);
    }
  }
}
