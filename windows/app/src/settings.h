#pragma once

#include <string>

// Ajustes de Extendesk. El PC es el único sitio donde se eligen (docs/ajustes.md): la tablet solo dice lo que puede
// hacer y enseña lo que el PC decide. Se guardan en %APPDATA%\Extendesk\extendesk.ini
// (antes %APPDATA%\DeskLink\desklink.ini: se pasa sola la primera vez).
//
// A la vista en la ventana: modo, resolución (o el monitor que se duplica), calidad y dónde suena el sonido. En
// «Configuración avanzada»: el táctil, la red Wi-Fi propia del PC, abrir con Windows y el idioma. Lo demás es automático y ya no
// se elige (docs/ajustes.md): el cable USB siempre está listo, la app de la tablet se abre al enchufarla y se
// transmite nada más abrir. Bajar la calidad (y el brillo) por la batería de la tablet es opcional y viene apagado:
// encendido de serie, una tablet en un puerto USB del PC (que da menos de lo que gasta) bajaba sola a Ahorro y a
// Mínimo aunque se hubiera elegido Equilibrada. fps, Mbps, códec y puerto
// solo se cambian a mano en el .ini (vacío o 0 = automático: lo decide el host con lo que dice la tablet).
// La resolución de la pantalla extra se cambia en Configuración de pantalla de Windows; el host la recuerda.
struct Settings {
  bool virtual_display = true;  // monitor virtual (driver) o duplicar un monitor existente
  int output = 0;               // monitor que se duplica (índice DXGI, como --output)
  std::wstring quality = L"alta";  // «alta», «equilibrada» o «ahorro»
  bool audio_tablet = false;    // el sonido del PC suena en la tablet (si no, en el PC)
  int touch = 0;                // 0 táctil, 2 solo ver (el 1, ratón, ya no se elige)
  std::wstring size;            // ya no se usa (la resolución se elige en Windows); se conserva para leer .ini viejos
  int fps = 0;                  // solo en el .ini; 0 = automático (según la pantalla de la tablet y la gráfica)
  int bitrate = 0;              // solo en el .ini; Mbps, 0 = automático
  std::wstring codec;           // solo en el .ini; «hevc» o «h264», vacío = automático (H.264 si decodifica antes)
  int port = 47800;             // solo en el .ini
  std::wstring adb;             // ruta de adb.exe; vacía = la busca el host
  std::wstring extra;           // opciones extra para extendesk-host.exe
  bool battery_adapt = false;   // la tablet puede pedir bajar fps, Mbps y brillo si su batería no aguanta
  std::wstring language;        // idioma de la ventana («en», «es»); vacío = el de Windows (i18n.h)
  int window_w = 0;             // tamaño del área de la ventana que dejó el usuario, a 96 ppp; 0 = el del contenido
  int window_h = 0;
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

// Opciones de línea de comandos de extendesk-host.exe para estos ajustes (sin --status ni --stop-event).
std::wstring HostArguments(const Settings& s);

// Línea «@ajustes ...» con lo que el host puede cambiar en marcha (calidad, fps, Mbps, códec, sonido y táctil); se
// le manda por la entrada estándar al cambiar algo en la ventana.
std::wstring LiveSettingsLine(const Settings& s);

// Lo que solo se aplica reiniciando el host (modo, monitor, puerto): distinto entre a y b.
bool NeedsRestart(const Settings& a, const Settings& b);
