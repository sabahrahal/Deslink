#pragma once

#include <windows.h>

#include <cstdint>
#include <mutex>
#include <vector>

// Convierte el estado táctil de la tablet en toques de Windows (InjectTouchInput).
// La tablet manda siempre el estado completo (todos los dedos apoyados), así que un paquete perdido
// no deja un dedo "pegado": la diferencia con el estado anterior decide qué dedos bajan, se mueven o suben.
class TouchInjector {
 public:
  struct Contact {
    uint8_t id = 0;
    float x = 0, y = 0;  // 0..1 dentro de la pantalla capturada
    uint16_t pressure = 0;
  };

  // `rect`: zona de pantalla (en píxeles del escritorio virtual) a la que se asignan los toques.
  // `mouse`: en vez de toques, mueve el ratón con el primer dedo (más compatible, sin multitáctil).
  bool Init(const RECT& rect, bool mouse = false);
  void Update(const std::vector<Contact>& contacts);
  // Suelta los dedos si la tablet lleva más de timeout_ms sin decir nada (cable desconectado, etc.).
  void ExpireIfSilent(uint64_t now_ms, uint64_t timeout_ms);
  void ReleaseAll();

 private:
  void InjectMouse(const std::vector<Contact>& contacts);
  void Inject(const std::vector<Contact>& contacts, const std::vector<uint8_t>& released);

  std::mutex mutex_;
  bool ready_ = false;
  bool mouse_ = false;
  bool mouse_down_ = false;
  RECT rect_{};
  std::vector<Contact> active_;
  uint64_t last_update_ms_ = 0;
};
