#include "system_info.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <cfgmgr32.h>
#include <dxgi.h>
#include <initguid.h>

#include <cmath>
#include <cstdint>
#include <cwchar>
#include <map>

#include "../../driver/include/desklink_idd.h"

std::wstring AppDirectory() {
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring dir(path, n);
  return dir.substr(0, dir.find_last_of(L'\\'));
}

bool VirtualDriverInstalled() {
  ULONG size = 0;
  return CM_Get_Device_Interface_List_SizeW(&size, const_cast<GUID*>(&GUID_DEVINTERFACE_DESKLINK_IDD), nullptr,
                                            CM_GET_DEVICE_INTERFACE_LIST_PRESENT) == CR_SUCCESS &&
         size > 1;
}

namespace {

// Nombre GDI de cada salida («\\.\DISPLAY1») -> nombre del monitor que da Windows («DELL U2720Q»).
std::map<std::wstring, std::wstring> FriendlyMonitorNames() {
  std::map<std::wstring, std::wstring> names;
  UINT32 path_count = 0, mode_count = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) return names;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) !=
      ERROR_SUCCESS) {
    return names;
  }
  for (UINT32 i = 0; i < path_count; ++i) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = paths[i].sourceInfo.adapterId;
    source.header.id = paths[i].sourceInfo.id;
    DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
    target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size = sizeof(target);
    target.header.adapterId = paths[i].targetInfo.adapterId;
    target.header.id = paths[i].targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS &&
        DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS && target.monitorFriendlyDeviceName[0]) {
      names[source.viewGdiDeviceName] = target.monitorFriendlyDeviceName;
    }
  }
  return names;
}

}  // namespace

std::vector<std::wstring> MonitorNames() {
  std::vector<std::wstring> result;
  const auto friendly = FriendlyMonitorNames();
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return result;
  // Mismo recorrido que CreateGpuForOutput en el host: todas las salidas de todos los adaptadores.
  IDXGIAdapter1* adapter = nullptr;
  for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
    IDXGIOutput* output = nullptr;
    for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
      DXGI_OUTPUT_DESC od{};
      output->GetDesc(&od);
      output->Release();
      const RECT& r = od.DesktopCoordinates;
      std::wstring name = std::to_wstring(result.size() + 1) + L": ";
      const auto it = friendly.find(od.DeviceName);
      name += it != friendly.end() ? it->second : std::wstring(od.DeviceName);
      name += L", " + std::to_wstring(r.right - r.left) + L"×" + std::to_wstring(r.bottom - r.top);
      MONITORINFO mi{sizeof(mi)};
      if (GetMonitorInfoW(od.Monitor, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY)) name += L" (principal)";
      result.push_back(name);
    }
    adapter->Release();
  }
  factory->Release();
  return result;
}

std::vector<std::wstring> LocalAddresses() {
  std::vector<std::wstring> result;
  ULONG size = 16 * 1024;
  std::vector<unsigned char> buf(size);
  auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
  const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
  if (rc == ERROR_BUFFER_OVERFLOW) {
    buf.resize(size);
    list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    rc = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
  }
  if (rc != NO_ERROR) return result;
  for (auto* a = list; a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
      const unsigned char* b = reinterpret_cast<const unsigned char*>(&sin->sin_addr);
      if (b[0] == 169 && b[1] == 254) continue;  // sin DHCP
      wchar_t ip[32];
      swprintf(ip, 32, L"%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
      result.push_back(std::wstring(ip) + L" (" + a->FriendlyName + L")");
    }
  }
  return result;
}

namespace {
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"DeskLink";
}  // namespace

bool StartsWithWindows() {
  return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) ==
         ERROR_SUCCESS;
}

void SetStartWithWindows(bool on) {
  if (!on) {
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
    return;
  }
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  const std::wstring cmd = L"\"" + std::wstring(path, n) + L"\" --tray";
  RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, cmd.c_str(),
                  static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
}

namespace {

// Distancia con signo de (x, y) a un rectángulo redondeado: negativa dentro.
float RoundRectDistance(float x, float y, float x0, float y0, float x1, float y1, float r) {
  const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
  const float hx = (x1 - x0) / 2 - r, hy = (y1 - y0) / 2 - r;
  float dx = (x > cx ? x - cx : cx - x) - hx;
  float dy = (y > cy ? y - cy : cy - y) - hy;
  const float ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
  const float outside = ox * ox + oy * oy;
  const float inside = dx > dy ? dx : dy;
  return (outside > 0 ? std::sqrt(outside) : (inside < 0 ? inside : 0)) - r;
}

}  // namespace

HICON CreateTabletIcon(COLORREF color, int size) {
  BITMAPV5HEADER bi{};
  bi.bV5Size = sizeof(bi);
  bi.bV5Width = size;
  bi.bV5Height = -size;  // de arriba abajo
  bi.bV5Planes = 1;
  bi.bV5BitCount = 32;
  bi.bV5Compression = BI_BITFIELDS;
  bi.bV5RedMask = 0x00FF0000;
  bi.bV5GreenMask = 0x0000FF00;
  bi.bV5BlueMask = 0x000000FF;
  bi.bV5AlphaMask = 0xFF000000;
  void* bits = nullptr;
  HDC dc = GetDC(nullptr);
  HBITMAP color_bmp = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, dc);
  if (!color_bmp) return nullptr;

  // Cuerpo de la tableta en el color del estado y pantalla blanca; 4x4 muestras por píxel para suavizar bordes.
  const float s = static_cast<float>(size);
  auto* px = static_cast<uint32_t*>(bits);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      float body = 0, screen = 0;
      for (int sy = 0; sy < 4; ++sy) {
        for (int sx = 0; sx < 4; ++sx) {
          const float fx = (x + (sx + 0.5f) / 4) / s, fy = (y + (sy + 0.5f) / 4) / s;
          if (RoundRectDistance(fx, fy, 0.15f, 0.27f, 0.85f, 0.73f, 0.03f) < 0) {
            screen += 1;
          } else if (RoundRectDistance(fx, fy, 0.05f, 0.16f, 0.95f, 0.84f, 0.14f) < 0) {
            body += 1;
          }
        }
      }
      body /= 16;
      screen /= 16;
      const float a = body + screen;
      if (a <= 0) {
        px[y * size + x] = 0;
        continue;
      }
      // Mezcla del color del cuerpo y el blanco de la pantalla, en alfa directo (sin premultiplicar).
      const float r = (GetRValue(color) * body + 255 * screen) / a;
      const float g = (GetGValue(color) * body + 255 * screen) / a;
      const float b = (GetBValue(color) * body + 255 * screen) / a;
      px[y * size + x] = (static_cast<uint32_t>(a * 255 + 0.5f) << 24) | (static_cast<uint32_t>(r + 0.5f) << 16) |
                         (static_cast<uint32_t>(g + 0.5f) << 8) | static_cast<uint32_t>(b + 0.5f);
    }
  }

  const std::vector<unsigned char> zeros(((size + 15) / 16) * 2 * size, 0);  // máscara vacía: manda el alfa
  HBITMAP mask = CreateBitmap(size, size, 1, 1, zeros.data());
  ICONINFO ii{};
  ii.fIcon = TRUE;
  ii.hbmColor = color_bmp;
  ii.hbmMask = mask;
  HICON icon = CreateIconIndirect(&ii);
  DeleteObject(color_bmp);
  DeleteObject(mask);
  return icon;
}
