#include "udp.h"

#include <iphlpapi.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string ProcessName(DWORD pid) {
  std::string name = "PID " + std::to_string(pid);
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return name;
  wchar_t path[MAX_PATH];
  DWORD len = MAX_PATH;
  if (QueryFullProcessImageNameW(h, 0, path, &len)) {
    const std::wstring w(path, len);
    const std::wstring base = w.substr(w.find_last_of(L'\\') + 1);
    const int n = WideCharToMultiByte(CP_UTF8, 0, base.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, base.c_str(), -1, s.data(), n, nullptr, nullptr);
    name = s + " (PID " + std::to_string(pid) + ")";
  }
  CloseHandle(h);
  return name;
}

}  // namespace

DWORD UdpPortOwner(uint16_t port) {
  ULONG size = 0;
  GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
  std::vector<uint8_t> buf(size + 1024);
  size = static_cast<ULONG>(buf.size());
  if (GetExtendedUdpTable(buf.data(), &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) != NO_ERROR) return 0;
  const auto* table = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(buf.data());
  for (DWORD i = 0; i < table->dwNumEntries; ++i) {
    if (ntohs(static_cast<u_short>(table->table[i].dwLocalPort)) == port) return table->table[i].dwOwningPid;
  }
  return 0;
}

std::string DescribeProcess(DWORD pid) { return pid ? ProcessName(pid) : std::string("otro programa"); }

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
  if (!BindWithRetry(addr, port)) return false;
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

// Si el puerto está ocupado se reintenta unos segundos (un host que se está cerrando tarda en soltarlo). Si lo
// tiene un desklink-host antiguo (sin el relevo automático de main.cpp), se cierra; si es otro programa, se dice cuál.
bool UdpEndpoint::BindWithRetry(const sockaddr_in& addr, uint16_t port) {
  bool killed = false;
  for (int attempt = 0;; ++attempt) {
    if (::bind(socket_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) return true;
    const int err = WSAGetLastError();
    if (err != WSAEADDRINUSE) {
      std::fprintf(stderr, "Error: no se pudo abrir el puerto UDP %u (error %d).\n", port, err);
      return false;
    }
    const DWORD pid = UdpPortOwner(port);
    const std::string owner = DescribeProcess(pid);
    if (!killed && pid && pid != GetCurrentProcessId() && _strnicmp(owner.c_str(), "desklink-host.exe", 17) == 0) {
      std::printf("Cierro una versión anterior de DeskLink que seguía abierta: %s.\n", owner.c_str());
      if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid)) {
        TerminateProcess(h, 0);
        WaitForSingleObject(h, 5000);
        CloseHandle(h);
      }
      killed = true;
      continue;
    }
    if (attempt >= 10) {
      std::fprintf(stderr,
                   "Error: el puerto UDP %u, el de DeskLink, lo está usando %s. Cierra ese programa y vuelve a "
                   "abrir DeskLink.\n",
                   port, owner.c_str());
      return false;
    }
    Sleep(300);
  }
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
