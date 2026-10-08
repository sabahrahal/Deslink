#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Carpeta de desklink.exe (sin barra final). desklink-host.exe y adb.exe van junto a él.
std::wstring AppDirectory();

// ¿Está instalado el driver del monitor virtual (desklink-idd)?
bool VirtualDriverInstalled();

// Monitores en el mismo orden que --output del host: «1: DELL U2720Q, 3840×2160 (principal)».
std::vector<std::wstring> MonitorNames();

// IPv4 de las redes conectadas: «192.168.1.50 (Wi-Fi)».
std::vector<std::wstring> LocalAddresses();

// Arranque con Windows (HKCU\...\Run): abre desklink.exe --tray al iniciar sesión.
bool StartsWithWindows();
void SetStartWithWindows(bool on);

// Icono de DeskLink: una tableta con el cuerpo en degradado diagonal de `from` a `to` y la pantalla oscura,
// dibujada con suavizado (mismo dibujo que res/make_icon.py). Para un solo color, from == to.
HICON CreateTabletIcon(COLORREF from, COLORREF to, int size);
