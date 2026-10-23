#include "usb.h"

#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

#include "dlp/packet.h"

SOCKET NonInheritableSocket(int type, int protocol) {
  // WSA_FLAG_OVERLAPPED: lo mismo que socket(), para que SO_RCVTIMEO funcione.
  return ::WSASocketW(AF_INET, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
}

UsbLink::~UsbLink() { Stop(); }

bool UsbLink::Start(uint16_t port, PacketHandler on_packet, CloseHandler on_close) {
  on_packet_ = std::move(on_packet);
  on_close_ = std::move(on_close);
  listen_ = NonInheritableSocket(SOCK_STREAM, IPPROTO_TCP);
  if (listen_ == INVALID_SOCKET) return false;
  // Solo en loopback: el único que llega aquí es el servidor adb del propio PC.
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
      ::listen(listen_, 2) == SOCKET_ERROR) {
    std::fprintf(stderr, "USB: no se pudo escuchar en TCP 127.0.0.1:%u (%d)\n", port, WSAGetLastError());
    closesocket(listen_);
    listen_ = INVALID_SOCKET;
    return false;
  }
  thread_ = std::thread([this] { Loop(); });
  return true;
}

void UsbLink::Loop() {
  std::vector<uint8_t> buf(64 * 1024);
  size_t have = 0;
  while (!stop_) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(listen_, &rd);
    const SOCKET c = client_;
    if (c != INVALID_SOCKET) FD_SET(c, &rd);
    timeval tv{0, 200'000};  // para poder salir del bucle al parar
    if (::select(0, &rd, nullptr, nullptr, &tv) <= 0) continue;

    if (FD_ISSET(listen_, &rd)) {
      SOCKET s = ::accept(listen_, nullptr, nullptr);
      if (s != INVALID_SOCKET) {
        SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
        if (c != INVALID_SOCKET) {
          CloseClient();
          on_close_();
        }
        // Sin Nagle: cada paquete sale en cuanto se escribe. El búfer de envío es pequeño a propósito:
        // si el cable no da abasto, mejor enterarse pronto que acumular latencia.
        BOOL nodelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        int send_buf = 1024 * 1024;
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&send_buf), sizeof(send_buf));
        DWORD send_timeout_ms = 1000;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&send_timeout_ms),
                   sizeof(send_timeout_ms));
        have = 0;
        client_ = s;
      }
      continue;
    }

    if (c == INVALID_SOCKET || !FD_ISSET(c, &rd)) continue;
    const int n = ::recv(c, reinterpret_cast<char*>(buf.data() + have), static_cast<int>(buf.size() - have), 0);
    if (n <= 0) {
      CloseClient();
      on_close_();
      have = 0;
      continue;
    }
    have += static_cast<size_t>(n);
    size_t off = 0;
    bool bad = false;
    while (have - off >= 2) {
      const size_t len = buf[off] | (buf[off + 1] << 8);
      if (len == 0 || len > dlp::kMaxPacketSize) {
        bad = true;
        break;
      }
      if (have - off - 2 < len) break;
      on_packet_(buf.data() + off + 2, len);
      off += 2 + len;
    }
    if (bad) {
      std::fprintf(stderr, "USB: datos no válidos de la tablet, cierro la conexión.\n");
      CloseClient();
      on_close_();
      have = 0;
      continue;
    }
    if (off > 0) {
      std::memmove(buf.data(), buf.data() + off, have - off);
      have -= off;
    }
  }
}

bool UsbLink::SendAll(const uint8_t* data, size_t size) {
  const SOCKET c = client_;
  while (size > 0) {
    const int n = ::send(c, reinterpret_cast<const char*>(data), static_cast<int>(size), 0);
    if (n == SOCKET_ERROR) return false;
    data += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

void UsbLink::Send(const std::vector<std::vector<uint8_t>>& packets) {
  std::lock_guard<std::mutex> lock(send_mutex_);
  const SOCKET c = client_;
  if (c == INVALID_SOCKET) return;
  send_buf_.clear();
  for (const auto& p : packets) {
    send_buf_.push_back(static_cast<uint8_t>(p.size() & 0xFF));
    send_buf_.push_back(static_cast<uint8_t>(p.size() >> 8));
    send_buf_.insert(send_buf_.end(), p.begin(), p.end());
  }
  // El hilo de recepción ve el corte y cierra el socket (cerrarlo aquí podría reutilizar el handle a medias).
  if (!SendAll(send_buf_.data(), send_buf_.size())) ::shutdown(c, SD_BOTH);
}

void UsbLink::CloseClient() {
  std::lock_guard<std::mutex> lock(send_mutex_);
  const SOCKET c = client_.exchange(INVALID_SOCKET);
  if (c != INVALID_SOCKET) closesocket(c);
}

void UsbLink::Stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  CloseClient();
  if (listen_ != INVALID_SOCKET) {
    closesocket(listen_);
    listen_ = INVALID_SOCKET;
  }
}

namespace {

std::string Utf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

bool FileExists(const std::wstring& path) {
  const DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Junto al .exe (para repartirlo todo en una carpeta), en el PATH o en el SDK de Android.
std::wstring FindAdb() {
  wchar_t buf[MAX_PATH];
  const DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (len > 0 && len < MAX_PATH) {
    std::wstring dir(buf, len);
    dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
    if (FileExists(dir + L"adb.exe")) return dir + L"adb.exe";
    if (FileExists(dir + L"platform-tools\\adb.exe")) return dir + L"platform-tools\\adb.exe";
  }
  if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, buf, nullptr) > 0) return buf;
  for (const wchar_t* var : {L"ANDROID_HOME", L"ANDROID_SDK_ROOT"}) {
    if (GetEnvironmentVariableW(var, buf, MAX_PATH) > 0) {
      const std::wstring p = std::wstring(buf) + L"\\platform-tools\\adb.exe";
      if (FileExists(p)) return p;
    }
  }
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH) > 0) {
    const std::wstring p = std::wstring(buf) + L"\\Android\\Sdk\\platform-tools\\adb.exe";
    if (FileExists(p)) return p;
  }
  return {};
}

// Ejecuta adb sin ventana y recoge su salida. Se espera a que el proceso termine, no al final de la tubería:
// la primera vez adb arranca su servidor en segundo plano y este podría quedarse con la tubería abierta.
// adb solo hereda su tubería de salida: si heredara todo (el socket UDP del host, su salida hacia la app…), el
// servidor de adb, que sigue vivo cuando el host se cierra, se quedaría con el puerto 47800 y el siguiente host
// no podría abrirlo («lo está usando PID x», un PID que ya no existe).
bool RunAdb(const std::wstring& adb, const std::wstring& args, std::string* out, DWORD timeout_ms,
            const std::atomic<bool>* stop) {
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE read_end = nullptr, write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &sa, 0)) return false;
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
  std::vector<uint8_t> attr_buf(attr_size);
  auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
  HANDLE inherit[] = {write_end};
  if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) {
    CloseHandle(read_end);
    CloseHandle(write_end);
    return false;
  }
  const bool listed = UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                                sizeof(inherit), nullptr, nullptr);

  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdOutput = write_end;
  si.StartupInfo.hStdError = write_end;
  si.StartupInfo.hStdInput = nullptr;
  si.lpAttributeList = attrs;
  PROCESS_INFORMATION pi{};
  std::wstring cmd = L"\"" + adb + L"\" " + args;
  // Sin la lista de handles no se lanza: heredarlo todo es justo lo que deja el puerto ocupado.
  const BOOL ok = listed && CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                           CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                           &si.StartupInfo, &pi);
  DeleteProcThreadAttributeList(attrs);
  CloseHandle(write_end);
  if (!ok) {
    CloseHandle(read_end);
    return false;
  }

  auto drain = [&] {
    DWORD avail = 0;
    while (PeekNamedPipe(read_end, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
      char chunk[4096];
      DWORD got = 0;
      if (!ReadFile(read_end, chunk, std::min<DWORD>(avail, sizeof(chunk)), &got, nullptr) || got == 0) break;
      if (out) out->append(chunk, got);
    }
  };
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  bool exited = false;
  while (!(exited = WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0)) {
    drain();
    if (GetTickCount64() > deadline || (stop && *stop)) break;
  }
  drain();
  DWORD code = 1;
  if (exited) {
    GetExitCodeProcess(pi.hProcess, &code);
  } else {
    TerminateProcess(pi.hProcess, 1);
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(read_end);
  return exited && code == 0;
}

}  // namespace

AdbReverse::~AdbReverse() { Stop(); }

void AdbReverse::Start(uint16_t port, std::wstring adb_path) {
  port_ = port;
  adb_ = std::move(adb_path);
  thread_ = std::thread([this] { Loop(); });
}

bool AdbReverse::Run(const std::wstring& args, std::string* out, DWORD timeout_ms) {
  return RunAdb(adb_, args, out, timeout_ms, &stop_);
}

void AdbReverse::Loop() {
  if (adb_.empty()) adb_ = FindAdb();
  if (adb_.empty()) {
    missing_ = true;
    std::printf("USB por cable: no encontré adb.exe. Copia adb.exe, AdbWinApi.dll y AdbWinUsbApi.dll (de las "
                "platform-tools de Android) junto a extendesk-host.exe para conectar la tablet por USB sin anclaje.\n");
    return;
  }
  std::printf("USB por cable: usando %s\n", Utf8(adb_).c_str());
  const std::wstring tunnel = L"tcp:" + std::to_wstring(port_) + L" tcp:" + std::to_wstring(port_);
  bool server_warned = false;
  bool none_warned = false;
  const ULONGLONG started = GetTickCount64();

  while (!stop_) {
    std::string out;
    if (Run(L"devices", &out)) {
      std::set<std::string> present;
      int unauthorized = 0;
      std::istringstream lines(out);
      std::string line;
      while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string serial = line.substr(0, tab);
        const std::string state = line.substr(tab + 1);
        // Solo cable: las tablets emparejadas por «depuración inalámbrica» (ip:puerto, mDNS) usan el WiFi.
        if (serial.empty() || serial.find(':') != std::string::npos || serial.find("._adb") != std::string::npos ||
            serial.find('"') != std::string::npos) {
          continue;
        }
        if (state == "device") {
          present.insert(serial);
          if (reversed_.count(serial)) continue;
          if (Run(L"-s \"" + Widen(serial) + L"\" reverse " + tunnel, nullptr)) {
            reversed_.insert(serial);
            warned_.erase(serial);
            // Se abre la app en la tablet, que conecta sola por el cable. Si no está instalada, am start falla y
            // no pasa nada. A diferencia del modo accesorio USB, no deja nada registrado en la tablet.
            const bool opened = open_app_ && !busy_ &&
                                Run(L"-s \"" + Widen(serial) + L"\" shell am start -n app.extendesk/app.extendesk.MainActivity "
                                    L"-a app.extendesk.action.USB_ATTACHED", nullptr, 5000);
            std::printf(opened ? "Tablet %s conectada por USB (ADB): abro Extendesk en ella.\n"
                               : "Tablet %s conectada por USB (ADB). Abre Extendesk en ella y pulsa Conectar.\n",
                        serial.c_str());
          }
        } else if (state == "unauthorized") {
          ++unauthorized;
          if (warned_.insert(serial).second) {
            std::printf("Tablet %s enchufada por USB: acepta «¿Permitir depuración USB?» en su pantalla "
                        "(marca «Permitir siempre desde este ordenador»).\n", serial.c_str());
          }
        }
      }
      // Sin ninguna tablet a la vista: casi siempre es la depuración USB apagada o un cable solo de carga.
      if (present.empty() && warned_.empty() && !none_warned && GetTickCount64() - started > 8000) {
        none_warned = true;
        std::printf("USB por cable: no veo ninguna tablet. Si está enchufada, activa en ella Opciones de desarrollador "
                    "→ Depuración USB y usa un cable de datos; no hace falta el anclaje de red.\n");
      }
      if (!present.empty()) none_warned = true;
      for (auto it = reversed_.begin(); it != reversed_.end();) {
        if (present.count(*it)) {
          ++it;
        } else {
          std::printf("Tablet %s desconectada del USB.\n", it->c_str());
          it = reversed_.erase(it);
        }
      }
      ready_ = static_cast<int>(reversed_.size());
      unauthorized_ = unauthorized;
    } else if (!server_warned && !stop_) {
      server_warned = true;
      std::printf("USB por cable: adb no responde (¿otro programa usa adb?). Lo sigo intentando.\n");
    }
    for (int i = 0; i < 20 && !stop_; ++i) Sleep(100);
  }
}

void AdbReverse::Stop() {
  if (!thread_.joinable()) return;
  stop_ = true;
  thread_.join();
  // Se quita el túnel para no dejar nada puesto en la tablet al cerrar.
  stop_ = false;
  for (const auto& serial : reversed_) {
    Run(L"-s \"" + Widen(serial) + L"\" reverse --remove tcp:" + std::to_wstring(port_), nullptr, 2000);
  }
  reversed_.clear();
  ready_ = 0;
}

bool StopAdbServer(const std::wstring& adb_path) {
  const std::wstring adb = adb_path.empty() ? FindAdb() : adb_path;
  return !adb.empty() && RunAdb(adb, L"kill-server", nullptr, 5000, nullptr);
}
