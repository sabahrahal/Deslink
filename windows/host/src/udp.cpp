#include "udp.h"

#include <iphlpapi.h>
#include <windows.h>
#include <qos2.h>

#include <cstdio>
#include <cstring>
#include <cwctype>
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

// El proceso ya no existe. Si aun así figura como dueño del puerto, otro proceso heredó el socket y lo mantiene
// abierto (Windows apunta el PID de quien lo creó): casi siempre el servidor de adb lanzado por un host antiguo.
bool ProcessGone(DWORD pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return GetLastError() == ERROR_INVALID_PARAMETER;
  DWORD code = 0;
  const bool gone = GetExitCodeProcess(h, &code) && code != STILL_ACTIVE;
  CloseHandle(h);
  return gone;
}

// El host de Extendesk, con su nombre de ahora o el de antes (desklink-host.exe).
bool IsHostExe(const std::string& process) {
  return _strnicmp(process.c_str(), "extendesk-host.exe", 18) == 0 ||
         _strnicmp(process.c_str(), "desklink-host.exe", 17) == 0;
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

bool IsUsbTetherAddress(const in_addr& ip) {
  DWORD index = 0;
  if (GetBestInterface(ip.s_addr, &index) != NO_ERROR) return false;
  ULONG size = 16 * 1024;
  std::vector<uint8_t> buf(size);
  const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
  if (rc == ERROR_BUFFER_OVERFLOW) {
    buf.resize(size);
    rc = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
  }
  if (rc != NO_ERROR) return false;
  for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
    if (a->IfIndex != index) continue;
    // El anclaje de Android aparece como «Remote NDIS based Internet Sharing Device» (RNDIS) o como NCM.
    std::wstring d = a->Description ? a->Description : L"";
    for (auto& c : d) c = static_cast<wchar_t>(towlower(c));
    return d.find(L"ndis") != std::wstring::npos || d.find(L"ncm") != std::wstring::npos ||
           d.find(L"android") != std::wstring::npos || d.find(L"usb") != std::wstring::npos;
  }
  return false;
}

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

bool UdpEndpoint::Bind(uint16_t port, PacketHandler handler, bool usb, std::function<bool()> free_orphaned_port) {
  handler_ = std::move(handler);
  socket_ = NonInheritableSocket(SOCK_DGRAM, IPPROTO_UDP);
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
  if (!BindWithRetry(addr, port, free_orphaned_port)) return false;
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
// tiene un host antiguo (sin el relevo automático de main.cpp, o de antes del nombre Extendesk: desklink-host.exe),
// se cierra; si figura un proceso que ya no existe, el puerto lo retiene quien heredó su socket y se llama a
// free_orphaned_port; si es otro programa, se dice cuál.
bool UdpEndpoint::BindWithRetry(const sockaddr_in& addr, uint16_t port,
                                const std::function<bool()>& free_orphaned_port) {
  bool killed = false;
  bool freed = false;
  for (int attempt = 0;; ++attempt) {
    if (::bind(socket_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) return true;
    const int err = WSAGetLastError();
    if (err != WSAEADDRINUSE) {
      std::fprintf(stderr, "Error: no se pudo abrir el puerto UDP %u (error %d).\n", port, err);
      return false;
    }
    const DWORD pid = UdpPortOwner(port);
    const std::string owner = DescribeProcess(pid);
    if (!killed && pid && pid != GetCurrentProcessId() && IsHostExe(owner)) {
      std::printf("Cierro una versión anterior de Extendesk que seguía abierta: %s.\n", owner.c_str());
      if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid)) {
        TerminateProcess(h, 0);
        WaitForSingleObject(h, 5000);
        CloseHandle(h);
      }
      killed = true;
      continue;
    }
    const bool orphaned = pid && ProcessGone(pid);
    if (orphaned && !freed && free_orphaned_port) {
      freed = true;
      std::printf("El puerto UDP %u sigue retenido por un Extendesk que ya se cerró (PID %lu): cierro el servidor de "
                  "adb, que lo heredó.\n",
                  port, pid);
      if (free_orphaned_port()) continue;
    }
    if (attempt >= 10 && orphaned) {
      std::fprintf(stderr,
                   "Error: el puerto UDP %u, el de Extendesk, lo retiene un programa que lo heredó de un Extendesk ya "
                   "cerrado (PID %lu), casi siempre adb. Cierra adb.exe en el Administrador de tareas (o ejecuta "
                   "«adb kill-server») y vuelve a abrir Extendesk.\n",
                   port, pid);
      return false;
    }
    if (attempt >= 10) {
      std::fprintf(stderr,
                   "Error: el puerto UDP %u, el de Extendesk, lo está usando %s. Cierra ese programa y vuelve a "
                   "abrir Extendesk.\n",
                   port, owner.c_str());
      return false;
    }
    Sleep(300);
  }
}

void UdpEndpoint::PrioritizeFlowTo(const sockaddr_in& peer) {
  if (IsUsb(peer) || socket_ == INVALID_SOCKET) return;
  if (qos_flow_ && qos_peer_.sin_addr.s_addr == peer.sin_addr.s_addr && qos_peer_.sin_port == peer.sin_port) return;
  if (!qos_) {
    QOS_VERSION version{1, 0};
    if (!QOSCreateHandle(&version, &qos_)) {
      std::printf("Wi-Fi: no se pudo pedir prioridad de vídeo (qWAVE, error %lu); se envía sin marcar.\n",
                  GetLastError());
      qos_ = INVALID_HANDLE_VALUE;
    }
  }
  if (qos_ == INVALID_HANDLE_VALUE) return;
  if (qos_flow_) {
    QOSRemoveSocketFromFlow(qos_, socket_, qos_flow_, 0);
    qos_flow_ = 0;
  }
  QOS_FLOWID flow = 0;
  sockaddr_in dest = peer;
  if (QOSAddSocketToFlow(qos_, socket_, reinterpret_cast<sockaddr*>(&dest), QOSTrafficTypeAudioVideo,
                         QOS_NON_ADAPTIVE_FLOW, &flow)) {
    qos_flow_ = flow;
    qos_peer_ = peer;
    std::printf("Wi-Fi: vídeo marcado como prioritario (cola WMM de vídeo).\n");
  } else {
    std::printf("Wi-Fi: no se pudo marcar el vídeo como prioritario (qWAVE, error %lu).\n", GetLastError());
  }
}

void UdpEndpoint::SetPeer(const sockaddr_in& peer) {
  std::lock_guard<std::mutex> lock(mutex_);
  PrioritizeFlowTo(peer);
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

void UdpEndpoint::ForgetPeer() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (has_peer_) peer_lost_ = true;
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
  if (qos_ && qos_ != INVALID_HANDLE_VALUE) {
    QOSCloseHandle(qos_);  // también saca el socket de su flujo
    qos_ = nullptr;
    qos_flow_ = 0;
  }
  if (socket_ != INVALID_SOCKET) {
    closesocket(socket_);
    socket_ = INVALID_SOCKET;
  }
}
