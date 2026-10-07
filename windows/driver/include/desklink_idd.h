// Interfaz entre el host y el driver de monitor virtual (desklink-idd).
// El host abre el dispositivo por su interfaz y le manda IOCTL para enchufar o quitar el monitor.
#pragma once

#include <windows.h>
#include <guiddef.h>
#include <winioctl.h>

#include <cstdint>

// {A602EE2A-F6A8-42D3-853B-597C2DF1205D}
DEFINE_GUID(GUID_DEVINTERFACE_DESKLINK_IDD, 0xa602ee2a, 0xf6a8, 0x42d3, 0x85, 0x3b, 0x59, 0x7c, 0x2d, 0xf1,
            0x20, 0x5d);

// Id de hardware del dispositivo raíz que crea el instalador (debe coincidir con el .inf).
#define DESKLINK_IDD_HARDWARE_ID L"Root\\DeskLinkIdd"

// Nombre con el que aparece el monitor (EDID): el host lo usa para encontrarlo.
#define DESKLINK_MONITOR_NAME L"DeskLink"

// Entrada: DeskLinkPlugArgs. Enchufa el monitor virtual con ese modo (si ya había uno, lo cambia).
#define IOCTL_DESKLINK_PLUG CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
// Sin datos. Quita el monitor virtual.
#define IOCTL_DESKLINK_UNPLUG CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS)
// Sin datos. El host lo manda cada segundo; si pasan kDeskLinkWatchdogMs sin él, el driver quita el monitor
// (así no queda una pantalla fantasma si el host se cierra de golpe).
#define IOCTL_DESKLINK_PING CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS)

constexpr uint32_t kDeskLinkWatchdogMs = 5000;

#pragma pack(push, 1)
struct DeskLinkPlugArgs {
  uint32_t width;
  uint32_t height;
  uint32_t refresh_hz;
};
#pragma pack(pop)
