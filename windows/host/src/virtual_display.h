#pragma once

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>

// Monitor virtual de DeskLink (driver desklink-idd). Plug() pide al driver un monitor con ese modo,
// espera a que Windows lo active como pantalla extendida y devuelve su nombre GDI («\\.\DISPLAY3»),
// que es como DXGI lo identifica. Mientras el objeto vive, un hilo avisa al driver cada segundo;
// si el host muere, el driver quita el monitor solo a los pocos segundos.
class VirtualDisplay {
 public:
  ~VirtualDisplay() { Unplug(); }

  bool Plug(UINT width, UINT height, UINT refresh_hz);
  void Unplug();
  const std::wstring& gdi_name() const { return gdi_name_; }

 private:
  bool Ioctl(DWORD code, const void* in, DWORD in_size);
  void EnsureMode(UINT width, UINT height, UINT refresh_hz);

  HANDLE device_ = INVALID_HANDLE_VALUE;
  std::wstring gdi_name_;
  std::atomic<bool> pinging_{false};
  std::thread ping_thread_;
};
