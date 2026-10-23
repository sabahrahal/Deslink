#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "usb.h"

// Proceso que tiene abierto el puerto UDP (0 si no se sabe) y su nombre para mostrarlo.
DWORD UdpPortOwner(uint16_t port);
std::string DescribeProcess(DWORD pid);

// ¿Se llega a esta IP por el anclaje USB de la tablet (adaptador RNDIS/NCM del PC) y no por Wi-Fi o Ethernet?
bool IsUsbTetherAddress(const in_addr& ip);

// Socket UDP del host y enlace USB (TCP por adb reverse, ver usb.h). El destino de vídeo es el último
// cliente que envió HELLO, llegue por donde llegue: el cliente USB se identifica con UsbPeerAddress().
class UdpEndpoint {
 public:
  using PacketHandler = std::function<void(const uint8_t* data, size_t size, const sockaddr_in& from)>;

  ~UdpEndpoint();

  // Dirección con la que se presenta al manejador el cliente que llega por USB (puerto 0: ningún
  // cliente UDP lo usa).
  static sockaddr_in UsbPeerAddress();
  static bool IsUsb(const sockaddr_in& addr);

  // usb: escucha también en TCP 127.0.0.1:port para el cable USB. free_orphaned_port: se llama una vez si el
  // puerto lo retiene un proceso que heredó el socket de un host ya cerrado; true si lo ha soltado.
  bool Bind(uint16_t port, PacketHandler handler, bool usb = true, std::function<bool()> free_orphaned_port = {});
  void SetPeer(const sockaddr_in& peer);
  // Olvida al cliente si lleva más de timeout sin enviar nada (la tablet manda PING cada segundo).
  // Devuelve true si lo olvidó.
  bool ExpirePeer(std::chrono::milliseconds timeout);
  // El cliente dijo adiós (BYE): el próximo ExpirePeer lo olvida sin esperar.
  void ForgetPeer();
  bool has_peer() const;
  bool peer_is_usb() const;
  void SendToPeer(const std::vector<uint8_t>& packet);
  // Un frame entero. Por USB va de una sola escritura (TCP ya regula el ritmo); por UDP, paquete a paquete.
  void SendBatchToPeer(const std::vector<std::vector<uint8_t>>& packets);
  void SendTo(const sockaddr_in& to, const std::vector<uint8_t>& packet);
  void Stop();
  uint64_t send_errors() const { return send_errors_; }

 private:
  void ReceiveLoop();
  bool BindWithRetry(const sockaddr_in& addr, uint16_t port, const std::function<bool()>& free_orphaned_port);

  SOCKET socket_ = INVALID_SOCKET;
  PacketHandler handler_;
  std::mutex handler_mutex_;  // el manejador se llama desde el hilo UDP y desde el USB, nunca a la vez
  std::thread thread_;
  std::atomic<bool> stop_{false};
  mutable std::mutex mutex_;
  sockaddr_in peer_{};
  bool has_peer_ = false;
  std::atomic<uint64_t> send_errors_{0};
  std::chrono::steady_clock::time_point last_rx_{};
  bool peer_lost_ = false;  // el cable USB se cerró o el cliente dijo adiós: se olvida sin esperar
  UsbLink usb_;

  // Prioridad de vídeo en el Wi-Fi (qWAVE): los paquetes al cliente salen marcados como audio/vídeo (DSCP 40), y el
  // router y la tarjeta Wi-Fi los mandan por la cola WMM de vídeo, por delante de descargas y actualizaciones.
  void PrioritizeFlowTo(const sockaddr_in& peer);
  HANDLE qos_ = nullptr;
  unsigned long qos_flow_ = 0;  // QOS_FLOWID; 0 = ninguno
  sockaddr_in qos_peer_{};
};
