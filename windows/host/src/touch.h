#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "touch_gestures.h"

// Convierte el estado táctil de la tablet en entrada de Windows.
// La tablet manda siempre el estado completo (todos los dedos apoyados), así que un paquete perdido
// no deja un dedo "pegado": la diferencia con el estado anterior decide qué dedos bajan, se mueven o suben.
//
// Dos modos:
//  - Táctil (por defecto): toques reales de Windows, multitáctil y con presión. El cursor no se mueve y los
//    gestos (tocar, arrastrar, pellizcar, desplazar con dos dedos, mantener pulsado para el clic derecho)
//    los interpreta Windows como en una pantalla táctil. Se usa la API de punteros sintéticos (Windows 10
//    1809 o posterior) y, si no está, InjectTouchInput. Si Windows rechaza los toques se pasa solo a ratón.
//  - Ratón: los dedos mueven el cursor y los gestos se traducen a clics y rueda (ver touch_gestures.h).
class TouchInjector {
 public:
  enum class Mode : uint8_t { kTouch = 0, kMouse = 1 };

  struct Contact {
    uint8_t id = 0;
    float x = 0, y = 0;  // 0..1 dentro de la pantalla capturada
    uint16_t pressure = 0;
  };

  ~TouchInjector();

  // `rect`: zona de pantalla (en píxeles del escritorio virtual) a la que se asignan los toques.
  bool Init(const RECT& rect, Mode mode);
  // Cambia la zona de pantalla (resolución o posición del monitor capturado distintas).
  void SetRect(const RECT& rect);
  // Cambia de modo en marcha. Suelta antes lo que hubiera apoyado.
  void SetMode(Mode mode);
  // Modo pedido y modo en uso (distintos si Windows no acepta toques y se pasó a ratón).
  Mode requested() const { return requested_.load(); }
  Mode active() const { return active_mode_.load(); }
  // Nombre corto de lo que se está usando, para la consola.
  const char* active_name() const;

  void Update(const std::vector<Contact>& contacts);
  // Suelta los dedos si la tablet lleva más de timeout_ms sin decir nada (cable desconectado, etc.).
  void ExpireIfSilent(uint64_t now_ms, uint64_t timeout_ms);
  void ReleaseAll();

 private:
  enum class Backend { kNone, kSynthetic, kLegacy };
  struct Slot {
    bool used = false;
    uint8_t id = 0;   // id del dedo en la tablet
    int x = 0, y = 0;
    uint32_t pressure = 0;
  };

  bool StartBackend(Backend backend);
  void StopBackend();
  // Inyecta el estado de los dedos como toques. false si Windows lo rechaza.
  bool InjectTouch(const std::vector<Contact>& contacts);
  bool SendFrame(const std::vector<POINTER_TOUCH_INFO>& frame);
  void FallBack(DWORD error);
  void ReleaseLocked();
  void UpdateLocked(const std::vector<Contact>& contacts);
  void SendMouse(const std::vector<touch::MouseAction>& actions);
  touch::Point ToPixels(const Contact& c) const;
  void Worker();

  std::mutex mutex_;
  std::condition_variable wake_;
  std::thread worker_;
  bool stop_ = false;
  bool ready_ = false;
  std::atomic<Mode> requested_{Mode::kTouch};
  std::atomic<Mode> active_mode_{Mode::kTouch};
  std::atomic<Backend> backend_{Backend::kNone};
  HANDLE synthetic_ = nullptr;
  RECT rect_{};
  Slot slots_[10];
  std::vector<Contact> last_contacts_;
  touch::MouseGestures gestures_;
  uint64_t last_update_ms_ = 0;
  uint64_t last_inject_ms_ = 0;
};
