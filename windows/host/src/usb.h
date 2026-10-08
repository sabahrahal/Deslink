#pragma once

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifndef WSA_FLAG_NO_HANDLE_INHERIT
#define WSA_FLAG_NO_HANDLE_INHERIT 0x80
#endif

// Socket que los procesos hijos no heredan (adb sobre todo: ver RunAdb en usb.cpp).
SOCKET NonInheritableSocket(int type, int protocol);

// Cierra el servidor de adb (adb kill-server). adb_path vacío: lo busca como AdbReverse. Para soltar el puerto
// de DeskLink si lo retiene un servidor de adb que lo heredó de un host de una versión anterior.
bool StopAdbServer(const std::wstring& adb_path);

// Enlace USB: servidor TCP en 127.0.0.1 al que llega la tablet por `adb reverse` (ver docs/usb.md).
// Cada paquete DLP viaja precedido de su tamaño (u16, little-endian). Un solo cliente a la vez:
// una conexión nueva sustituye a la anterior.
class UsbLink {
 public:
  using PacketHandler = std::function<void(const uint8_t* data, size_t size)>;
  using CloseHandler = std::function<void()>;

  ~UsbLink();

  bool Start(uint16_t port, PacketHandler on_packet, CloseHandler on_close);
  bool connected() const { return client_ != INVALID_SOCKET; }
  // Escribe los paquetes de una vez (un frame entero en una sola llamada). Si la tablet deja de leer
  // durante más de 1 s, la conexión se corta y la tablet vuelve a conectar.
  void Send(const std::vector<std::vector<uint8_t>>& packets);
  void Stop();

 private:
  void Loop();
  void CloseClient();
  bool SendAll(const uint8_t* data, size_t size);

  SOCKET listen_ = INVALID_SOCKET;
  std::atomic<SOCKET> client_{INVALID_SOCKET};
  PacketHandler on_packet_;
  CloseHandler on_close_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::mutex send_mutex_;
  std::vector<uint8_t> send_buf_;
};

// Mantiene `adb reverse tcp:PORT tcp:PORT` en cada tablet conectada por USB con la depuración USB activada,
// para que el usuario no tenga que hacer nada al enchufarla. Sin adb.exe no hace nada (avisa una vez).
class AdbReverse {
 public:
  ~AdbReverse();

  // adb_path vacío: lo busca junto al .exe, en el PATH y en el SDK de Android.
  void Start(uint16_t port, std::wstring adb_path);
  void Stop();
  // Tablets con el túnel puesto y tablets enchufadas sin autorizar; ready() es -1 si no hay adb.exe.
  int ready() const { return missing_ ? -1 : ready_.load(); }
  int unauthorized() const { return unauthorized_; }

 private:
  void Loop();
  bool Run(const std::wstring& args, std::string* out, DWORD timeout_ms = 10000);

  uint16_t port_ = 0;
  std::wstring adb_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::set<std::string> reversed_;  // tablets con el túnel puesto
  std::set<std::string> warned_;    // tablets sin autorizar, ya avisadas
  std::atomic<int> ready_{0};
  std::atomic<int> unauthorized_{0};
  std::atomic<bool> missing_{false};
};
