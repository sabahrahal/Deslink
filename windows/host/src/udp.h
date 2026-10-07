#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// Socket UDP del host. El destino de vídeo es el último cliente que envió HELLO.
class UdpEndpoint {
 public:
  using PacketHandler = std::function<void(const uint8_t* data, size_t size, const sockaddr_in& from)>;

  ~UdpEndpoint();

  bool Bind(uint16_t port, PacketHandler handler);
  void SetPeer(const sockaddr_in& peer);
  bool has_peer() const;
  void SendToPeer(const std::vector<uint8_t>& packet);
  void SendTo(const sockaddr_in& to, const std::vector<uint8_t>& packet);
  void Stop();

 private:
  void ReceiveLoop();

  SOCKET socket_ = INVALID_SOCKET;
  PacketHandler handler_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  mutable std::mutex mutex_;
  sockaddr_in peer_{};
  bool has_peer_ = false;
};
