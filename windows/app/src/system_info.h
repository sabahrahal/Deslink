#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Carpeta de extendesk.exe (sin barra final). extendesk-host.exe y adb.exe van junto a él.
std::wstring AppDirectory();

// ¿Está instalado el driver del monitor virtual (desklink-idd)?
bool VirtualDriverInstalled();

// Monitores en el mismo orden que --output del host: «1: DELL U2720Q, 3840×2160 (principal)».
std::vector<std::wstring> MonitorNames();

// IPv4 de las redes conectadas: «192.168.1.50 (Wi-Fi)».
std::vector<std::wstring> LocalAddresses();

// Arranque con Windows (HKCU\...\Run): abre extendesk.exe --tray al iniciar sesión.
bool StartsWithWindows();
void SetStartWithWindows(bool on);
