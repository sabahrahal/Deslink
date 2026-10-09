#pragma once

#include <string>

// Ajustes de DeskLink. El PC es el único sitio donde se eligen (docs/ajustes.md): la tablet solo dice lo que puede
// hacer y enseña lo que el PC decide. Se guardan en %APPDATA%\DeskLink\desklink.ini.
//
// A la vista en la ventana: modo, calidad, dónde suena el sonido y el táctil. En «Configuración avanzada»: resolución,
// fps, Mbps, códec, monitor que se duplica, bajar la calidad si la batería de la tablet no aguanta, puntero, cable USB
// y el arranque. Vacío o 0 = automático (lo decide el host con lo que dice la tablet).
struct Settings {
  bool virtual_display = true;  // monitor virtual (driver) o duplicar un monitor existente
  int output = 0;               // monitor que se duplica (índice DXGI, como --output)
  std::wstring quality = L"alta";  // «alta», «equilibrada» o «ahorro»
  bool audio_tablet = false;    // el sonido del PC suena en la tablet (si no, en el PC)
  int touch = 0;                // 0 táctil, 1 ratón, 2 solo ver
  std::wstring size;            // resolución fija del monitor virtual («2560x1600»); vacía = la de la tablet
  int fps = 0;                  // 0 = automático (según la pantalla de la tablet y lo que aguante la gráfica)
  int bitrate = 0;              // Mbps; 0 = automático
  std::wstring codec;           // «hevc» o «h264»; vacío = automático
  bool battery_adapt = false;   // la tablet puede pedir bajar fps y tasa si su batería no aguanta
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

// --- Sin Windows (settings_args.cpp), con pruebas en tests/ ---

// «2560x1600» (también con «×» o espacios). Mismos límites que el host.
bool ParseSize(const std::wstring& text, unsigned* w, unsigned* h);

// Deja cada valor dentro de lo admitido (lo que venga del .ini puede estar escrito a mano).
void Sanitize(Settings* s);

// Opciones de línea de comandos de desklink-host.exe para estos ajustes (sin --status ni --stop-event).
std::wstring HostArguments(const Settings& s);

// Línea «@ajustes ...» con lo que el host puede cambiar en marcha (calidad, fps, Mbps, códec, sonido, táctil,
// batería y resolución); se le manda por la entrada estándar al cambiar algo en la ventana.
std::wstring LiveSettingsLine(const Settings& s);

// Lo que solo se aplica reiniciando el host (modo, monitor, puntero, cable USB, puerto): distinto entre a y b.
bool NeedsRestart(const Settings& a, const Settings& b);
