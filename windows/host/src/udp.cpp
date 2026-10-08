#include "udp.h"

#include <cstdio>

UdpEndpoint::~UdpEndpoint() { Stop(); }

sockaddr_in UdpEndpoint::UsbPeerAddress() {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  return a;
}

bool UdpEndpoint::IsUsb(const sockaddr_in& addr) {
  return addr.sin_port == 0 && addr.sin_addr.s_addr == htonl(INADDR_LOOPBACK);
}

bool UdpEndpoint::Bind(uint16_t port, PacketHandler handler, bool usb) {
  handler_ = std::move(handler);
  socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_ == INVALID_SOCKET) return false;

  int send_buf = 4 * 1024 * 1024;
  setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&send_buf), sizeof(send_buf));
  DWORD timeout_ms = 200;  // para poder salir del bucle al parar
  setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms),
             sizeof(timeout_ms));

  // Windows devuelve WSAECONNRESET en recvfrom si un envío anterior rebotó (ICMP): se desactiva.
  BOOL no_reset = FALSE;
  DWORD bytes = 0;
  WSAIoctl(socket_, _WSAIOW(IOC_VENDOR, 12), &no_reset, sizeof(no_reset), nullptr, 0, &bytes, nullptr,
           nullptr);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (::bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    std::fprintf(stderr, "bind al puerto %u falló: %d\n", port, WSAGetLastError());
    return false;
  }
  thread_ = std::thread([this] { ReceiveLoop(); });
  if (usb) {
    const sockaddr_in usb_addr = UsbPeerAddress();
    usb_.Start(
        port,
        [this, usb_addr](const uint8_t* data, size_t size) {
          {
            std::lock_guard<std::mutex> lock(mutex_);
            if (has_peer_ && IsUsb(peer_)) last_rx_ = std::chrono::steady_clock::now();
          }
          std::lock_guard<std::mutex> lock(handler_mutex_);
          handler_(data, size, usb_addr);
        },
        [this] {
          std::lock_guard<std::mutex> lock(mutex_);
          if (has_peer_ && IsUsb(peer_)) peer_lost_ = true;
        });
  }
  return true;
}

void UdpEndpoint::SetPeer(const sockaddr_in& peer) {
  std::lock_guard<std::mutex> lock(mutex_);
  peer_ = peer;
  has_peer_ = true;
  peer_lost_ = false;
  last_rx_ = std::chrono::steady_clock::now();
}

bool UdpEndpoint::ExpirePeer(std::chrono::milliseconds timeout) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!has_peer_ || (!peer_lost_ && std::chrono::steady_clock::now() - last_rx_ < timeout)) return false;
  has_peer_ = false;
  peer_lost_ = false;
  return true;
}

bool UdpEndpoint::has_peer() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return has_peer_;
}

bool UdpEndpoint::peer_is_usb() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return has_peer_ && IsUsb(peer_);
}

void UdpEndpoint::SendBatchToPeer(const std::vector<std::vector<uint8_t>>& packets) {
  sockaddr_in peer;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_peer_) return;
    peer = peer_;
  }
  if (IsUsb(peer)) {
    usb_.Send(packets);
    return;
  }
  for (const auto& p : packets) SendTo(peer, p);
}

void UdpEndpoint::SendToPeer(const std::vector<uint8_t>& packet) {
  sockaddr_in peer;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_peer_) return;
    peer = peer_;
  }
  SendTo(peer, packet);
}

void UdpEndpoint::SendTo(const sockaddr_in& to, const std::vector<uint8_t>& packet) {
  if (IsUsb(to)) {
    usb_.Send({packet});
    return;
  }
  if (::sendto(socket_, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
               reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == SOCKET_ERROR) {
    ++send_errors_;
  }
}

void UdpEndpoint::ReceiveLoop() {
  std::vector<uint8_t> buf(2048);
  while (!stop_) {
    sockaddr_in from{};
    int from_len = sizeof(from);
    int n = ::recvfrom(socket_, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0,
                       reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n > 0) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (has_peer_ && !IsUsb(peer_) && from.sin_addr.s_addr == peer_.sin_addr.s_addr) {
          last_rx_ = std::chrono::steady_clock::now();
        }
      }
      std::lock_guard<std::mutex> lock(handler_mutex_);
      handler_(buf.data(), static_cast<size_t>(n), from);
    }
  }
}

void UdpEndpoint::Stop() {
  stop_ = true;
  usb_.Stop();
  if (thread_.joinable()) thread_.join();
  if (socket_ != INVALID_SOCKET) {
    closesocket(socket_);
    socket_ = INVALID_SOCKET;
  }
}
