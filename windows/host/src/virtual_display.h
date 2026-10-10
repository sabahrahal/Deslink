#pragma once

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>

// Monitor virtual de DeskLink (driver desklink-idd). Plug() pide al driver un monitor de width x height (el tamaño
// nativo de la tablet: Windows ofrece de ahí hacia abajo y lo reconoce siempre como el mismo monitor) y le pone
// mode_w x mode_h (la resolución elegida en Windows),
// espera a que Windows lo active como pantalla extendida y devuelve su nombre GDI («\\.\DISPLAY3»),
// que es como DXGI lo identifica. Mientras el objeto vive, un hilo avisa al driver cada segundo;
// si el host muere, el driver quita el monitor solo a los pocos segundos.
class VirtualDisplay {
 public:
  ~VirtualDisplay() { Unplug(); }

  bool Plug(UINT width, UINT height, UINT refresh_hz, UINT mode_w, UINT mode_h);
  void Unplug();
  // Cambia la resolución del monitor ya enchufado (sin quitarlo: Windows conserva su posición y su escala).
  void SetMode(UINT width, UINT height, UINT refresh_hz) { EnsureMode(width, height, refresh_hz); }
  const std::wstring& gdi_name() const { return gdi_name_; }

 private:
  bool Ioctl(DWORD code, const void* in, DWORD in_size);
  void EnsureMode(UINT width, UINT height, UINT refresh_hz);

  HANDLE device_ = INVALID_HANDLE_VALUE;
  std::wstring gdi_name_;
  std::atomic<bool> pinging_{false};
  std::thread ping_thread_;
};
