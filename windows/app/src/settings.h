#pragma once

#include <string>

// Ajustes de la app de bandeja. Se guardan en %APPDATA%\DeskLink\desklink.ini.
// La calidad, los fps, el códec, la resolución, el sonido y el táctil se eligen en la tablet y llegan al conectar
// (HELLO, QUALITY_REQUEST, AUDIO_CONTROL, INPUT_MODE). Aquí solo está lo que la tablet no puede saber. fps, bitrate,
// codec y size siguen en el .ini para fijarlos desde el PC (mandan sobre la tablet), sin control en la ventana.
struct Settings {
  bool virtual_display = true;  // monitor virtual (driver) o duplicar un monitor existente
  int output = 0;               // monitor que se duplica (índice DXGI, como --output)
  std::wstring size;            // solo .ini: resolución fija del monitor virtual («2560x1600»); vacía = la de la tablet
  int fps = 0;                  // solo .ini: 0 = los de la tablet
  int bitrate = 0;              // solo .ini: Mbps; 0 = los de la tablet
  std::wstring codec;           // solo .ini: «hevc» o «h264»; vacío = el de la tablet
  bool cursor = true;
  bool usb = true;              // cable USB por adb reverse
  bool stream_on_open = true;   // empezar a transmitir al abrir la app (y al arrancar con Windows)
  int port = 47800;
  std::wstring adb;             // ruta de adb.exe; vacía = la busca el host
  std::wstring extra;           // opciones extra para desklink-host.exe
};

std::wstring SettingsPath();
// Devuelve false si aún no hay archivo de ajustes (primera vez): s queda con los valores por defecto.
// Los .ini de versiones anteriores guardaban fps, bitrate, codec y size de la ventana: se borran al leerlos
// para que mande la tablet.
bool LoadSettings(Settings* s);
void SaveSettings(const Settings& s);

// «2560x1600» (también con «×» o espacios). Mismos límites que el host.
bool ParseSize(const std::wstring& text, unsigned* w, unsigned* h);

// Hay fps, bitrate, códec o resolución fijados en el .ini (mandan sobre la tablet). *text: cuáles, para enseñarlo.
bool HasPcOverrides(const Settings& s, std::wstring* text);

// Opciones de línea de comandos de desklink-host.exe para estos ajustes (sin --status ni --stop-event).
std::wstring HostArguments(const Settings& s);
