#include "virtual_display.h"

#include <cfgmgr32.h>
#include <initguid.h>

#include <chrono>
#include <cstdio>
#include <vector>

#include "../../driver/include/desklink_idd.h"

namespace {

std::wstring FindDevicePath() {
  ULONG size = 0;
  if (CM_Get_Device_Interface_List_SizeW(&size, const_cast<GUID*>(&GUID_DEVINTERFACE_DESKLINK_IDD), nullptr,
                                         CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
      size <= 1) {
    return {};
  }
  std::vector<wchar_t> list(size);
  if (CM_Get_Device_Interface_ListW(const_cast<GUID*>(&GUID_DEVINTERFACE_DESKLINK_IDD), nullptr, list.data(), size,
                                    CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
    return {};
  }
  return list.data();  // la primera de la lista
}

// Busca el monitor llamado «DeskLink». Si está activo, devuelve el nombre GDI de su salida;
// si existe pero Windows no lo usa (p. ej. modo «duplicar»), lo indica en *inactive.
std::wstring FindDeskLinkSource(bool* inactive) {
  *inactive = false;
  UINT32 path_count = 0, mode_count = 0;
  if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) return {};
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
  if (QueryDisplayConfig(QDC_ALL_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) !=
      ERROR_SUCCESS) {
    return {};
  }
  for (UINT32 i = 0; i < path_count; ++i) {
    const auto& p = paths[i];
    if (!p.targetInfo.targetAvailable) continue;
    DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
    target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size = sizeof(target);
    target.header.adapterId = p.targetInfo.adapterId;
    target.header.id = p.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) continue;
    if (wcscmp(target.monitorFriendlyDeviceName, DESKLINK_MONITOR_NAME) != 0) continue;
    if (!(p.flags & DISPLAYCONFIG_PATH_ACTIVE)) {
      *inactive = true;
      continue;
    }
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = p.sourceInfo.adapterId;
    source.header.id = p.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS) return source.viewGdiDeviceName;
  }
  return {};
}

}  // namespace

bool VirtualDisplay::Ioctl(DWORD code, const void* in, DWORD in_size) {
  DWORD returned = 0;
  return DeviceIoControl(device_, code, const_cast<void*>(in), in_size, nullptr, 0, &returned, nullptr) != FALSE;
}

bool VirtualDisplay::Plug(UINT width, UINT height, UINT refresh_hz) {
  const std::wstring path = FindDevicePath();
  if (path.empty()) {
    std::fprintf(stderr,
                 "Error: el driver del monitor virtual no está instalado.\n"
                 "Ejecuta instalar-driver.bat (del paquete desklink-driver-windows) o arranca sin --virtual.\n");
    return false;
  }
  device_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, 0, nullptr);
  if (device_ == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    std::fprintf(stderr, "Error: no se pudo abrir el driver del monitor virtual (error %lu).\n", error);
    if (error == ERROR_ACCESS_DENIED) {
      std::fprintf(stderr, "Es un problema de permisos: usa el driver de la última compilación (vuelve a ejecutar\n"
                           "instalar-driver.bat) o abre la consola como administrador.\n");
    }
    return false;
  }

  const DeskLinkPlugArgs args{width, height, refresh_hz};
  bool plugged = false;
  // Justo después de arrancar Windows el adaptador puede tardar un poco en estar listo.
  for (int i = 0; i < 20 && !plugged; ++i) {
    plugged = Ioctl(IOCTL_DESKLINK_PLUG, &args, sizeof(args));
    if (!plugged) Sleep(250);
  }
  if (!plugged) {
    std::fprintf(stderr, "Error: el driver no pudo crear el monitor virtual (error %lu).\n", GetLastError());
    return false;
  }

  pinging_ = true;
  ping_thread_ = std::thread([this] {
    while (pinging_) {
      Ioctl(IOCTL_DESKLINK_PING, nullptr, 0);
      for (int i = 0; i < 10 && pinging_; ++i) Sleep(100);
    }
  });

  // Windows tarda un momento en activar el monitor nuevo. Si lo deja apagado (p. ej. porque las
  // pantallas están en modo «duplicar»), se pide el modo «extender» una vez.
  bool forced_extend = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < deadline) {
    bool inactive = false;
    gdi_name_ = FindDeskLinkSource(&inactive);
    if (!gdi_name_.empty()) {
      EnsureMode(width, height, refresh_hz);
      std::printf("Monitor virtual activo: %ls, %ux%u a %u Hz.\n", gdi_name_.c_str(), width, height, refresh_hz);
      Sleep(300);  // deja que DXGI vea la salida nueva
      return true;
    }
    if (inactive && !forced_extend) {
      forced_extend = true;
      std::printf("El monitor virtual está apagado: pongo las pantallas en modo «extender».\n");
      SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
    }
    Sleep(200);
  }
  std::fprintf(stderr,
               "Error: Windows no activó el monitor virtual. Mira en Configuración > Pantalla que «DeskLink»\n"
               "esté en «Extender estas pantallas».\n");
  return false;
}

// Windows recuerda el último modo de cada monitor y, al volver a enchufar el virtual, puede ponerle ese en vez del
// que se pidió (p. ej. 1280x800 en una tablet de 2944x1840: imagen borrosa). Se le pone el modo pedido, que ya es
// el que el usuario eligió en Windows para esta tablet si lo hizo (main.cpp, SaveSize).
void VirtualDisplay::EnsureMode(UINT width, UINT height, UINT refresh_hz) {
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  if (!EnumDisplaySettingsW(gdi_name_.c_str(), ENUM_CURRENT_SETTINGS, &dm)) return;
  if (dm.dmPelsWidth == width && dm.dmPelsHeight == height) return;
  std::printf("Windows puso el monitor virtual a %lux%lu: lo cambio a %ux%u.\n", dm.dmPelsWidth, dm.dmPelsHeight,
              width, height);
  dm.dmPelsWidth = width;
  dm.dmPelsHeight = height;
  dm.dmDisplayFrequency = refresh_hz;
  dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
  LONG r = ChangeDisplaySettingsExW(gdi_name_.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY, nullptr);
  if (r != DISP_CHANGE_SUCCESSFUL) {
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;  // por si no acepta esos Hz
    r = ChangeDisplaySettingsExW(gdi_name_.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY, nullptr);
  }
  if (r != DISP_CHANGE_SUCCESSFUL) {
    std::fprintf(stderr, "Aviso: Windows no aceptó %ux%u para el monitor virtual (código %ld).\n", width, height, r);
    return;
  }
  // Se espera a que el modo nuevo esté puesto antes de capturar.
  for (int i = 0; i < 20; ++i) {
    DEVMODEW now{};
    now.dmSize = sizeof(now);
    if (EnumDisplaySettingsW(gdi_name_.c_str(), ENUM_CURRENT_SETTINGS, &now) && now.dmPelsWidth == width &&
        now.dmPelsHeight == height) {
      break;
    }
    Sleep(100);
  }
}

void VirtualDisplay::Unplug() {
  pinging_ = false;
  if (ping_thread_.joinable()) ping_thread_.join();
  if (device_ != INVALID_HANDLE_VALUE) {
    Ioctl(IOCTL_DESKLINK_UNPLUG, nullptr, 0);
    CloseHandle(device_);
    device_ = INVALID_HANDLE_VALUE;
  }
  gdi_name_.clear();
}
