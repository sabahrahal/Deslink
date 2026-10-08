#pragma once

#include <string>

// Ajustes de la app de bandeja. Se guardan en %APPDATA%\DeskLink\desklink.ini; los que no tienen control en la
// ventana (puerto, ruta de adb, opciones extra) solo se cambian editando ese archivo.
struct Settings {
  bool virtual_display = true;  // monitor virtual (driver) o duplicar un monitor existente
  int output = 0;               // monitor que se duplica (índice DXGI, como --output)
  std::wstring size;            // resolución del monitor virtual («2560x1600»); vacía = la última usada
  int fps = 60;
  int bitrate = 50;             // Mbps
  bool hevc = true;             // HEVC o H.264
  int touch = 0;                // 0 = táctil, 1 = el toque mueve el ratón, 2 = sin control táctil
  bool cursor = true;
  bool usb = true;              // cable USB por adb reverse
  bool stream_on_open = true;   // empezar a transmitir al abrir la app (y al arrancar con Windows)
  int port = 47800;
  std::wstring adb;             // ruta de adb.exe; vacía = la busca el host
  std::wstring extra;           // opciones extra para desklink-host.exe
};

std::wstring SettingsPath();
// Devuelve false si aún no hay archivo de ajustes (primera vez): s queda con los valores por defecto.
bool LoadSettings(Settings* s);
void SaveSettings(const Settings& s);

// «2560x1600» (también con «×» o espacios). Mismos límites que el host.
bool ParseSize(const std::wstring& text, unsigned* w, unsigned* h);

// Opciones de línea de comandos de desklink-host.exe para estos ajustes (sin --status ni --stop-event).
std::wstring HostArguments(const Settings& s);
