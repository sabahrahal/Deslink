#include "udp.h"

#include <cstdio>

UdpEndpoint::~UdpEndpoint() { Stop(); }

bool UdpEndpoint::Bind(uint16_t port, PacketHandler handler) {
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
  return true;
}

void UdpEndpoint::SetPeer(const sockaddr_in& peer) {
  std::lock_guard<std::mutex> lock(mutex_);
  peer_ = peer;
  has_peer_ = true;
}

bool UdpEndpoint::has_peer() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return has_peer_;
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
  ::sendto(socket_, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

void UdpEndpoint::ReceiveLoop() {
  std::vector<uint8_t> buf(2048);
  while (!stop_) {
    sockaddr_in from{};
    int from_len = sizeof(from);
    int n = ::recvfrom(socket_, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0,
                       reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n > 0) handler_(buf.data(), static_cast<size_t>(n), from);
  }
}

void UdpEndpoint::Stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  if (socket_ != INVALID_SOCKET) {
    closesocket(socket_);
    socket_ = INVALID_SOCKET;
  }
}
