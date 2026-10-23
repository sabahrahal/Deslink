// Extendesk: app de bandeja para Windows. Arranca desklink-host.exe sin consola con los ajustes guardados, muestra
// el estado de la conexión (cable USB o Wi-Fi, fps, Mbps, RTT) y explica los errores con palabras normales.
// La captura y la codificación siguen en desklink-host.exe, que también se puede usar solo desde una consola.
//
// El PC manda: calidad, sonido y táctil se eligen aquí y el host se los dice a la tablet. Lo que se ve sale de
// view_model.h (probado en tests/test_view_model.cpp); este archivo solo lo pinta con GDI+, en una columna, con
// los colores de design/tokens.json en claro u oscuro según Windows (docs/design.md).
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <objidl.h>
#include <shellapi.h>

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include <cmath>
#include <cstdarg>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "host_process.h"
#include "logo.h"
#include "settings.h"
#include "system_info.h"
#include "tokens.h"
#include "view_model.h"

namespace gp = Gdiplus;

namespace {

constexpr wchar_t kMainClass[] = L"DeskLinkMain";
constexpr wchar_t kLogClass[] = L"DeskLinkLog";
constexpr wchar_t kAppName[] = L"Extendesk";
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_APP_SHOW = WM_APP + 4;  // otra copia de la app pide mostrar esta ventana
constexpr UINT WM_DRIVER_DONE = WM_APP + 5;  // terminó la instalación del driver; lParam = código de salida
constexpr UINT kTrayId = 1;
constexpr UINT_PTR kTimerRestart = 1;
constexpr UINT_PTR kTimerRefresh = 2;
constexpr UINT_PTR kTimerStopKill = 3;
constexpr UINT_PTR kTimerPulse = 4;
constexpr ULONGLONG kStableRunMs = 15000;  // si el host cae después de funcionar este tiempo, se reinicia solo

enum MenuId { kMenuOpen = 200, kMenuToggle, kMenuVirtual, kMenuMirror, kMenuAutostart, kMenuExit, kMenuPick = 300 };
enum ChildId { kIdLogEdit = 100, kIdLogCopy };

gp::Color C(COLORREF c, BYTE alpha = 255) { return gp::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c)); }

constexpr int kClientW = 540;  // ancho del área cliente a 96 ppp; el alto sale del contenido
constexpr float kMargin = 24;
constexpr float kPad = 16;  // relleno dentro de las tarjetas

struct Hit {
  gp::RectF rect;
  std::function<void()> action;
};

struct App {
  HINSTANCE inst = nullptr;
  HWND wnd = nullptr;
  HWND log_wnd = nullptr;
  HWND log_edit = nullptr;
  int dpi = 96;
  float scale = 1;
  bool dark = false;  // Windows en modo oscuro (AppsUseLightTheme = 0)
  HFONT mono_font = nullptr, ui_font = nullptr;
  HBRUSH log_bg = nullptr;
  HICON tray_icons[4] = {};  // detenido, esperando, conectado, error
  HICON app_icon = nullptr, app_icon_small = nullptr;
  UINT taskbar_created = 0;
  ULONG_PTR gdiplus_token = 0;

  // Dibujo
  std::unique_ptr<gp::FontFamily> family;
  std::wstring semibold_family;
  // Montserrat (la del logo), de los recursos: Black para el nombre y los títulos grandes, Bold para el resto.
  std::unique_ptr<gp::PrivateFontCollection> brand_fonts;
  std::unique_ptr<gp::FontFamily> brand_black, brand_bold;
  std::vector<Hit> hits;
  int hover = -1;
  bool mouse_tracked = false;
  float pulse = 0;            // el punto de estado late mientras espera
  gp::RectF pulse_rect;       // zona que se repinta con la animación
  float content_bottom = 0;   // alto que necesita el contenido, con lo avanzado abierto (para desplazar)
  float base_bottom = 0;      // alto sin lo avanzado: el de la ventana, para que no salte al abrirlo
  float scroll = 0;           // desplazamiento vertical cuando el contenido no cabe en la pantalla
  float advanced_top = 0;     // dónde empieza «Configuración avanzada» (para llevarla a la vista al abrirla)
  bool reveal_advanced = false;

  // Datos
  Settings settings;
  HostProcess host;
  State state = State::kStopped;
  bool restart_after_stop = false;  // al terminar el host se vuelve a arrancar (cambio de modo, «Aplicar cambios»)
  ULONGLONG started_at = 0;
  std::wstring args;                // opciones con las que corre el host
  std::wstring error;               // mensaje para el usuario
  bool error_mapped = false;        // error ya traducido a algo claro: no lo pisan las líneas siguientes
  std::map<std::wstring, std::wstring> stats;  // última línea «@estado»
  std::wstring link = L"ninguno";
  std::deque<std::wstring> log;
  bool dirty = false;               // ajustes que piden reiniciar la transmisión
  bool advanced_open = false;       // «Configuración avanzada» desplegada
  bool tray_hint_shown = false;
  bool driver_installed = false;
  bool repairing_driver = false;
  bool autostart = false;
  std::vector<std::wstring> monitors;
  std::wstring addresses;
} g;

const tokens::Colors& P() { return g.dark ? tokens::kDark : tokens::kLight; }

float F(float v) { return v * g.scale; }

std::wstring Format(const wchar_t* fmt, ...) {
  wchar_t buf[512];
  va_list args;
  va_start(args, fmt);
  _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
  va_end(args);
  return buf;
}

bool Running() { return g.state != State::kStopped && g.state != State::kError; }

void Redraw() {
  if (g.wnd) InvalidateRect(g.wnd, nullptr, FALSE);
}

COLORREF ToneColor(Tone t) {
  switch (t) {
    case Tone::kNormal: return P().text;
    case Tone::kSuccess: return P().success;
    case Tone::kWarning: return P().warning;
    case Tone::kError: return P().error;
    default: return P().text_secondary;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Mensajes del host traducidos a algo que el usuario pueda resolver.

std::wstring FriendlyError(const std::wstring& line) {
  auto has = [&](const wchar_t* s) { return line.find(s) != std::wstring::npos; };
  if (has(L"driver del monitor virtual no está instalado")) {
    return L"Falta el driver de la pantalla extra. Pulsa «Instalar la pantalla extra» (Windows pedirá permiso) o "
           L"elige «Duplicar».";
  }
  if (has(L"no se pudo abrir el driver")) {
    return L"Windows no deja abrir el driver de la pantalla extra. Reinstala Extendesk con el instalador de la "
           L"última versión.";
  }
  if (has(L"no activó el monitor virtual")) {
    return L"Windows no activó la pantalla extra. En Configuración > Pantalla, elige «Extender estas pantallas».";
  }
  if (has(L"el driver no pudo crear el monitor")) {
    return L"El driver no pudo crear la pantalla extra. Prueba otra resolución en Configuración avanzada o "
           L"reinstala el driver.";
  }
  if (has(L"bind al puerto") || has(L"USB: no se pudo escuchar")) {
    return Format(L"El puerto %d ya está en uso. ¿Hay otro Extendesk abierto (por ejemplo desklink-host.exe en una "
                  L"consola)? Ciérralo y pulsa Iniciar.",
                  g.settings.port);
  }
  if (has(L"codificador")) {
    return L"La tarjeta gráfica no pudo iniciar el codificador de vídeo. Prueba el códec H.264 en Configuración "
           L"avanzada, o actualiza el driver de la GPU.";
  }
  if (has(L"No existe la salida")) return L"Ese monitor ya no está conectado. Elige otro en Configuración avanzada.";
  if (has(L"DuplicateOutput falló") || has(L"no se pudo recuperar la captura")) {
    return L"Windows no deja capturar la pantalla ahora mismo (pantalla bloqueada, aviso de UAC o un juego a "
           L"pantalla completa).";
  }
  return {};
}

// ---------------------------------------------------------------------------------------------------------------
// Claro u oscuro, como Windows.

bool SystemUsesDark() {
  DWORD light = 1, size = sizeof(light);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                   L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) != ERROR_SUCCESS) {
    return false;
  }
  return light == 0;
}

// Barra de título del color del fondo (Windows 11) y oscura o clara según el tema.
void StyleTitleBar(HWND hwnd) {
  const BOOL dark = g.dark;
  if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)))) DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
  const COLORREF caption = P().background;
  DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));  // DWMWA_CAPTION_COLOR
}

// Menús (los de la bandeja y los desplegables) claros u oscuros según Windows. uxtheme no lo documenta, pero es
// lo que usan el Explorador y el Bloc de notas; si no existe, los menús salen claros y ya está.
void FollowSystemMenus() {
  HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!ux) return;
  using SetPreferredAppMode = int(WINAPI*)(int);
  using FlushMenuThemes = void(WINAPI*)();
  auto set_mode = reinterpret_cast<SetPreferredAppMode>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
  auto flush = reinterpret_cast<FlushMenuThemes>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
  if (set_mode) set_mode(1);  // AllowDark: sigue al sistema
  if (flush) flush();
}

void ApplyTheme() {
  g.dark = SystemUsesDark();
  if (g.log_bg) DeleteObject(g.log_bg);
  g.log_bg = CreateSolidBrush(P().surface);
  if (g.wnd) StyleTitleBar(g.wnd);
  if (g.log_wnd) {
    StyleTitleBar(g.log_wnd);
    InvalidateRect(g.log_wnd, nullptr, TRUE);
  }
  Redraw();
}

// ---------------------------------------------------------------------------------------------------------------
// Bandeja del sistema.

int ColorIndex(State s) {
  switch (s) {
    case State::kStopped: return 0;
    case State::kConnected: return 2;
    case State::kError: return 3;
    default: return 1;
  }
}

ViewInput Input() {
  ViewInput in;
  in.state = g.state;
  in.settings = g.settings;
  in.stats = g.stats;
  in.error = g.error;
  in.advanced_open = g.advanced_open;
  in.repairing_driver = g.repairing_driver;
  in.driver_installed = g.driver_installed;
  in.dirty = g.dirty;
  in.autostart = g.autostart;
  in.monitors = g.monitors;
  in.addresses = g.addresses;
  return in;
}

void TrayCommand(DWORD message) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g.wnd;
  nid.uID = kTrayId;
  nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  nid.uCallbackMessage = WM_TRAY;
  nid.hIcon = g.tray_icons[ColorIndex(g.state)];
  wcsncpy_s(nid.szTip, (std::wstring(kAppName) + L": " + BuildView(Input()).title).c_str(), _TRUNCATE);
  Shell_NotifyIconW(message, &nid);
  if (message == NIM_ADD) {
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
  }
}

// Aviso junto al reloj; solo con la ventana oculta (con ella abierta ya se ve el estado).
void Notify(const std::wstring& title, const std::wstring& text, bool warning = false) {
  if (IsWindowVisible(g.wnd)) return;
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g.wnd;
  nid.uID = kTrayId;
  nid.uFlags = NIF_INFO;
  nid.dwInfoFlags = (warning ? NIIF_WARNING : NIIF_INFO) | NIIF_RESPECT_QUIET_TIME;
  wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
  wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ---------------------------------------------------------------------------------------------------------------
// Ventana de registro: la salida completa del host, para diagnosticar o copiar en un informe.

void AddLog(const std::wstring& line) {
  g.log.push_back(line);
  if (g.log.size() > 2000) g.log.pop_front();
  if (g.log_edit) {
    const std::wstring text = line + L"\r\n";
    const int len = GetWindowTextLengthW(g.log_edit);
    SendMessageW(g.log_edit, EM_SETSEL, len, len);
    SendMessageW(g.log_edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
  }
}

LRESULT CALLBACK LogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      StyleTitleBar(hwnd);
      g.log_edit = CreateWindowExW(0, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                   0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kIdLogEdit), g.inst, nullptr);
      CreateWindowExW(0, L"BUTTON", L"Copiar todo", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0,
                      hwnd, reinterpret_cast<HMENU>(kIdLogCopy), g.inst, nullptr);
      SendMessageW(g.log_edit, WM_SETFONT, reinterpret_cast<WPARAM>(g.mono_font), TRUE);
      SendMessageW(GetDlgItem(hwnd, kIdLogCopy), WM_SETFONT, reinterpret_cast<WPARAM>(g.ui_font), TRUE);
      SendMessageW(g.log_edit, EM_SETLIMITTEXT, 0, 0);
      std::wstring all;
      for (const auto& l : g.log) all += l + L"\r\n";
      SetWindowTextW(g.log_edit, all.c_str());
      SendMessageW(g.log_edit, EM_SETSEL, all.size(), all.size());
      SendMessageW(g.log_edit, EM_SCROLLCARET, 0, 0);
      return 0;
    }
    case WM_SIZE: {
      const int w = LOWORD(lp), h = HIWORD(lp);
      MoveWindow(g.log_edit, static_cast<int>(F(12)), static_cast<int>(F(12)), w - static_cast<int>(F(24)),
                 h - static_cast<int>(F(60)), TRUE);
      MoveWindow(GetDlgItem(hwnd, kIdLogCopy), w - static_cast<int>(F(132)), h - static_cast<int>(F(40)),
                 static_cast<int>(F(120)), static_cast<int>(F(30)), TRUE);
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wp) == kIdLogCopy) {
        SendMessageW(g.log_edit, EM_SETSEL, 0, -1);
        SendMessageW(g.log_edit, WM_COPY, 0, 0);
        const int len = GetWindowTextLengthW(g.log_edit);
        SendMessageW(g.log_edit, EM_SETSEL, len, len);
      }
      return 0;
    case WM_CTLCOLORSTATIC:  // edit de solo lectura con los colores de la app
    case WM_CTLCOLOREDIT: {
      HDC dc = reinterpret_cast<HDC>(wp);
      SetTextColor(dc, P().text);
      SetBkColor(dc, P().surface);
      return reinterpret_cast<LRESULT>(g.log_bg);
    }
    case WM_ERASEBKGND: {
      RECT r;
      GetClientRect(hwnd, &r);
      HBRUSH b = CreateSolidBrush(P().background);
      FillRect(reinterpret_cast<HDC>(wp), &r, b);
      DeleteObject(b);
      return 1;
    }
    case WM_DESTROY:
      g.log_wnd = nullptr;
      g.log_edit = nullptr;
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// La resolución de la pantalla extra se cambia donde la de cualquier monitor; el host sigue lo que se elija.
void OpenDisplaySettings() { ShellExecuteW(nullptr, L"open", L"ms-settings:display", nullptr, nullptr, SW_SHOWNORMAL); }

void ShowLog() {
  if (g.log_wnd) {
    ShowWindow(g.log_wnd, SW_RESTORE);
    SetForegroundWindow(g.log_wnd);
    return;
  }
  g.log_wnd = CreateWindowExW(0, kLogClass, L"Extendesk: registro", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              static_cast<int>(F(760)), static_cast<int>(F(440)), nullptr, nullptr, g.inst, nullptr);
  ShowWindow(g.log_wnd, SW_SHOW);
}

// ---------------------------------------------------------------------------------------------------------------
// Estado.

void UpdateAddresses() {
  const auto ips = LocalAddresses();
  std::wstring text;
  for (size_t i = 0; i < ips.size() && i < 2; ++i) text += (i ? L", " : L"") + ips[i];
  if (text != g.addresses) {
    g.addresses = text;
    Redraw();
  }
}

void PulseTimer() {
  const bool animate = g.state == State::kStarting || g.state == State::kWaiting || g.state == State::kRestarting ||
                       g.state == State::kStopping;
  if (animate && IsWindowVisible(g.wnd)) {
    SetTimer(g.wnd, kTimerPulse, 50, nullptr);
  } else {
    KillTimer(g.wnd, kTimerPulse);
  }
}

void SetState(State s) {
  g.state = s;
  TrayCommand(NIM_MODIFY);
  PulseTimer();
  Redraw();
}

// ---------------------------------------------------------------------------------------------------------------
// Arrancar y parar el host.

void StartHost() {
  KillTimer(g.wnd, kTimerRestart);
  g.error.clear();
  g.error_mapped = false;
  g.stats.clear();
  g.link = L"ninguno";
  g.dirty = false;
  g.driver_installed = VirtualDriverInstalled();
  if (g.settings.virtual_display && !g.driver_installed) {
    g.error = FriendlyError(L"driver del monitor virtual no está instalado");
    SetState(State::kError);
    return;
  }
  g.args = HostArguments(g.settings);
  std::wstring error;
  if (!g.host.Start(g.wnd, AppDirectory() + L"\\desklink-host.exe", g.args, &error)) {
    g.error = error;
    SetState(State::kError);
    return;
  }
  AddLog(L"--- desklink-host.exe " + g.args);
  g.started_at = GetTickCount64();
  SetState(State::kStarting);
}

void StopHost() {
  KillTimer(g.wnd, kTimerRestart);
  if (!g.host.running()) {
    SetState(State::kStopped);
    return;
  }
  g.host.RequestStop();
  SetTimer(g.wnd, kTimerStopKill, 8000, nullptr);  // si no termina por las buenas, se cierra a la fuerza
  SetState(State::kStopping);
}

void ToggleHost() {
  if (g.state == State::kStopping) return;
  if (Running()) {
    g.restart_after_stop = false;
    StopHost();
  } else {
    StartHost();
  }
}

void ApplyChanges() {
  if (!Running() || g.state == State::kStopping) return;
  g.restart_after_stop = true;
  StopHost();
}

// ---------------------------------------------------------------------------------------------------------------
// Instalar el driver de la pantalla extra desde la app (lo que hace el instalador, por si falló o se quitó).

void RepairDriver() {
  if (g.repairing_driver) return;
  const std::wstring dir = AppDirectory() + L"\\driver";
  const std::wstring exe = dir + L"\\desklink-driver.exe";
  if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    g.error = L"No encuentro el driver junto a Extendesk. Vuelve a instalar Extendesk con Extendesk-Setup.exe.";
    g.error_mapped = true;
    SetState(State::kError);
    return;
  }
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);
  const std::wstring cmd = std::wstring(sys) + L"\\cmd.exe";
  const std::wstring params =
      L"/c \"\"" + exe + L"\" instalar --sin-pausa > \"" + dir + L"\\reparar.log\" 2>&1\"";
  SHELLEXECUTEINFOW sei{sizeof(sei)};
  sei.fMask = SEE_MASK_NOCLOSEPROCESS;
  sei.hwnd = g.wnd;
  sei.lpVerb = L"runas";  // pide permiso de administrador (UAC)
  sei.lpFile = cmd.c_str();
  sei.lpParameters = params.c_str();
  sei.lpDirectory = dir.c_str();
  sei.nShow = SW_HIDE;
  if (!ShellExecuteExW(&sei) || !sei.hProcess) {
    g.error = GetLastError() == ERROR_CANCELLED
                  ? L"Para instalar la pantalla extra hace falta aceptar el permiso de Windows. Vuelve a pulsar el botón."
                  : L"No se pudo lanzar la instalación de la pantalla extra.";
    g.error_mapped = true;
    SetState(State::kError);
    return;
  }
  g.repairing_driver = true;
  AddLog(L"--- instalando el driver de la pantalla extra");
  Redraw();
  HANDLE proc = sei.hProcess;
  HWND wnd = g.wnd;
  std::thread([proc, wnd] {
    WaitForSingleObject(proc, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(proc, &code);
    CloseHandle(proc);
    PostMessageW(wnd, WM_DRIVER_DONE, 0, static_cast<LPARAM>(code));
  }).detach();
}

void OnDriverDone(DWORD code) {
  g.repairing_driver = false;
  // La salida de desklink-driver.exe va al registro de la app (Configuración avanzada > Ver registro).
  const std::wstring log_path = AppDirectory() + L"\\driver\\reparar.log";
  HANDLE f = CreateFileW(log_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    std::string bytes(16384, '\0');
    DWORD read = 0;
    ReadFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(f);
    bytes.resize(read);
    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring text(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), n);
    size_t start = 0;
    while (start < text.size()) {
      size_t end = text.find(L'\n', start);
      if (end == std::wstring::npos) end = text.size();
      std::wstring line = text.substr(start, end - start);
      if (!line.empty() && line.back() == L'\r') line.pop_back();
      if (!line.empty()) AddLog(line);
      start = end + 1;
    }
  }
  // Windows tarda un momento en arrancar el dispositivo nuevo.
  for (int i = 0; i < 20 && !(g.driver_installed = VirtualDriverInstalled()); ++i) Sleep(250);
  if (g.driver_installed) {
    AddLog(L"--- pantalla extra instalada");
    StartHost();
    return;
  }
  g.error = code == 0 ? L"El driver se instaló pero Windows no arrancó la pantalla extra. Reinicia el PC y pulsa Iniciar."
                      : Format(L"No se pudo instalar la pantalla extra (código %lu). El detalle está en Configuración "
                               L"avanzada > Ver registro. Mientras tanto puedes usar «Duplicar».",
                               code);
  g.error_mapped = true;
  SetState(State::kError);
}

// Valor de «clave=valor» dentro de una línea del host.
std::wstring Field(const std::wstring& line, const wchar_t* key) {
  const std::wstring k = std::wstring(L" ") + key + L"=";
  const size_t pos = line.find(k);
  if (pos == std::wstring::npos) return {};
  const size_t start = pos + k.size();
  const size_t end = line.find(L' ', start);
  return line.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
}

void OnHostLine(bool from_stderr, const std::wstring& line) {
  if (line.rfind(L"@listo", 0) == 0) {
    if (g.state == State::kStarting) SetState(State::kWaiting);
    return;
  }
  if (line.rfind(L"@ajuste ", 0) == 0) {
    // La tablet cambió algo desde su menú rápido (táctil o ratón): se recuerda aquí, que es donde manda el PC.
    const std::wstring touch = Field(line, L"tactil");
    const int mode = touch == L"solo-ver" ? 2 : touch == L"tactil" ? 0 : -1;
    if (mode >= 0 && mode != g.settings.touch) {
      g.settings.touch = mode;
      SaveSettings(g.settings);
      Redraw();
    }
    return;
  }
  if (line.rfind(L"@estado ", 0) == 0) {
    g.stats.clear();
    size_t pos = 8;
    while (pos < line.size()) {
      size_t end = line.find(L' ', pos);
      if (end == std::wstring::npos) end = line.size();
      const std::wstring kv = line.substr(pos, end - pos);
      const size_t eq = kv.find(L'=');
      if (eq != std::wstring::npos) g.stats[kv.substr(0, eq)] = kv.substr(eq + 1);
      pos = end + 1;
    }
    const std::wstring link = g.stats[L"enlace"];
    if (link != g.link && g.state != State::kStopping) {
      if (link == L"ninguno") {
        Notify(L"Dispositivo desconectado", L"Extendesk sigue esperando a que vuelva a conectarse.");
      } else {
        Notify(L"Dispositivo conectado", link == L"usb" ? L"Por cable USB." : L"Por Wi-Fi (" + g.stats[L"ip"] + L").");
      }
      g.link = link;
    }
    if (g.state == State::kStarting || g.state == State::kWaiting || g.state == State::kConnected) {
      const State next = link == L"ninguno" ? State::kWaiting : State::kConnected;
      if (next != g.state) {
        SetState(next);
        return;
      }
    }
    Redraw();
    return;
  }
  AddLog(line);
  const std::wstring friendly = FriendlyError(line);
  if (!friendly.empty()) {
    g.error = friendly;
    g.error_mapped = true;
  } else if (!g.error_mapped && line.rfind(L"Error: ", 0) == 0) {
    g.error = line.substr(7);
    if (!g.error.empty()) g.error[0] = towupper(g.error[0]);
  } else if (from_stderr && g.error.empty()) {
    g.error = line;
  } else {
    return;
  }
  // Un aviso durante la transmisión no es un fallo: solo pasa a «error» si el host termina.
  Redraw();
}

void OnHostExit(DWORD code) {
  KillTimer(g.wnd, kTimerStopKill);
  const ULONGLONG ran = GetTickCount64() - g.started_at;
  if (g.state == State::kStopping) {
    AddLog(L"--- Detenido.");
    if (g.restart_after_stop) {
      g.restart_after_stop = false;
      StartHost();
    } else {
      g.error.clear();
      SetState(State::kStopped);
    }
    return;
  }
  AddLog(Format(L"--- desklink-host.exe terminó (código %lu).", code));
  if (g.error.empty()) {
    g.error = Format(L"El host se cerró inesperadamente (código %lu). Abre «Ver registro» para el detalle.", code);
  }
  if (ran >= kStableRunMs) {
    // Funcionaba: un fallo pasajero (la GPU se reinició, cambió el modo de pantalla…). Se vuelve a intentar.
    Notify(L"Extendesk se reinicia", g.error, true);
    SetTimer(g.wnd, kTimerRestart, 3000, nullptr);
    SetState(State::kRestarting);
    return;
  }
  Notify(L"Extendesk no pudo transmitir", g.error, true);
  SetState(State::kError);
}

// Cambia los ajustes: se guardan y, con la transmisión en marcha, se aplican al momento si se puede (calidad,
// sonido, táctil, fps…: el host los recibe por su entrada) o se ofrece «Aplicar cambios». restart_now reinicia
// sin preguntar (el modo, que es lo primero que se ve).
void ChangeSettings(const Settings& next, bool restart_now = false) {
  const Settings before = g.settings;
  g.settings = next;
  SaveSettings(g.settings);
  if (Running() && g.state != State::kStopping) {
    if (NeedsRestart(before, next)) {
      if (restart_now) {
        ApplyChanges();
      } else {
        g.dirty = true;
      }
    } else if (g.host.running()) {
      g.host.Send(LiveSettingsLine(next));
    }
  }
  Redraw();
}

void SetMode(bool virtual_display) {
  if (g.settings.virtual_display == virtual_display) return;
  Settings s = g.settings;
  s.virtual_display = virtual_display;
  g.driver_installed = VirtualDriverInstalled();
  ChangeSettings(s, true);
}

// ---------------------------------------------------------------------------------------------------------------
// Dibujo: piezas sueltas, planas, sin brillos ni degradados.

// brand: Montserrat (tokens::k…Brand); con weight >= 800, la Black.
std::unique_ptr<gp::Font> MakeFont(float px, bool semibold, bool brand = false, int weight = 700) {
  std::unique_ptr<gp::Font> f;
  if (brand) {
    if (weight >= 800 && g.brand_black) {
      f = std::make_unique<gp::Font>(g.brand_black.get(), F(px), gp::FontStyleRegular, gp::UnitPixel);
    } else if (g.brand_bold) {
      f = std::make_unique<gp::Font>(g.brand_bold.get(), F(px), gp::FontStyleBold, gp::UnitPixel);
    }
    if (f && f->GetLastStatus() == gp::Ok) return f;
  }
  if (semibold && !g.semibold_family.empty()) {
    f = std::make_unique<gp::Font>(g.semibold_family.c_str(), F(px), gp::FontStyleRegular, gp::UnitPixel);
    if (f->GetLastStatus() == gp::Ok) return f;
  }
  return std::make_unique<gp::Font>(g.family.get(), F(px), semibold ? gp::FontStyleBold : gp::FontStyleRegular,
                                    gp::UnitPixel);
}

// Carga las Montserrat de desklink.rc. Sin ellas, MakeFont sigue con Segoe UI.
void LoadBrandFonts() {
  g.brand_fonts = std::make_unique<gp::PrivateFontCollection>();
  for (const wchar_t* name : {L"FONT_BLACK", L"FONT_BOLD"}) {
    HRSRC res = FindResourceW(g.inst, name, RT_RCDATA);
    HGLOBAL data = res ? LoadResource(g.inst, res) : nullptr;
    const void* bytes = data ? LockResource(data) : nullptr;
    if (bytes) g.brand_fonts->AddMemoryFont(bytes, static_cast<INT>(SizeofResource(g.inst, res)));
  }
  const INT count = g.brand_fonts->GetFamilyCount();
  if (count <= 0) return;
  std::vector<gp::FontFamily> families(count);
  INT found = 0;
  g.brand_fonts->GetFamilies(count, families.data(), &found);
  for (INT i = 0; i < found; ++i) {
    WCHAR name[LF_FACESIZE] = {};
    families[i].GetFamilyName(name);
    if (wcscmp(name, L"Montserrat Black") == 0) g.brand_black.reset(families[i].Clone());
    if (wcscmp(name, L"Montserrat") == 0) g.brand_bold.reset(families[i].Clone());
  }
}

// El icono (logo.h) con la esquina de arriba a la izquierda en (x, y) y este alto. Sin shadow, la sombra va del
// color de delante.
void DrawLogo(gp::Graphics& gr, float x, float y, float height, COLORREF back = tokens::kLogoBack,
              COLORREF front = tokens::kLogoFront, COLORREF shadow = tokens::kLogoFrontShadow) {
  const float s = height / logo::kHeight;
  auto build = [&](gp::GraphicsPath* path, const logo::Cmd* cmds, size_t n) {
    gp::PointF cur;
    for (size_t i = 0; i < n; ++i) {
      const logo::Cmd& c = cmds[i];
      auto pt = [&](int k) { return gp::PointF(x + c.p[2 * k] * s, y + c.p[2 * k + 1] * s); };
      switch (c.op) {
        case logo::kMove: path->StartFigure(); cur = pt(0); break;
        case logo::kLine: path->AddLine(cur, pt(0)); cur = pt(0); break;
        case logo::kCubic: path->AddBezier(cur, pt(0), pt(1), pt(2)); cur = pt(2); break;
        case logo::kClose: path->CloseFigure(); break;
      }
    }
  };
  gp::GraphicsPath back_path, front_path, shadow_path;  // relleno alterno: el segundo trazo de kFront es el hueco
  build(&back_path, logo::kBack, std::size(logo::kBack));
  build(&front_path, logo::kFront, std::size(logo::kFront));
  build(&shadow_path, logo::kShadow, std::size(logo::kShadow));
  gp::SolidBrush back_brush(C(back)), front_brush(C(front));
  gr.FillPath(&back_brush, &back_path);
  gr.FillPath(&front_brush, &front_path);
  gp::LinearGradientBrush shadow_brush(gp::PointF(0, y + logo::kShadowTop * s - 1),
                                       gp::PointF(0, y + logo::kShadowBottom * s + 1), C(shadow), C(front));
  gr.FillPath(&shadow_brush, &shadow_path);
}

// Icono cuadrado con el logo centrado (bandeja y, si falta el recurso, ventana).
HICON CreateLogoIcon(int size, COLORREF back, COLORREF front, COLORREF shadow) {
  gp::Bitmap bmp(size, size, PixelFormat32bppARGB);
  {
    gp::Graphics gr(&bmp);
    gr.Clear(gp::Color(0, 0, 0, 0));
    gr.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    gr.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
    const float w = static_cast<float>(size), h = w * logo::kHeight / logo::kWidth;
    DrawLogo(gr, 0, (size - h) / 2, h, back, front, shadow);
  }
  HICON icon = nullptr;
  bmp.GetHICON(&icon);
  return icon;
}

void RoundRect(gp::GraphicsPath* path, const gp::RectF& r, float radius) {
  const float d = std::min(radius * 2, std::min(r.Width, r.Height));
  path->AddArc(r.X, r.Y, d, d, 180, 90);
  path->AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
  path->AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
  path->AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
  path->CloseFigure();
}

void FillRound(gp::Graphics& gr, const gp::RectF& r, float radius, COLORREF c, BYTE alpha = 255) {
  gp::GraphicsPath path;
  RoundRect(&path, r, radius);
  gp::SolidBrush brush(C(c, alpha));
  gr.FillPath(&brush, &path);
}

void StrokeRound(gp::Graphics& gr, const gp::RectF& r, float radius, COLORREF c, float width = 1) {
  gp::GraphicsPath path;
  RoundRect(&path, r, radius);
  gp::Pen pen(C(c), F(width));
  gr.DrawPath(&pen, &path);
}

enum class Align { kLeft, kCenter, kRight };

void Text(gp::Graphics& gr, const std::wstring& s, const gp::RectF& r, const gp::Font& font, COLORREF c,
          Align align = Align::kLeft, bool wrap = false) {
  gp::StringFormat sf;
  sf.SetAlignment(align == Align::kLeft ? gp::StringAlignmentNear
                  : align == Align::kCenter ? gp::StringAlignmentCenter : gp::StringAlignmentFar);
  sf.SetLineAlignment(wrap ? gp::StringAlignmentNear : gp::StringAlignmentCenter);
  sf.SetTrimming(gp::StringTrimmingEllipsisCharacter);
  if (!wrap) sf.SetFormatFlags(gp::StringFormatFlagsNoWrap);
  gp::SolidBrush brush(C(c));
  gr.DrawString(s.c_str(), static_cast<INT>(s.size()), &font, r, &sf, &brush);
}

// Alto que ocupa un texto partido en líneas a este ancho.
float TextHeight(gp::Graphics& gr, const std::wstring& s, const gp::Font& font, float width) {
  if (s.empty()) return 0;
  gp::RectF box;
  gr.MeasureString(s.c_str(), static_cast<INT>(s.size()), &font, gp::RectF(0, 0, width, 10000), &box);
  return std::ceil(box.Height);
}

float TextWidth(gp::Graphics& gr, const std::wstring& s, const gp::Font& font) {
  gp::RectF box;
  gr.MeasureString(s.c_str(), static_cast<INT>(s.size()), &font, gp::PointF(0, 0), &box);
  return std::ceil(box.Width);
}

int AddHit(const gp::RectF& r, std::function<void()> action) {
  g.hits.push_back({r, std::move(action)});
  return static_cast<int>(g.hits.size()) - 1;
}

bool Hovered(int hit) { return hit >= 0 && hit == g.hover; }

void Chevron(gp::Graphics& gr, float cx, float cy, bool down, COLORREF c) {
  gp::Pen pen(C(c), F(1.6f));
  pen.SetLineJoin(gp::LineJoinRound);
  pen.SetStartCap(gp::LineCapRound);
  pen.SetEndCap(gp::LineCapRound);
  if (down) {
    const gp::PointF pts[3] = {{cx - F(4.5f), cy - F(2)}, {cx, cy + F(2.5f)}, {cx + F(4.5f), cy - F(2)}};
    gr.DrawLines(&pen, pts, 3);
  } else {
    const gp::PointF pts[3] = {{cx - F(2), cy - F(4.5f)}, {cx + F(2.5f), cy}, {cx - F(2), cy + F(4.5f)}};
    gr.DrawLines(&pen, pts, 3);
  }
}

// Control segmentado como el de macOS: un carril gris y la opción elegida en una pastilla más clara.
void Segmented(gp::Graphics& gr, const gp::RectF& r, const std::vector<std::wstring>& labels, int selected,
               const std::function<void(int)>& on_select) {
  FillRound(gr, r, F(tokens::kControlRadius), P().surface_secondary);
  auto font = MakeFont(tokens::kNotePx, false);
  auto bold = MakeFont(tokens::kNotePx, true);
  const float w = r.Width / labels.size();
  for (size_t i = 0; i < labels.size(); ++i) {
    const gp::RectF cell(r.X + w * i + F(2), r.Y + F(2), w - F(4), r.Height - F(4));
    const int hit = AddHit(cell, [on_select, i] { on_select(static_cast<int>(i)); });
    const bool chosen = static_cast<int>(i) == selected;
    if (chosen) {
      FillRound(gr, cell, F(tokens::kControlRadius - 2), g.dark ? P().separator : P().surface);
      if (!g.dark) StrokeRound(gr, cell, F(tokens::kControlRadius - 2), P().separator, 0.75f);
    } else if (Hovered(hit)) {
      FillRound(gr, cell, F(tokens::kControlRadius - 2), P().separator, 90);
    }
    Text(gr, labels[i], cell, chosen ? *bold : *font, P().text, Align::kCenter);
  }
}

// Interruptor: carril con el acento si está activo y botón blanco.
void Switch(gp::Graphics& gr, float x, float y, bool on, bool hover) {
  const gp::RectF track(x, y, F(38), F(22));
  FillRound(gr, track, F(11), on ? P().accent : (hover ? P().text_secondary : P().separator));
  const float knob = F(18);
  gp::SolidBrush knob_brush(C(RGB(0xFF, 0xFF, 0xFF)));
  gr.FillEllipse(&knob_brush, on ? track.X + track.Width - knob - F(2) : track.X + F(2), track.Y + F(2), knob, knob);
}

enum class ButtonKind { kPrimary, kStop, kSecondary, kDisabled };

void Button(gp::Graphics& gr, const gp::RectF& r, const std::wstring& label, ButtonKind kind,
            std::function<void()> action) {
  const int hit = kind == ButtonKind::kDisabled ? -1 : AddHit(r, std::move(action));
  const float radius = F(tokens::kControlRadius);
  auto font = MakeFont(tokens::kBodyPx, true);
  switch (kind) {
    case ButtonKind::kPrimary:
      FillRound(gr, r, radius, P().accent, Hovered(hit) ? 225 : 255);
      Text(gr, label, r, *font, P().on_accent, Align::kCenter);
      break;
    case ButtonKind::kStop:
      FillRound(gr, r, radius, Hovered(hit) ? P().separator : P().surface_secondary);
      Text(gr, label, r, *font, P().error, Align::kCenter);
      break;
    case ButtonKind::kSecondary:
      FillRound(gr, r, radius, Hovered(hit) ? P().separator : P().surface_secondary);
      Text(gr, label, r, *font, P().accent, Align::kCenter);
      break;
    case ButtonKind::kDisabled:
      FillRound(gr, r, radius, P().surface_secondary);
      Text(gr, label, r, *font, P().text_secondary, Align::kCenter);
      break;
  }
}

// Menú desplegable bajo un punto de la ventana; devuelve el índice elegido o -1.
int PickFromMenu(float x, float y, const std::vector<std::wstring>& items, int checked) {
  HMENU m = CreatePopupMenu();
  for (size_t i = 0; i < items.size(); ++i) {
    AppendMenuW(m, MF_STRING | (static_cast<int>(i) == checked ? MF_CHECKED : 0), kMenuPick + i, items[i].c_str());
  }
  POINT pt{static_cast<LONG>(x), static_cast<LONG>(y - g.scroll)};
  ClientToScreen(g.wnd, &pt);
  const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, pt.x, pt.y, 0,
                                 g.wnd, nullptr);
  DestroyMenu(m);
  return cmd >= kMenuPick ? cmd - kMenuPick : -1;
}

template <typename T, typename Label>
void PickValue(float x, float y, const std::vector<T>& values, const T& current, Label label,
               const std::function<void(const T&)>& apply) {
  std::vector<std::wstring> items;
  int checked = -1;
  for (size_t i = 0; i < values.size(); ++i) {
    items.push_back(label(values[i]));
    if (values[i] == current) checked = static_cast<int>(i);
  }
  const int pick = PickFromMenu(x, y, items, checked);
  if (pick >= 0 && values[pick] != current) apply(values[pick]);
}

// ---------------------------------------------------------------------------------------------------------------
// Qué hace cada fila al pulsarla.

void OnSegment(const std::string& id, int i) {
  Settings s = g.settings;
  if (id == "modo") {
    SetMode(i == 0);
    return;
  }
  if (id == "calidad") s.quality = QualityAt(i);
  else if (id == "sonido") s.audio_tablet = i == 1;
  else if (id == "tactil") s.touch = i == 1 ? 2 : 0;
  ChangeSettings(s);
}

void OnChoice(const std::string& id, float x, float y) {
  if (id == "monitor") {
    g.monitors = MonitorNames();
    const int pick = PickFromMenu(x, y, g.monitors, g.settings.output);
    if (pick >= 0 && pick != g.settings.output) {
      Settings s = g.settings;
      s.output = pick;
      ChangeSettings(s);
    }
  } else if (id == "fps") {
    PickValue<int>(x, y, FpsChoices(), g.settings.fps, FpsLabel, [](const int& v) {
      Settings s = g.settings;
      s.fps = v;
      ChangeSettings(s);
    });
  } else if (id == "mbps") {
    PickValue<int>(x, y, BitrateChoices(), g.settings.bitrate, BitrateLabel, [](const int& v) {
      Settings s = g.settings;
      s.bitrate = v;
      ChangeSettings(s);
    });
  } else if (id == "codec") {
    PickValue<std::wstring>(x, y, CodecChoices(), g.settings.codec, CodecLabel, [](const std::wstring& v) {
      Settings s = g.settings;
      s.codec = v;
      ChangeSettings(s);
    });
  }
}

void OnToggle(const std::string& id) {
  Settings s = g.settings;
  if (id == "autostart") {
    SetStartWithWindows(!g.autostart);
    g.autostart = StartsWithWindows();
    Redraw();
    return;
  }
  if (id == "bateria") s.battery_adapt = !s.battery_adapt;
  else if (id == "usb") s.usb = !s.usb;
  else if (id == "abrir") s.stream_on_open = !s.stream_on_open;
  else if (id == "abrir_tablet") s.open_tablet = !s.open_tablet;
  ChangeSettings(s);
}

// ---------------------------------------------------------------------------------------------------------------
// Dibujo: la ventana. Cada tarjeta se mide primero (draw = false) y luego se pinta encima de su fondo.

void Card(gp::Graphics& gr, float x, float* y, float w, const std::function<float(bool draw, float top)>& content) {
  const float h = content(false, *y);
  const gp::RectF r(x, *y, w, h);
  FillRound(gr, r, F(tokens::kCardRadius), P().surface);
  if (!g.dark) StrokeRound(gr, r, F(tokens::kCardRadius), P().separator, 0.75f);
  content(true, *y);
  *y += h;
}

float StatusContent(gp::Graphics& gr, const View& v, bool draw, float x, float top, float w) {
  const float ix = x + F(kPad), iw = w - F(2 * kPad);
  float y = top + F(kPad);
  auto title = MakeFont(tokens::kTitlePx, true, tokens::kTitleBrand, tokens::kTitleWeight);
  auto body = MakeFont(tokens::kBodyPx, false);
  auto note = MakeFont(tokens::kNotePx, false);

  if (draw) {
    // Punto de estado: mientras espera, late suave.
    const float dot = F(10);
    const gp::RectF dot_rect(ix, y + F(8), dot, dot);
    g.pulse_rect = gp::RectF(dot_rect.X - F(2), dot_rect.Y - F(2), dot + F(4), dot + F(4));
    const bool waiting = g.state == State::kStarting || g.state == State::kWaiting ||
                         g.state == State::kRestarting || g.state == State::kStopping;
    const BYTE alpha = waiting ? static_cast<BYTE>(140 + 115 * (0.5f + 0.5f * std::sin(g.pulse))) : 255;
    gp::SolidBrush dot_brush(C(ToneColor(v.dot), alpha));
    gr.FillEllipse(&dot_brush, dot_rect);
    Text(gr, v.title, gp::RectF(ix + F(20), y, iw - F(20), F(26)), *title, P().text);
  }
  y += F(30);

  const float dh = TextHeight(gr, v.detail, *body, iw);
  if (draw) Text(gr, v.detail, gp::RectF(ix, y, iw, dh + F(2)), *body, P().text_secondary, Align::kLeft, true);
  y += dh + F(4);

  if (!v.stats.empty()) {
    y += F(4);
    if (draw) Text(gr, v.stats, gp::RectF(ix, y, iw, F(22)), *body, P().text);
    y += F(24);
  }
  if (!v.fluency.empty()) {
    if (draw) Text(gr, v.fluency, gp::RectF(ix, y, iw, F(20)), *note, P().text_secondary);
    y += F(22);
  }
  if (!v.notice.text.empty()) {
    const float nh = TextHeight(gr, v.notice.text, *note, iw);
    if (draw) Text(gr, v.notice.text, gp::RectF(ix, y, iw, nh + F(2)), *note, ToneColor(v.notice.tone), Align::kLeft, true);
    y += nh + F(4);
  }
  if (!v.error.empty()) {
    y += F(6);
    const float eh = TextHeight(gr, v.error, *note, iw - F(24));
    const gp::RectF box(ix, y, iw, eh + F(20));
    if (draw) {
      FillRound(gr, box, F(tokens::kControlRadius), P().error, 26);
      Text(gr, v.error, gp::RectF(box.X + F(12), box.Y + F(10), box.Width - F(24), eh + F(2)), *note, P().error,
           Align::kLeft, true);
    }
    y += box.Height + F(4);
  }
  if (!v.connection.empty()) {
    y += F(8);
    if (draw) {
      gp::Pen sep(C(P().separator), F(1));
      gr.DrawLine(&sep, ix, y, ix + iw, y);
    }
    y += F(10);
    for (const Line& line : v.connection) {
      const float lh = TextHeight(gr, line.text, *note, iw);
      if (draw) Text(gr, line.text, gp::RectF(ix, y, iw, lh + F(2)), *note, ToneColor(line.tone), Align::kLeft, true);
      y += lh + F(4);
    }
  }
  return y + F(kPad) - F(4) - top;
}

// Filas de una tarjeta de ajustes, separadas por una línea fina.
float RowsContent(gp::Graphics& gr, const std::vector<Row>& rows, bool draw, float x, float top, float w) {
  const float ix = x + F(kPad), iw = w - F(2 * kPad);
  auto body = MakeFont(tokens::kBodyPx, false);
  auto note = MakeFont(tokens::kNotePx, false);
  float y = top;
  for (size_t i = 0; i < rows.size(); ++i) {
    const Row& r = rows[i];
    if (i > 0 && draw) {
      gp::Pen sep(C(P().separator), F(1));
      gr.DrawLine(&sep, ix, y, ix + iw, y);
    }
    const float row_top = y;
    y += F(10);
    switch (r.control) {
      case Control::kSegmented: {
        const float seg_w = std::min(iw * 0.66f, F(118) * r.options.size());
        const float label_w = iw - seg_w - F(12);
        const float lh = std::max(F(30), TextHeight(gr, r.label, *body, label_w));
        if (draw) {
          Text(gr, r.label, gp::RectF(ix, y, label_w, lh), *body, P().text);
          const std::string id = r.id;
          Segmented(gr, gp::RectF(ix + iw - seg_w, y, seg_w, F(30)), r.options, r.selected,
                    [id](int k) { OnSegment(id, k); });
        }
        y += lh;
        if (!r.hint.empty()) {
          y += F(4);
          const float hh = TextHeight(gr, r.hint, *note, iw);
          if (draw) Text(gr, r.hint, gp::RectF(ix, y, iw, hh + F(2)), *note, ToneColor(r.hint_tone), Align::kRight, true);
          y += hh;
        }
        break;
      }
      case Control::kChoice: {
        const gp::RectF row(x, row_top, w, F(52));
        if (draw) {
          const std::string id = r.id;
          const float menu_x = ix + iw, menu_y = row_top + F(46);
          const int hit = AddHit(row, [id, menu_x, menu_y] { OnChoice(id, menu_x, menu_y); });
          if (Hovered(hit)) FillRound(gr, gp::RectF(x + F(4), row_top + F(3), w - F(8), F(46)), F(6), P().surface_secondary);
          const float value_w = std::min(iw * 0.55f, TextWidth(gr, r.value, *body) + F(4));
          Text(gr, r.label, gp::RectF(ix, y, iw - value_w - F(28), F(32)), *body, P().text);
          Text(gr, r.value, gp::RectF(ix + iw - value_w - F(18), y, value_w, F(32)), *body, P().text_secondary,
               Align::kRight);
          Chevron(gr, ix + iw - F(5), y + F(16), true, P().text_secondary);
        }
        y += F(32);
        break;
      }
      case Control::kToggle: {
        const float lh = std::max(F(32), TextHeight(gr, r.label, *body, iw - F(54)));
        if (draw) {
          const std::string id = r.id;
          const int hit = AddHit(gp::RectF(x, row_top, w, lh + F(20)), [id] { OnToggle(id); });
          Text(gr, r.label, gp::RectF(ix, y, iw - F(54), lh), *body, P().text, Align::kLeft, lh > F(32));
          Switch(gr, ix + iw - F(38), y + (lh - F(22)) / 2, r.on, Hovered(hit));
        }
        y += lh;
        break;
      }
      case Control::kInfo: {
        if (draw) {
          Text(gr, r.label, gp::RectF(ix, y, iw * 0.4f, F(32)), *body, P().text);
          Text(gr, r.value, gp::RectF(ix + iw * 0.4f, y, iw * 0.6f, F(32)), *note, P().text_secondary, Align::kRight);
        }
        y += F(32);
        break;
      }
      case Control::kLink: {
        if (draw) {
          const int hit = AddHit(gp::RectF(x, row_top, w, F(52)),
                                 r.id == "resolucion" ? std::function<void()>(OpenDisplaySettings)
                                                      : std::function<void()>(ShowLog));
          Text(gr, r.label, gp::RectF(ix, y, iw, F(32)), *body, Hovered(hit) ? P().text : P().accent);
          Chevron(gr, ix + iw - F(5), y + F(16), false, P().text_secondary);
        }
        y += F(32);
        if (!r.hint.empty()) {
          const float hh = TextHeight(gr, r.hint, *note, iw);
          if (draw) Text(gr, r.hint, gp::RectF(ix, y, iw, hh + F(2)), *note, P().text_secondary, Align::kLeft, true);
          y += hh;
        }
        break;
      }
    }
    y += F(10);
  }
  return y - top;
}

void Paint(gp::Graphics& gr, float width) {
  g.hits.clear();
  gr.SetSmoothingMode(gp::SmoothingModeAntiAlias);
  gr.SetTextRenderingHint(gp::TextRenderingHintClearTypeGridFit);
  gr.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
  gp::SolidBrush bg(C(P().background));
  gr.FillRectangle(&bg, 0.0f, 0.0f, width, 10000.0f);
  gr.TranslateTransform(0.0f, -g.scroll);

  const View v = BuildView(Input());
  const float x = F(kMargin), w = width - F(2 * kMargin);
  float y = F(20);

  auto large = MakeFont(tokens::kLargeTitlePx, true, tokens::kLargeTitleBrand, tokens::kLargeTitleWeight);
  auto note = MakeFont(tokens::kNotePx, false);
  auto section = MakeFont(tokens::kSectionPx, true, tokens::kSectionBrand, tokens::kSectionWeight);
  // El logo: los dos eslabones y «Extendesk» en Montserrat Black, del color del acento.
  const float logo_h = F(26);
  DrawLogo(gr, x, y + (F(36) - logo_h) / 2, logo_h);
  const float logo_w = logo_h * logo::kWidth / logo::kHeight + F(10);
  Text(gr, L"Extendesk", gp::RectF(x + logo_w, y, w - logo_w, F(36)), *large, P().accent);
  y += F(36);
  Text(gr, L"Tu dispositivo como segunda pantalla", gp::RectF(x, y, w, F(18)), *note, P().text_secondary);
  y += F(30);

  // Estado y botón principal.
  Card(gr, x, &y, w, [&](bool draw, float top) { return StatusContent(gr, v, draw, x, top, w); });
  y += F(12);
  Button(gr, gp::RectF(x, y, w, F(40)), v.primary,
         !v.primary_enabled ? ButtonKind::kDisabled : v.primary_stop ? ButtonKind::kStop : ButtonKind::kPrimary,
         ToggleHost);
  y += F(40);
  if (!v.repair.empty()) {
    y += F(8);
    Button(gr, gp::RectF(x, y, w, F(36)), v.repair,
           g.repairing_driver ? ButtonKind::kDisabled : ButtonKind::kSecondary, RepairDriver);
    y += F(36);
  }
  if (v.show_apply) {
    y += F(8);
    Button(gr, gp::RectF(x, y, w, F(36)), L"Aplicar cambios (reinicia la transmisión)", ButtonKind::kSecondary,
           ApplyChanges);
    y += F(36);
  }
  y += F(24);

  // Ajustes a la vista.
  Card(gr, x, &y, w, [&](bool draw, float top) { return RowsContent(gr, v.main, draw, x, top, w); });
  y += F(16);

  // Configuración avanzada, plegada.
  g.advanced_top = y;
  {
    const gp::RectF head(x, y, w, F(32));
    const int hit = AddHit(head, [] {
      g.advanced_open = !g.advanced_open;
      g.reveal_advanced = g.advanced_open;
      if (g.advanced_open) {
        g.monitors = MonitorNames();
        g.autostart = StartsWithWindows();
      }
      Redraw();
    });
    const COLORREF c = Hovered(hit) ? P().text : P().accent;
    Text(gr, L"Configuración avanzada", gp::RectF(x + F(4), y, w - F(30), F(32)), *section, c);
    Chevron(gr, x + F(4) + TextWidth(gr, L"Configuración avanzada", *section) + F(8), y + F(16), v.advanced_open, c);
    y += F(36);
  }
  g.base_bottom = y + F(16);
  if (v.advanced_open) {
    Card(gr, x, &y, w, [&](bool draw, float top) { return RowsContent(gr, v.advanced, draw, x, top, w); });
    y += F(8);
  }
  g.content_bottom = y + F(16);
}

// Alto máximo del área de la ventana: el del escritorio de su pantalla menos la barra de título.
int MaxClientHeight(HWND hwnd) {
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
  RECT frame{0, 0, 100, 0};
  AdjustWindowRectEx(&frame, GetWindowLongW(hwnd, GWL_STYLE), FALSE, GetWindowLongW(hwnd, GWL_EXSTYLE));
  return (mi.rcWork.bottom - mi.rcWork.top) - (frame.bottom - frame.top);
}

// Si al crecer la ventana se sale por abajo, la sube.
void KeepOnScreen(HWND hwnd) {
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
  RECT wr;
  GetWindowRect(hwnd, &wr);
  if (wr.bottom <= mi.rcWork.bottom) return;
  const int top = std::max<int>(mi.rcWork.top, wr.top - (wr.bottom - mi.rcWork.bottom));
  SetWindowPos(hwnd, nullptr, wr.left, top, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

void ClampScroll(float client_h) {
  g.scroll = std::clamp(g.scroll, 0.0f, std::max(0.0f, g.content_bottom - client_h));
}

// Barra fina a la derecha cuando el contenido no cabe: indica que hay más y dónde se está.
void ScrollIndicator(gp::Graphics& gr, float w, float h) {
  if (g.content_bottom <= h + 1) return;
  const float track = h - F(16);
  const float thumb = std::max(F(32), track * h / g.content_bottom);
  const float top = F(8) + (track - thumb) * g.scroll / (g.content_bottom - h);
  FillRound(gr, gp::RectF(w - F(8), top, F(4), thumb), F(2), P().text_secondary, 140);
}

void OnPaint(HWND hwnd) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(hwnd, &ps);
  RECT rc;
  GetClientRect(hwnd, &rc);
  const int w = rc.right, h = rc.bottom;
  // Doble búfer: se dibuja en memoria y se copia de una vez (sin parpadeo).
  HDC mem = CreateCompatibleDC(dc);
  HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
  HGDIOBJ old = SelectObject(mem, bmp);
  ClampScroll(static_cast<float>(h));
  {
    gp::Graphics gr(mem);
    Paint(gr, static_cast<float>(w));
    gr.ResetTransform();
    ScrollIndicator(gr, static_cast<float>(w), static_cast<float>(h));
  }
  BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
         mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  EndPaint(hwnd, &ps);

  // La ventana toma el alto del contenido sin lo avanzado, sin pasar del alto de la pantalla: al abrir «Configuración
  // avanzada» no crece ni se mueve (antes saltaba hacia arriba); se desplaza con la rueda hasta ella.
  const int want = std::min(static_cast<int>(std::ceil(g.base_bottom)), MaxClientHeight(hwnd));
  if (want > 0 && std::abs(want - h) > 1) {
    RECT wr{0, 0, w, want};
    AdjustWindowRectEx(&wr, GetWindowLongW(hwnd, GWL_STYLE), FALSE, GetWindowLongW(hwnd, GWL_EXSTYLE));
    SetWindowPos(hwnd, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER);
    KeepOnScreen(hwnd);
  }
  // Al abrir «Configuración avanzada» se desplaza para que se vea.
  if (g.reveal_advanced) {
    g.reveal_advanced = false;
    const float before = g.scroll;
    g.scroll = std::max(g.scroll, g.advanced_top - F(12));
    ClampScroll(static_cast<float>(want > 0 ? want : h));
    if (g.scroll != before) Redraw();
  }
}

void ScrollBy(float dy) {
  RECT rc;
  GetClientRect(g.wnd, &rc);
  const float before = g.scroll;
  g.scroll += dy;
  ClampScroll(static_cast<float>(rc.bottom));
  if (g.scroll != before) {
    g.hover = -1;
    Redraw();
  }
}

int HitAt(int x, int y) {
  for (size_t i = 0; i < g.hits.size(); ++i) {
    if (g.hits[i].rect.Contains(static_cast<float>(x), static_cast<float>(y) + g.scroll)) return static_cast<int>(i);
  }
  return -1;
}

// ---------------------------------------------------------------------------------------------------------------
// Ventana principal.

void CenterOnWorkArea(HWND hwnd) {
  RECT wr, work;
  GetWindowRect(hwnd, &wr);
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int w = wr.right - wr.left, h = wr.bottom - wr.top;
  SetWindowPos(hwnd, nullptr, work.left + (work.right - work.left - w) / 2,
               std::max<int>(work.top, work.top + (work.bottom - work.top - h) / 2), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

void ShowMain() {
  g.monitors = MonitorNames();
  g.driver_installed = VirtualDriverInstalled();
  g.autostart = StartsWithWindows();
  UpdateAddresses();
  ShowWindow(g.wnd, IsIconic(g.wnd) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(g.wnd);
  PulseTimer();
  Redraw();
}

void ExitApp() {
  if (g.host.running()) {
    SetState(State::kStopping);
    g.host.StopAndWait(8000);
  }
  if (g.log_wnd) DestroyWindow(g.log_wnd);
  DestroyWindow(g.wnd);
}

void ShowTrayMenu() {
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, kMenuOpen, L"Abrir Extendesk");
  SetMenuDefaultItem(m, kMenuOpen, FALSE);
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | (g.state == State::kStopping ? MF_GRAYED : 0), kMenuToggle,
              Running() ? L"Detener la transmisión" : L"Iniciar la transmisión");
  AppendMenuW(m, MF_STRING, kMenuVirtual, L"Pantalla extra");
  AppendMenuW(m, MF_STRING, kMenuMirror, L"Duplicar un monitor");
  CheckMenuRadioItem(m, kMenuVirtual, kMenuMirror, g.settings.virtual_display ? kMenuVirtual : kMenuMirror,
                     MF_BYCOMMAND);
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | (StartsWithWindows() ? MF_CHECKED : 0), kMenuAutostart, L"Abrir al iniciar Windows");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, kMenuExit, L"Salir");
  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(g.wnd);  // si no, el menú no se cierra al hacer clic fuera
  TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g.wnd, nullptr);
  PostMessageW(g.wnd, WM_NULL, 0, 0);
  DestroyMenu(m);
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == g.taskbar_created && msg != 0) {  // el Explorador se reinició: hay que volver a poner el icono
    TrayCommand(NIM_ADD);
    return 0;
  }
  switch (msg) {
    case WM_PAINT:
      OnPaint(hwnd);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_MOUSEMOVE: {
      if (!g.mouse_tracked) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        g.mouse_tracked = true;
      }
      const int hit = HitAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      if (hit != g.hover) {
        g.hover = hit;
        Redraw();
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      g.mouse_tracked = false;
      if (g.hover != -1) {
        g.hover = -1;
        Redraw();
      }
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT && g.hover >= 0) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    case WM_LBUTTONUP: {
      const int hit = HitAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      if (hit >= 0 && g.hits[hit].action) {
        auto action = g.hits[hit].action;  // la acción puede repintar y rehacer la lista
        action();
      }
      return 0;
    }
    case WM_DRIVER_DONE:
      OnDriverDone(static_cast<DWORD>(lp));
      return 0;
    case WM_MOUSEWHEEL:
      ScrollBy(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * F(60));
      return 0;
    case WM_KEYDOWN:
      if (wp == VK_SPACE || wp == VK_RETURN) ToggleHost();
      else if (wp == VK_NEXT || wp == VK_DOWN) ScrollBy(F(wp == VK_NEXT ? 300 : 60));
      else if (wp == VK_PRIOR || wp == VK_UP) ScrollBy(-F(wp == VK_PRIOR ? 300 : 60));
      return 0;
    case WM_TRAY:
      switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK:
          ShowMain();
          break;
        case WM_CONTEXTMENU:
          ShowTrayMenu();
          break;
      }
      return 0;
    case WM_APP_SHOW:
      ShowMain();
      return 0;
    case WM_HOST_LINE: {
      auto* text = reinterpret_cast<std::wstring*>(lp);
      if (wp / 2 == g.host.generation()) OnHostLine(wp & 1, *text);
      delete text;
      return 0;
    }
    case WM_HOST_EXIT:
      if (wp == g.host.generation()) OnHostExit(static_cast<DWORD>(lp));
      return 0;
    case WM_TIMER:
      if (wp == kTimerRestart) {
        KillTimer(hwnd, kTimerRestart);
        if (g.state == State::kRestarting) StartHost();
      } else if (wp == kTimerStopKill) {
        KillTimer(hwnd, kTimerStopKill);
        g.host.StopAndWait(0);  // WM_HOST_EXIT llega igual
      } else if (wp == kTimerRefresh && IsWindowVisible(hwnd)) {
        UpdateAddresses();
      } else if (wp == kTimerPulse) {
        g.pulse += 0.2f;
        RECT r{static_cast<LONG>(g.pulse_rect.X), static_cast<LONG>(g.pulse_rect.Y - g.scroll),
               static_cast<LONG>(g.pulse_rect.X + g.pulse_rect.Width + 1),
               static_cast<LONG>(g.pulse_rect.Y - g.scroll + g.pulse_rect.Height + 1)};
        InvalidateRect(hwnd, &r, FALSE);
      }
      return 0;
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case kMenuOpen: ShowMain(); return 0;
        case kMenuToggle: ToggleHost(); return 0;
        case kMenuVirtual: SetMode(true); return 0;
        case kMenuMirror: SetMode(false); return 0;
        case kMenuAutostart:
          SetStartWithWindows(!StartsWithWindows());
          g.autostart = StartsWithWindows();
          Redraw();
          return 0;
        case kMenuExit: ExitApp(); return 0;
      }
      return 0;
    case WM_SETTINGCHANGE:
      // Windows cambió entre claro y oscuro.
      if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
        FollowSystemMenus();
        ApplyTheme();
      }
      return 0;
    case WM_DISPLAYCHANGE:
      g.monitors = MonitorNames();
      Redraw();
      return 0;
    case WM_SHOWWINDOW:
      if (!wp) KillTimer(hwnd, kTimerPulse);
      break;
    case WM_CLOSE:
      ShowWindow(hwnd, SW_HIDE);
      if (!g.tray_hint_shown) {
        g.tray_hint_shown = true;
        Notify(L"Extendesk sigue abierto", L"Está junto al reloj. Para cerrarlo: clic derecho en el icono > Salir.");
      }
      return 0;
    case WM_QUERYENDSESSION:
      return TRUE;
    case WM_ENDSESSION:
      if (wp) g.host.StopAndWait(3000);  // Windows se apaga: quitar el monitor virtual y el túnel de adb
      return 0;
    case WM_DESTROY:
      TrayCommand(NIM_DELETE);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd_line, int) {
  // Una sola copia: si ya está abierta, se muestra esa.
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\DeskLinkApp");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND other = FindWindowW(kMainClass, nullptr)) PostMessageW(other, WM_APP_SHOW, 0, 0);
    return 0;
  }
  const bool tray_only = wcsstr(cmd_line, L"--tray") != nullptr;

  SetProcessDPIAware();
  FollowSystemMenus();
  g.inst = inst;
  HDC dc = GetDC(nullptr);
  g.dpi = GetDeviceCaps(dc, LOGPIXELSY);
  ReleaseDC(nullptr, dc);
  g.scale = g.dpi / 96.0f;
  ApplyTheme();

  gp::GdiplusStartupInput gdiplus_input;
  gp::GdiplusStartup(&g.gdiplus_token, &gdiplus_input, nullptr);
  g.family = std::make_unique<gp::FontFamily>(L"Segoe UI");
  if (g.family->GetLastStatus() != gp::Ok) g.family.reset(gp::FontFamily::GenericSansSerif()->Clone());
  {
    gp::FontFamily semibold(L"Segoe UI Semibold");
    if (semibold.GetLastStatus() == gp::Ok) g.semibold_family = L"Segoe UI Semibold";
  }

  NONCLIENTMETRICSW ncm{};
  ncm.cbSize = sizeof(ncm);
  SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
  g.ui_font = CreateFontIndirectW(&ncm.lfMessageFont);
  g.mono_font = CreateFontW(-MulDiv(9, g.dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");

  LoadBrandFonts();

  // Iconos de la bandeja: los eslabones en el color del estado (los del tema oscuro se ven en las dos barras); el
  // de atrás, algo más oscuro para que se distingan.
  const COLORREF tray_colors[4] = {tokens::kDark.text_secondary, tokens::kDark.warning, tokens::kDark.success,
                                   tokens::kDark.error};
  for (int i = 0; i < 4; ++i) {
    const COLORREF c = tray_colors[i];
    const COLORREF back = RGB(GetRValue(c) * 3 / 4, GetGValue(c) * 3 / 4, GetBValue(c) * 3 / 4);
    g.tray_icons[i] = CreateLogoIcon(GetSystemMetrics(SM_CXSMICON), back, c, back);
  }
  g.app_icon = LoadIconW(inst, L"APP");
  if (!g.app_icon) {
    g.app_icon = CreateLogoIcon(GetSystemMetrics(SM_CXICON), tokens::kLogoBack, tokens::kLogoFront,
                                tokens::kLogoFrontShadow);
  }
  g.app_icon_small = static_cast<HICON>(LoadImageW(inst, L"APP", IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                   GetSystemMetrics(SM_CYSMICON), 0));
  if (!g.app_icon_small) {
    g.app_icon_small = CreateLogoIcon(GetSystemMetrics(SM_CXSMICON), tokens::kLogoBack, tokens::kLogoFront,
                                      tokens::kLogoFrontShadow);
  }

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = MainProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;  // se pinta todo en WM_PAINT
  wc.hIcon = g.app_icon;
  wc.hIconSm = g.app_icon_small;
  wc.lpszClassName = kMainClass;
  RegisterClassExW(&wc);
  wc.lpfnWndProc = LogProc;
  wc.lpszClassName = kLogClass;
  RegisterClassExW(&wc);
  g.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

  if (!LoadSettings(&g.settings)) {
    // Primera vez: pantalla extra si el driver está instalado; si no, duplicar el monitor principal.
    g.settings.virtual_display = VirtualDriverInstalled();
    SaveSettings(g.settings);
  }
  g.driver_installed = VirtualDriverInstalled();
  g.autostart = StartsWithWindows();
  g.monitors = MonitorNames();

  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT r{0, 0, static_cast<LONG>(F(kClientW)), static_cast<LONG>(F(640))};
  AdjustWindowRectEx(&r, style, FALSE, 0);
  g.wnd = CreateWindowExW(0, kMainClass, kAppName, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                          r.bottom - r.top, nullptr, nullptr, inst, nullptr);
  if (!g.wnd) return 1;
  StyleTitleBar(g.wnd);

  // Primera pasada de dibujo fuera de pantalla para saber el alto del contenido y centrar la ventana ya con él.
  {
    HDC screen = GetDC(g.wnd);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, static_cast<int>(F(kClientW)), static_cast<int>(F(900)));
    HGDIOBJ old = SelectObject(mem, bmp);
    {
      gp::Graphics gr(mem);
      Paint(gr, F(kClientW));
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(g.wnd, screen);
    RECT wr{0, 0, static_cast<LONG>(F(kClientW)),
            static_cast<LONG>(std::min(static_cast<int>(std::ceil(g.base_bottom)), MaxClientHeight(g.wnd)))};
    AdjustWindowRectEx(&wr, style, FALSE, 0);
    SetWindowPos(g.wnd, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER);
    CenterOnWorkArea(g.wnd);
  }

  UpdateAddresses();
  TrayCommand(NIM_ADD);
  SetTimer(g.wnd, kTimerRefresh, 5000, nullptr);
  if (!tray_only) ShowMain();
  if (g.settings.stream_on_open) StartHost();

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  g.family.reset();
  g.brand_black.reset();
  g.brand_bold.reset();
  g.brand_fonts.reset();
  gp::GdiplusShutdown(g.gdiplus_token);
  ReleaseMutex(mutex);
  return 0;
}
