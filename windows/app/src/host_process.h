#pragma once

#include <windows.h>

#include <string>

// Mensajes que HostProcess envía a la ventana. En los dos, wParam lleva el número de arranque (generation) para
// descartar los avisos de un host anterior.
// WM_HOST_LINE: una línea de la salida del host. wParam = generation * 2 + (1 si viene de stderr);
//               lParam = std::wstring* que el receptor debe borrar.
// WM_HOST_EXIT: el host terminó. wParam = generation; lParam = código de salida. Llega después de sus líneas.
constexpr UINT WM_HOST_LINE = WM_APP + 2;
constexpr UINT WM_HOST_EXIT = WM_APP + 3;

// desklink-host.exe como proceso hijo, sin consola. Va dentro de un «job» que lo cierra si la app muere de golpe,
// y se le pide que pare con un evento con nombre (--stop-event), para que quite el monitor virtual y el túnel de
// adb antes de salir.
class HostProcess {
 public:
  HostProcess();
  ~HostProcess();

  // args: opciones del host; se añaden --status y --stop-event. Devuelve false con el motivo en *error.
  bool Start(HWND hwnd, const std::wstring& exe, const std::wstring& args, std::wstring* error);
  // Pide al host que pare; WM_HOST_EXIT llega cuando termine.
  void RequestStop();
  // Para cerrar la app: pide parar, espera hasta ms y si no ha terminado lo cierra a la fuerza.
  void StopAndWait(DWORD ms);
  bool running() const;
  unsigned generation() const { return generation_; }
  DWORD pid() const { return pid_; }

 private:
  void CloseHandles();

  HANDLE job_ = nullptr;
  HANDLE process_ = nullptr;
  HANDLE stop_event_ = nullptr;
  DWORD pid_ = 0;
  unsigned generation_ = 0;
};
