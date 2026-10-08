// DeskLink: app de bandeja para Windows. Arranca desklink-host.exe sin consola con los ajustes guardados, muestra
// el estado de la conexión (cable USB o WiFi, fps, Mbps, RTT) y explica los errores con palabras normales.
// La captura y la codificación siguen en desklink-host.exe, que también se puede usar solo desde una consola.
//
// La ventana se dibuja entera con GDI+ (sin controles estándar) con el estilo oscuro de docs/design.md.
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
#include <vector>

#include "host_process.h"
#include "settings.h"
#include "system_info.h"

namespace gp = Gdiplus;

namespace {

constexpr wchar_t kMainClass[] = L"DeskLinkMain";
constexpr wchar_t kLogClass[] = L"DeskLinkLog";
constexpr wchar_t kAppName[] = L"DeskLink";
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_APP_SHOW = WM_APP + 4;  // otra copia de la app pide mostrar esta ventana
constexpr UINT kTrayId = 1;
constexpr UINT_PTR kTimerRestart = 1;
constexpr UINT_PTR kTimerRefresh = 2;
constexpr UINT_PTR kTimerStopKill = 3;
constexpr UINT_PTR kTimerPulse = 4;
constexpr ULONGLONG kStableRunMs = 15000;  // si el host cae después de funcionar este tiempo, se reinicia solo

enum MenuId { kMenuOpen = 200, kMenuToggle, kMenuVirtual, kMenuMirror, kMenuAutostart, kMenuExit, kMenuPick = 300 };
enum ChildId { kIdLogEdit = 100, kIdLogCopy };

enum class State { kStopped, kStarting, kWaiting, kConnected, kStopping, kRestarting, kError };

// Colores de docs/design.md.
namespace color {
constexpr COLORREF kBackground = RGB(0x0A, 0x0E, 0x17);
constexpr COLORREF kSurface = RGB(0x12, 0x18, 0x26);
constexpr COLORREF kSurfaceRaised = RGB(0x1A, 0x22, 0x33);
constexpr COLORREF kOutline = RGB(0x26, 0x30, 0x49);
constexpr COLORREF kAccent = RGB(0x00, 0xE5, 0xFF);
constexpr COLORREF kAccentAlt = RGB(0xFF, 0x2B, 0xD6);
constexpr COLORREF kSuccess = RGB(0x3D, 0xFF, 0x9A);
constexpr COLORREF kWarning = RGB(0xFF, 0xB5, 0x47);
constexpr COLORREF kError = RGB(0xFF, 0x4D, 0x6D);
constexpr COLORREF kText = RGB(0xE8, 0xEE, 0xFF);
constexpr COLORREF kTextMuted = RGB(0x8A, 0x96, 0xB8);
constexpr COLORREF kOnAccent = RGB(0x05, 0x07, 0x0C);
}  // namespace color

gp::Color C(COLORREF c, BYTE alpha = 255) { return gp::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c)); }

// Resoluciones que ofrece el desplegable del monitor virtual.
const wchar_t* const kSizePresets[] = {L"2944x1840", L"2560x1600", L"2000x1200", L"1920x1200", L"1920x1080",
                                       L"1280x800"};
const int kFpsChoices[] = {30, 60, 90, 120, 144};
const int kQualityMbps[] = {20, 50, 100};
const wchar_t* const kQualityNames[] = {L"Ahorro", L"Equilibrada", L"Máxima"};

constexpr int kClientW = 760;  // tamaño del área cliente a 96 ppp; el alto sale del contenido

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
  HFONT mono_font = nullptr, ui_font = nullptr;
  HBRUSH log_bg = nullptr;
  HICON tray_icons[4] = {};  // detenido, esperando, conectado, error
  HICON app_icon = nullptr, app_icon_small = nullptr;
  UINT taskbar_created = 0;
  ULONG_PTR gdiplus_token = 0;

  // Dibujo
  std::unique_ptr<gp::FontFamily> family;
  std::wstring semibold_family;
  std::vector<Hit> hits;
  int hover = -1;
  bool mouse_tracked = false;
  float pulse = 0;            // animación del punto de estado mientras espera
  gp::RectF pulse_rect;       // zona que se repinta con la animación
  float content_bottom = 0;   // alto que necesita el contenido (para ajustar la ventana)

  // Datos
  Settings settings;
  HostProcess host;
  State state = State::kStopped;
  bool restart_after_stop = false;  // «Aplicar cambios»: al terminar el host se vuelve a arrancar
  ULONGLONG started_at = 0;
  std::wstring args;                // opciones con las que corre el host
  std::wstring error;               // mensaje para el usuario
  bool error_mapped = false;        // error ya traducido a algo claro: no lo pisan las líneas siguientes
  std::map<std::wstring, std::wstring> stats;  // última línea «@estado»
  std::wstring link = L"ninguno";
  std::deque<std::wstring> log;
  bool dirty = false;               // ajustes cambiados con el host en marcha
  bool tray_hint_shown = false;
  bool driver_installed = false;
  bool autostart = false;
  std::vector<std::wstring> monitors;
  std::wstring addresses;
} g;

float F(float v) { return v * g.scale; }

std::wstring Format(const wchar_t* fmt, ...) {
  wchar_t buf[512];
  va_list args;
  va_start(args, fmt);
  _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
  va_end(args);
  return buf;
}

double Stat(const wchar_t* key) {
  const auto it = g.stats.find(key);
  return it == g.stats.end() ? 0.0 : _wtof(it->second.c_str());
}

bool Running() { return g.state != State::kStopped && g.state != State::kError; }

void Redraw() {
  if (g.wnd) InvalidateRect(g.wnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------------------------------------------
// Mensajes del host traducidos a algo que el usuario pueda resolver.

std::wstring FriendlyError(const std::wstring& line) {
  auto has = [&](const wchar_t* s) { return line.find(s) != std::wstring::npos; };
  if (has(L"driver del monitor virtual no está instalado")) {
    return L"Falta el driver de la pantalla extra. Instálalo con instalar-driver.bat (paquete "
           L"desklink-driver-windows) o elige «Duplicar».";
  }
  if (has(L"no se pudo abrir el driver")) {
    return L"Windows no deja abrir el driver de la pantalla extra. Vuelve a ejecutar instalar-driver.bat con el "
           L"driver de la última versión.";
  }
  if (has(L"no activó el monitor virtual")) {
    return L"Windows no activó la pantalla extra. En Configuración > Pantalla, elige «Extender estas pantallas».";
  }
  if (has(L"el driver no pudo crear el monitor")) {
    return L"El driver no pudo crear la pantalla extra. Prueba otra resolución o reinstala el driver.";
  }
  if (has(L"bind al puerto") || has(L"USB: no se pudo escuchar")) {
    return Format(L"El puerto %d ya está en uso. ¿Hay otro DeskLink abierto (por ejemplo desklink-host.exe en una "
                  L"consola)? Ciérralo y pulsa Iniciar.",
                  g.settings.port);
  }
  if (has(L"codificador")) {
    return g.settings.hevc ? L"La tarjeta gráfica no pudo iniciar el codificador HEVC. Elige H.264 en Códec o "
                             L"actualiza el driver de la GPU."
                           : L"La tarjeta gráfica no pudo iniciar el codificador de vídeo. Actualiza el driver de "
                             L"la GPU.";
  }
  if (has(L"No existe la salida")) return L"Ese monitor ya no está conectado. Elige otro en «Monitor».";
  if (has(L"DuplicateOutput falló") || has(L"no se pudo recuperar la captura")) {
    return L"Windows no deja capturar la pantalla ahora mismo (pantalla bloqueada, aviso de UAC o un juego a "
           L"pantalla completa).";
  }
  return {};
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

COLORREF StateColor(State s) {
  const COLORREF colors[4] = {color::kTextMuted, color::kWarning, color::kSuccess, color::kError};
  return colors[ColorIndex(s)];
}

std::wstring Title() {
  switch (g.state) {
    case State::kStopped: return L"Detenido";
    case State::kStarting: return L"Iniciando…";
    case State::kWaiting: return L"Esperando a la tablet";
    case State::kConnected:
      if (g.link == L"usb") return L"Conectado por cable USB";
      return L"Conectado por WiFi";
    case State::kStopping: return L"Deteniendo…";
    case State::kRestarting: return L"Reiniciando…";
    case State::kError: return L"No se pudo transmitir";
  }
  return {};
}

void TrayCommand(DWORD message) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g.wnd;
  nid.uID = kTrayId;
  nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  nid.uCallbackMessage = WM_TRAY;
  nid.hIcon = g.tray_icons[ColorIndex(g.state)];
  wcsncpy_s(nid.szTip, (std::wstring(kAppName) + L": " + Title()).c_str(), _TRUNCATE);
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

// Barra de título oscura (Windows 10 2004+ y 11) y, en Windows 11, del color del fondo.
void DarkTitleBar(HWND hwnd) {
  const BOOL dark = TRUE;
  if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)))) DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
  const COLORREF caption = color::kBackground;
  DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));  // DWMWA_CAPTION_COLOR
}

// Menús oscuros (los de la bandeja y los desplegables). uxtheme no lo documenta, pero es lo que usan el
// Explorador y el Bloc de notas; si no existe, los menús salen claros y ya está.
void DarkMenus() {
  HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!ux) return;
  using SetPreferredAppMode = int(WINAPI*)(int);
  using FlushMenuThemes = void(WINAPI*)();
  auto set_mode = reinterpret_cast<SetPreferredAppMode>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
  auto flush = reinterpret_cast<FlushMenuThemes>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
  if (set_mode) set_mode(2);  // ForceDark
  if (flush) flush();
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
      DarkTitleBar(hwnd);
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
      SetTextColor(dc, color::kText);
      SetBkColor(dc, color::kSurface);
      return reinterpret_cast<LRESULT>(g.log_bg);
    }
    case WM_ERASEBKGND: {
      RECT r;
      GetClientRect(hwnd, &r);
      HBRUSH b = CreateSolidBrush(color::kBackground);
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

void ShowLog() {
  if (g.log_wnd) {
    ShowWindow(g.log_wnd, SW_RESTORE);
    SetForegroundWindow(g.log_wnd);
    return;
  }
  g.log_wnd = CreateWindowExW(0, kLogClass, L"DeskLink: registro", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
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
  unsigned w, h;
  if (g.settings.virtual_display && !g.driver_installed) {
    g.error = FriendlyError(L"driver del monitor virtual no está instalado");
    SetState(State::kError);
    return;
  }
  if (g.settings.virtual_display && !g.settings.size.empty() && !ParseSize(g.settings.size, &w, &h)) {
    g.error = L"La resolución no es válida: escribe ANCHOxALTO, por ejemplo 2560x1600.";
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
  g.restart_after_stop = true;
  StopHost();
}

void OnHostLine(bool from_stderr, const std::wstring& line) {
  if (line.rfind(L"@listo", 0) == 0) {
    if (g.state == State::kStarting) SetState(State::kWaiting);
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
        Notify(L"Tablet desconectada", L"DeskLink sigue esperando a que vuelva a conectarse.");
      } else {
        Notify(L"Tablet conectada", link == L"usb" ? L"Por cable USB." : L"Por WiFi (" + g.stats[L"ip"] + L").");
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
  // Un aviso durante la transmisión (p. ej. un ajuste de códec no soportado) no es un fallo: se muestra igual,
  // pero solo pasa a «error» si el host termina.
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
    Notify(L"DeskLink se reinicia", g.error, true);
    SetTimer(g.wnd, kTimerRestart, 3000, nullptr);
    SetState(State::kRestarting);
    return;
  }
  Notify(L"DeskLink no pudo transmitir", g.error, true);
  SetState(State::kError);
}

// Un ajuste cambió: se guarda; con el host en marcha se ofrece «Aplicar cambios».
void SettingsChanged() {
  SaveSettings(g.settings);
  if (Running()) g.dirty = true;
  Redraw();
}

void SetMode(bool virtual_display, bool apply_now) {
  if (g.settings.virtual_display == virtual_display) return;
  g.settings.virtual_display = virtual_display;
  SettingsChanged();
  if (apply_now && Running()) ApplyChanges();
}

// ---------------------------------------------------------------------------------------------------------------
// Dibujo: piezas sueltas.

std::unique_ptr<gp::Font> MakeFont(float px, bool bold) {
  std::unique_ptr<gp::Font> f;
  if (bold && !g.semibold_family.empty()) {
    f = std::make_unique<gp::Font>(g.semibold_family.c_str(), F(px), gp::FontStyleRegular, gp::UnitPixel);
    if (f->GetLastStatus() == gp::Ok) return f;
  }
  return std::make_unique<gp::Font>(g.family.get(), F(px), bold ? gp::FontStyleBold : gp::FontStyleRegular,
                                    gp::UnitPixel);
}

void RoundRect(gp::GraphicsPath* path, const gp::RectF& r, float radius) {
  const float d = std::min(radius * 2, std::min(r.Width, r.Height));
  path->AddArc(r.X, r.Y, d, d, 180, 90);
  path->AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
  path->AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
  path->AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
  path->CloseFigure();
}

void FillRound(gp::Graphics& gr, const gp::RectF& r, float radius, const gp::Brush& brush) {
  gp::GraphicsPath path;
  RoundRect(&path, r, radius);
  gr.FillPath(&brush, &path);
}

void StrokeRound(gp::Graphics& gr, const gp::RectF& r, float radius, const gp::Pen& pen) {
  gp::GraphicsPath path;
  RoundRect(&path, r, radius);
  gr.DrawPath(&pen, &path);
}

// Halo: varios bordes cada vez más anchos y transparentes alrededor del rectángulo.
void Glow(gp::Graphics& gr, const gp::RectF& r, float radius, COLORREF c, float strength = 1) {
  for (int i = 4; i >= 1; --i) {
    gp::Pen pen(C(c, static_cast<BYTE>(std::min(255.0f, (34 - i * 6) * strength))), F(i * 3.0f));
    StrokeRound(gr, r, radius, pen);
  }
}

gp::LinearGradientBrush BrandBrush(const gp::RectF& r) {
  return gp::LinearGradientBrush(gp::RectF(r.X - 1, r.Y - 1, r.Width + 2, r.Height + 2), C(color::kAccent),
                                 C(color::kAccentAlt), gp::LinearGradientModeForwardDiagonal);
}

enum class Align { kLeft, kCenter, kRight };

void Text(gp::Graphics& gr, const std::wstring& s, const gp::RectF& r, const gp::Font& font, COLORREF c,
          Align align = Align::kLeft, bool wrap = false, BYTE alpha = 255) {
  gp::StringFormat sf;
  sf.SetAlignment(align == Align::kLeft ? gp::StringAlignmentNear
                  : align == Align::kCenter ? gp::StringAlignmentCenter : gp::StringAlignmentFar);
  sf.SetLineAlignment(wrap ? gp::StringAlignmentNear : gp::StringAlignmentCenter);
  sf.SetTrimming(gp::StringTrimmingEllipsisCharacter);
  if (!wrap) sf.SetFormatFlags(gp::StringFormatFlagsNoWrap);
  gp::SolidBrush brush(C(c, alpha));
  gr.DrawString(s.c_str(), static_cast<INT>(s.size()), &font, r, &sf, &brush);
}

// Texto en mayúsculas con espaciado entre letras (logotipo y títulos de sección).
void SpacedText(gp::Graphics& gr, const std::wstring& s, float x, float y, float px, float spacing,
                const gp::Brush& brush, bool bold) {
  gp::GraphicsPath path;
  gp::StringFormat sf(gp::StringFormat::GenericTypographic());
  const INT style = bold ? gp::FontStyleBold : gp::FontStyleRegular;
  auto font = MakeFont(px, bold);
  for (wchar_t ch : s) {
    path.AddString(&ch, 1, g.family.get(), style, F(px), gp::PointF(x, y), &sf);
    gp::RectF box;
    gr.MeasureString(&ch, 1, font.get(), gp::PointF(0, 0), &sf, &box);
    x += (ch == L' ' ? F(px) * 0.3f : box.Width) + F(spacing);
  }
  gr.FillPath(&brush, &path);
}

int AddHit(const gp::RectF& r, std::function<void()> action) {
  g.hits.push_back({r, std::move(action)});
  return static_cast<int>(g.hits.size()) - 1;
}

bool Hovered(int hit) { return hit == g.hover; }

void SectionTitle(gp::Graphics& gr, const std::wstring& s, float x, float y) {
  gp::SolidBrush brush(C(color::kAccent));
  SpacedText(gr, s, x, y, 12, 2, brush, true);
}

// Fila de opciones (control segmentado): una pastilla por opción; la elegida en cian con halo.
void Segmented(gp::Graphics& gr, const gp::RectF& r, const std::vector<std::wstring>& labels, int selected,
               const std::function<void(int)>& on_select) {
  gp::SolidBrush raised(C(color::kSurfaceRaised));
  FillRound(gr, r, F(10), raised);
  gp::Pen outline(C(color::kOutline), F(1));
  StrokeRound(gr, r, F(10), outline);
  auto font = MakeFont(13, false);
  auto bold = MakeFont(13, true);
  const float w = r.Width / labels.size();
  for (size_t i = 0; i < labels.size(); ++i) {
    const gp::RectF cell(r.X + w * i + F(3), r.Y + F(3), w - F(6), r.Height - F(6));
    const int hit = AddHit(cell, [on_select, i] { on_select(static_cast<int>(i)); });
    if (static_cast<int>(i) == selected) {
      gp::SolidBrush fill(C(color::kAccent, 40));
      FillRound(gr, cell, F(8), fill);
      gp::Pen pen(C(color::kAccent), F(1.5f));
      StrokeRound(gr, cell, F(8), pen);
      Text(gr, labels[i], cell, *bold, color::kAccent, Align::kCenter);
    } else {
      if (Hovered(hit)) {
        gp::SolidBrush fill(C(color::kOutline));
        FillRound(gr, cell, F(8), fill);
      }
      Text(gr, labels[i], cell, *font, Hovered(hit) ? color::kText : color::kTextMuted, Align::kCenter);
    }
  }
}

// Interruptor con su texto; toda la fila responde al clic.
void Toggle(gp::Graphics& gr, const gp::RectF& r, const std::wstring& label, bool on, std::function<void()> action) {
  const int hit = AddHit(r, std::move(action));
  auto font = MakeFont(14, false);
  Text(gr, label, gp::RectF(r.X, r.Y, r.Width - F(56), r.Height), *font, Hovered(hit) ? color::kText : color::kText,
       Align::kLeft, false, Hovered(hit) ? 255 : 230);
  const gp::RectF track(r.X + r.Width - F(44), r.Y + (r.Height - F(24)) / 2, F(44), F(24));
  if (on) {
    gp::LinearGradientBrush brand = BrandBrush(track);
    FillRound(gr, track, F(12), brand);
    if (Hovered(hit)) Glow(gr, track, F(12), color::kAccent, 0.7f);
  } else {
    gp::SolidBrush fill(C(color::kSurfaceRaised));
    FillRound(gr, track, F(12), fill);
    gp::Pen pen(C(Hovered(hit) ? color::kTextMuted : color::kOutline), F(1.5f));
    StrokeRound(gr, track, F(12), pen);
  }
  const float knob = F(18);
  const float kx = on ? track.X + track.Width - knob - F(3) : track.X + F(3);
  gp::SolidBrush knob_brush(on ? C(color::kOnAccent) : C(color::kTextMuted));
  gr.FillEllipse(&knob_brush, kx, track.Y + F(3), knob, knob);
}

// Desplegable: al pulsar abre un menú con las opciones.
void Dropdown(gp::Graphics& gr, const gp::RectF& r, const std::wstring& text, std::function<void()> action,
              bool enabled = true) {
  const int hit = enabled ? AddHit(r, std::move(action)) : -1;
  gp::SolidBrush fill(C(color::kSurfaceRaised));
  FillRound(gr, r, F(10), fill);
  gp::Pen pen(C(Hovered(hit) ? color::kAccent : color::kOutline), F(Hovered(hit) ? 1.5f : 1));
  StrokeRound(gr, r, F(10), pen);
  auto font = MakeFont(13, false);
  Text(gr, text, gp::RectF(r.X + F(12), r.Y, r.Width - F(40), r.Height), *font,
       enabled ? color::kText : color::kTextMuted);
  // Flecha
  const float cx = r.X + r.Width - F(18), cy = r.Y + r.Height / 2;
  gp::Pen arrow(C(enabled ? color::kAccent : color::kTextMuted), F(1.8f));
  arrow.SetLineJoin(gp::LineJoinRound);
  arrow.SetStartCap(gp::LineCapRound);
  arrow.SetEndCap(gp::LineCapRound);
  const gp::PointF pts[3] = {{cx - F(5), cy - F(2.5f)}, {cx, cy + F(2.5f)}, {cx + F(5), cy - F(2.5f)}};
  gr.DrawLines(&arrow, pts, 3);
}

// Botón: principal (degradado de marca), peligro (borde rojo) o secundario (borde cian).
enum class ButtonKind { kPrimary, kDanger, kSecondary, kDisabled };

void Button(gp::Graphics& gr, const gp::RectF& r, const std::wstring& label, ButtonKind kind, float text_px,
            std::function<void()> action) {
  const int hit = kind == ButtonKind::kDisabled ? -1 : AddHit(r, std::move(action));
  const float radius = F(14);
  auto font = MakeFont(text_px, true);
  switch (kind) {
    case ButtonKind::kPrimary: {
      if (Hovered(hit)) Glow(gr, r, radius, color::kAccent, 1.4f);
      gp::LinearGradientBrush brand = BrandBrush(r);
      FillRound(gr, r, radius, brand);
      Text(gr, label, r, *font, color::kOnAccent, Align::kCenter);
      break;
    }
    case ButtonKind::kDanger: {
      if (Hovered(hit)) Glow(gr, r, radius, color::kError, 1.0f);
      gp::SolidBrush fill(C(color::kError, Hovered(hit) ? 40 : 22));
      FillRound(gr, r, radius, fill);
      gp::Pen pen(C(color::kError), F(1.5f));
      StrokeRound(gr, r, radius, pen);
      Text(gr, label, r, *font, color::kError, Align::kCenter);
      break;
    }
    case ButtonKind::kSecondary: {
      gp::SolidBrush fill(C(color::kAccent, Hovered(hit) ? 40 : 18));
      FillRound(gr, r, radius, fill);
      gp::Pen pen(C(color::kAccent), F(1.5f));
      StrokeRound(gr, r, radius, pen);
      Text(gr, label, r, *font, color::kAccent, Align::kCenter);
      break;
    }
    case ButtonKind::kDisabled: {
      gp::SolidBrush fill(C(color::kSurfaceRaised));
      FillRound(gr, r, radius, fill);
      Text(gr, label, r, *font, color::kTextMuted, Align::kCenter);
      break;
    }
  }
}

// Menú desplegable bajo un control; devuelve el índice elegido o -1.
int PickFromMenu(const gp::RectF& anchor, const std::vector<std::wstring>& items, int checked) {
  HMENU m = CreatePopupMenu();
  for (size_t i = 0; i < items.size(); ++i) {
    AppendMenuW(m, MF_STRING | (static_cast<int>(i) == checked ? MF_CHECKED : 0), kMenuPick + i, items[i].c_str());
  }
  POINT pt{static_cast<LONG>(anchor.X), static_cast<LONG>(anchor.Y + anchor.Height + F(4))};
  ClientToScreen(g.wnd, &pt);
  const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, pt.x, pt.y, 0, g.wnd,
                                 nullptr);
  DestroyMenu(m);
  return cmd >= kMenuPick ? cmd - kMenuPick : -1;
}

// ---------------------------------------------------------------------------------------------------------------
// Dibujo: la ventana.

void PaintHeader(gp::Graphics& gr) {
  // Logotipo: la tableta con degradado y «DESKLINK».
  const gp::RectF body(F(28), F(26), F(34), F(24));
  gp::LinearGradientBrush brand = BrandBrush(body);
  FillRound(gr, body, F(6), brand);
  gp::SolidBrush screen(C(color::kBackground));
  FillRound(gr, gp::RectF(body.X + F(4), body.Y + F(4), body.Width - F(8), body.Height - F(8)), F(2), screen);
  gp::LinearGradientBrush text_brush(gp::RectF(F(72), F(20), F(170), F(30)), C(color::kAccent), C(color::kAccentAlt),
                                     gp::LinearGradientModeHorizontal);
  SpacedText(gr, L"DESKLINK", F(72), F(21), 24, 4, text_brush, true);
  auto small = MakeFont(12, false);
  Text(gr, L"Tu tablet como segunda pantalla", gp::RectF(F(74), F(50), F(260), F(16)), *small, color::kTextMuted);
}

void PaintStatusCard(gp::Graphics& gr, const gp::RectF& card) {
  const bool connected = g.state == State::kConnected;
  if (connected) Glow(gr, card, F(20), color::kAccent, 0.8f);
  gp::SolidBrush surface(C(color::kSurface));
  FillRound(gr, card, F(20), surface);
  if (connected) {
    gp::LinearGradientBrush brand = BrandBrush(card);
    gp::Pen pen(&brand, F(1.5f));
    StrokeRound(gr, card, F(20), pen);
  } else {
    gp::Pen pen(C(color::kOutline), F(1));
    StrokeRound(gr, card, F(20), pen);
  }

  const float x = card.X + F(20);
  const float w = card.Width - F(40);
  float y = card.Y + F(20);

  // Punto de estado con halo; mientras espera, el halo late.
  const COLORREF sc = StateColor(g.state);
  const float dot = F(12);
  const gp::RectF dot_rect(x, y + F(7), dot, dot);
  g.pulse_rect = gp::RectF(dot_rect.X - F(12), dot_rect.Y - F(12), dot + F(24), dot + F(24));
  const float beat = (g.state == State::kConnected || g.state == State::kError || g.state == State::kStopped)
                         ? 1.0f
                         : 0.5f + 0.5f * std::sin(g.pulse);
  if (g.state != State::kStopped) {
    const float grow = F(11) * (0.6f + 0.4f * beat);
    gp::GraphicsPath halo;
    halo.AddEllipse(dot_rect.X - grow, dot_rect.Y - grow, dot + 2 * grow, dot + 2 * grow);
    gp::PathGradientBrush brush(&halo);
    brush.SetCenterColor(C(sc, static_cast<BYTE>(70 + 70 * beat)));
    gp::Color edge = C(sc, 0);
    INT count = 1;
    brush.SetSurroundColors(&edge, &count);
    gr.FillPath(&brush, &halo);
  }
  gp::SolidBrush dot_brush(C(sc));
  gr.FillEllipse(&dot_brush, dot_rect);

  auto title = MakeFont(19, true);
  Text(gr, Title(), gp::RectF(x + F(24), y, w - F(24), F(26)), *title, color::kText);
  y += F(30);

  auto body = MakeFont(13, false);
  std::wstring detail;
  switch (g.state) {
    case State::kStopped: detail = L"Pulsa Iniciar para usar la tablet como pantalla."; break;
    case State::kStarting:
      detail = g.settings.virtual_display ? L"Creando la pantalla extra…" : L"Preparando la captura…";
      break;
    case State::kWaiting: detail = L"Abre DeskLink en la tablet y pulsa Conectar."; break;
    case State::kConnected:
      detail = g.link == L"usb" ? L"Tablet por cable: la conexión más rápida." : L"Tablet " + g.stats[L"ip"] + L" por WiFi.";
      if (!g.stats[L"size"].empty()) detail += L" Pantalla de " + g.stats[L"size"] + L".";
      break;
    case State::kStopping: detail = L"Quitando la pantalla extra y el túnel USB…"; break;
    case State::kRestarting: detail = L"El host se cerró; vuelvo a arrancarlo."; break;
    case State::kError: detail = L"Corrige esto y pulsa Iniciar:"; break;
  }
  Text(gr, detail, gp::RectF(x, y, w, F(38)), *body, color::kTextMuted, Align::kLeft, true);
  y += F(42);

  if (!g.error.empty() && (g.state == State::kError || g.state == State::kRestarting)) {
    // El error ocupa el sitio de las cifras.
    const gp::RectF box(x, y, w, F(64));
    gp::SolidBrush fill(C(color::kError, 22));
    FillRound(gr, box, F(12), fill);
    gp::Pen pen(C(color::kError, 120), F(1));
    StrokeRound(gr, box, F(12), pen);
    Text(gr, g.error, gp::RectF(box.X + F(12), box.Y + F(8), box.Width - F(24), box.Height - F(12)), *body,
         color::kError, Align::kLeft, true);
  } else {
    // Cuatro cifras: fps, Mbps, codificación y RTT.
    const bool live = connected;
    const double fps = Stat(L"codificados");
    const std::wstring values[4] = {
        live ? Format(L"%.0f", fps) : L"—",
        live ? Format(L"%.1f", Stat(L"mbps")) : L"—",
        live ? Format(L"%.1f", Stat(L"cod_ms")) : L"—",
        live && Stat(L"rtt_ms") > 0 ? Format(L"%.0f", Stat(L"rtt_ms")) : L"—",
    };
    const wchar_t* labels[4] = {live && fps < 1 ? L"fps · quieta" : L"fps", L"Mbps", L"ms codif.", L"ms RTT"};
    auto value_font = MakeFont(20, true);
    auto label_font = MakeFont(11, false);
    const float gap = F(8);
    const float cw = (w - 3 * gap) / 4;
    for (int i = 0; i < 4; ++i) {
      const gp::RectF chip(x + i * (cw + gap), y, cw, F(64));
      gp::SolidBrush fill(C(color::kSurfaceRaised));
      FillRound(gr, chip, F(12), fill);
      Text(gr, values[i], gp::RectF(chip.X, chip.Y + F(8), chip.Width, F(28)), *value_font,
           live ? color::kText : color::kTextMuted, Align::kCenter);
      Text(gr, labels[i], gp::RectF(chip.X, chip.Y + F(38), chip.Width, F(18)), *label_font, color::kTextMuted,
           Align::kCenter);
    }
  }
  y += F(76);

  // Cable USB e IP del PC.
  std::wstring usb;
  COLORREF usb_color = color::kTextMuted;
  if (!g.settings.usb) {
    usb = L"Cable USB desactivado en los ajustes.";
  } else if ((g.state == State::kWaiting || g.state == State::kConnected) && g.stats.count(L"adb")) {
    const int ready = _wtoi(g.stats[L"adb"].c_str());
    const int pending = _wtoi(g.stats[L"adb_pendientes"].c_str());
    if (ready < 0) {
      usb = L"Para el cable USB falta adb.exe junto a desklink.exe.";
      usb_color = color::kWarning;
    } else if (pending > 0) {
      usb = L"Acepta «¿Permitir depuración USB?» en la tablet.";
      usb_color = color::kWarning;
    } else if (ready > 0) {
      usb = ready == 1 ? L"Tablet enchufada por USB y lista." : Format(L"%d tablets enchufadas por USB.", ready);
      usb_color = color::kSuccess;
    } else {
      usb = L"Sin tablet por USB (activa la depuración USB).";
    }
  } else {
    usb = L"Cable USB: se activa al iniciar.";
  }
  auto small = MakeFont(12, false);
  Text(gr, usb, gp::RectF(x, y, w, F(18)), *small, usb_color);
  y += F(20);
  Text(gr, L"IP de este PC: " + (g.addresses.empty() ? std::wstring(L"sin red") : g.addresses),
       gp::RectF(x, y, w, F(18)), *small, color::kTextMuted);
}

void PaintSettingsCard(gp::Graphics& gr, float card_x, float card_y, float card_w, float* bottom) {
  // El alto de la tarjeta depende de las filas: primero se dibuja el contenido en una capa y luego el fondo
  // debajo. Para simplificar, el fondo se pinta con el alto calculado en la pasada anterior (ver Paint).
  Settings& s = g.settings;
  const float x = card_x + F(20);
  const float w = card_w - F(40);
  float y = card_y + F(20);
  auto label_font = MakeFont(13, false);
  auto hint_font = MakeFont(12, false);

  auto row_label = [&](const std::wstring& text, const std::wstring& hint = L"",
                       COLORREF hint_color = color::kTextMuted) {
    Text(gr, text, gp::RectF(x, y, w, F(18)), *label_font, color::kText);
    if (!hint.empty()) Text(gr, hint, gp::RectF(x, y, w, F(18)), *hint_font, hint_color, Align::kRight);
    y += F(22);
  };

  SectionTitle(gr, L"PANTALLA", x, y);
  y += F(28);

  row_label(L"Modo", g.driver_installed ? L"" : L"falta el driver de la pantalla extra", color::kWarning);
  Segmented(gr, gp::RectF(x, y, w, F(36)), {L"Pantalla extra", L"Duplicar un monitor"}, s.virtual_display ? 0 : 1,
            [](int i) { SetMode(i == 0, false); });
  y += F(48);

  if (s.virtual_display) {
    row_label(L"Resolución", L"la tablet puede pedir la suya");
    const std::wstring text = s.size.empty() ? L"Automática (la de la tablet o la última)" : s.size;
    gp::RectF r(x, y, w, F(36));
    Dropdown(gr, r, text, [r] {
      std::vector<std::wstring> items = {L"Automática (la de la tablet o la última)"};
      int checked = g.settings.size.empty() ? 0 : -1;
      bool found = g.settings.size.empty();
      for (const wchar_t* p : kSizePresets) {
        if (g.settings.size == p) {
          checked = static_cast<int>(items.size());
          found = true;
        }
        items.push_back(p);
      }
      if (!found) {  // una resolución puesta a mano en el .ini
        checked = static_cast<int>(items.size());
        items.push_back(g.settings.size);
      }
      const int pick = PickFromMenu(r, items, checked);
      if (pick < 0 || pick == checked) return;
      g.settings.size = pick == 0 ? L"" : items[pick];
      SettingsChanged();
    });
  } else {
    row_label(L"Monitor que se duplica");
    const int out = s.output;
    const std::wstring text =
        out < static_cast<int>(g.monitors.size()) ? g.monitors[out] : std::to_wstring(out + 1) + L": (no conectado)";
    gp::RectF r(x, y, w, F(36));
    Dropdown(gr, r, text, [r] {
      g.monitors = MonitorNames();
      const int pick = PickFromMenu(r, g.monitors, g.settings.output);
      if (pick < 0 || pick == g.settings.output) return;
      g.settings.output = pick;
      SettingsChanged();
    });
  }
  y += F(48);

  row_label(L"Fluidez", L"fotogramas por segundo");
  {
    std::vector<std::wstring> labels;
    int selected = -1;
    for (size_t i = 0; i < std::size(kFpsChoices); ++i) {
      labels.push_back(std::to_wstring(kFpsChoices[i]));
      if (kFpsChoices[i] == s.fps) selected = static_cast<int>(i);
    }
    Segmented(gr, gp::RectF(x, y, w, F(36)), labels, selected, [](int i) {
      g.settings.fps = kFpsChoices[i];
      SettingsChanged();
    });
  }
  y += F(48);

  row_label(L"Calidad", Format(L"hasta %d Mbps, baja sola si hace falta", s.bitrate));
  {
    int selected = -1;
    for (size_t i = 0; i < std::size(kQualityMbps); ++i) {
      if (kQualityMbps[i] == s.bitrate) selected = static_cast<int>(i);
    }
    Segmented(gr, gp::RectF(x, y, w, F(36)), {kQualityNames[0], kQualityNames[1], kQualityNames[2]}, selected,
              [](int i) {
                g.settings.bitrate = kQualityMbps[i];
                SettingsChanged();
              });
  }
  y += F(48);

  row_label(L"Códec");
  Segmented(gr, gp::RectF(x, y, w, F(36)), {L"HEVC · mejor imagen", L"H.264 · más compatible"}, s.hevc ? 0 : 1,
            [](int i) {
              g.settings.hevc = i == 0;
              SettingsChanged();
            });
  y += F(52);

  SectionTitle(gr, L"CONTROL", x, y);
  y += F(28);
  row_label(L"Toque en la tablet");
  Segmented(gr, gp::RectF(x, y, w, F(36)), {L"Táctil", L"Ratón", L"Apagado"}, s.touch, [](int i) {
    g.settings.touch = i;
    SettingsChanged();
  });
  y += F(46);
  Toggle(gr, gp::RectF(x, y, w, F(32)), L"Dibujar el puntero del ratón", s.cursor, [] {
    g.settings.cursor = !g.settings.cursor;
    SettingsChanged();
  });
  y += F(36);
  Toggle(gr, gp::RectF(x, y, w, F(32)), L"Conexión por cable USB (adb)", s.usb, [] {
    g.settings.usb = !g.settings.usb;
    SettingsChanged();
  });
  y += F(36);

  // Ajustes que llegarán con otros cambios («Táctil directo», «Audio del PC en la tablet», «Perfiles de
  // batería»): cada uno añade su campo en Settings (settings.h/.cpp), su opción en HostArguments y aquí una fila
  // (Toggle o Segmented) en la sección que le toque, por ejemplo una sección «AUDIO» o «BATERÍA» con SectionTitle.
  // La ventana crece sola con el contenido.

  *bottom = y + F(8);
}

void Paint(gp::Graphics& gr, float width) {
  g.hits.clear();
  gr.SetSmoothingMode(gp::SmoothingModeAntiAlias);
  gr.SetTextRenderingHint(gp::TextRenderingHintClearTypeGridFit);
  gr.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
  gp::SolidBrush bg(C(color::kBackground));
  gr.FillRectangle(&bg, 0.0f, 0.0f, width, 10000.0f);

  // Brillo de fondo muy suave en una esquina: da profundidad sin distraer.
  {
    gp::GraphicsPath glow;
    glow.AddEllipse(-F(160), -F(200), F(520), F(420));
    gp::PathGradientBrush brush(&glow);
    brush.SetCenterColor(C(color::kAccent, 22));
    gp::Color edge = C(color::kAccent, 0);
    INT count = 1;
    brush.SetSurroundColors(&edge, &count);
    gr.FillPath(&brush, &glow);
  }

  PaintHeader(gr);

  // Columna izquierda: estado, botón principal y arranque.
  const float left_x = F(24), left_w = F(330);
  const gp::RectF status(left_x, F(84), left_w, F(234));
  PaintStatusCard(gr, status);

  float y = status.Y + status.Height + F(18);
  const gp::RectF primary(left_x, y, left_w, F(54));
  switch (g.state) {
    case State::kStopped:
    case State::kError:
      Button(gr, primary, L"INICIAR", ButtonKind::kPrimary, 16, ToggleHost);
      break;
    case State::kStopping:
      Button(gr, primary, L"DETENIENDO…", ButtonKind::kDisabled, 16, nullptr);
      break;
    default:
      Button(gr, primary, L"DETENER", ButtonKind::kDanger, 16, ToggleHost);
      break;
  }
  y += F(66);
  if (g.dirty && Running() && g.state != State::kStopping) {
    Button(gr, gp::RectF(left_x, y, left_w, F(40)), L"Aplicar cambios (reinicia la transmisión)",
           ButtonKind::kSecondary, 13, ApplyChanges);
    y += F(52);
  }

  SectionTitle(gr, L"INICIO", left_x + F(4), y + F(4));
  y += F(30);
  Toggle(gr, gp::RectF(left_x + F(4), y, left_w - F(8), F(32)), L"Abrir DeskLink al iniciar Windows", g.autostart, [] {
    SetStartWithWindows(!g.autostart);
    g.autostart = StartsWithWindows();
    Redraw();
  });
  y += F(36);
  Toggle(gr, gp::RectF(left_x + F(4), y, left_w - F(8), F(32)), L"Transmitir nada más abrir", g.settings.stream_on_open,
         [] {
           g.settings.stream_on_open = !g.settings.stream_on_open;
           SaveSettings(g.settings);  // no afecta al host en marcha
           Redraw();
         });
  y += F(42);

  // Enlace al registro.
  {
    auto font = MakeFont(13, false);
    const gp::RectF link(left_x + F(4), y, F(140), F(22));
    const int hit = AddHit(link, ShowLog);
    Text(gr, L"Ver registro  ›", link, *font, Hovered(hit) ? color::kAccent : color::kTextMuted);
    y += F(30);
  }
  const float left_bottom = y;

  // Columna derecha: ajustes. El fondo de la tarjeta se pinta con el alto de la pasada anterior y el contenido
  // encima; si el alto cambió (p. ej. una fila nueva), se repinta.
  const float right_x = F(378), right_w = width - right_x - F(24);
  static float settings_bottom = F(560);
  const gp::RectF card(right_x, F(84), right_w, settings_bottom - F(84));
  gp::SolidBrush surface(C(color::kSurface));
  FillRound(gr, card, F(20), surface);
  gp::Pen pen(C(color::kOutline), F(1));
  StrokeRound(gr, card, F(20), pen);
  float bottom = 0;
  PaintSettingsCard(gr, right_x, F(84), right_w, &bottom);
  if (std::fabs(bottom - settings_bottom) > 0.5f) {
    settings_bottom = bottom;
    Redraw();
  }

  g.content_bottom = std::max(left_bottom, settings_bottom) + F(24);
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
  {
    gp::Graphics gr(mem);
    Paint(gr, static_cast<float>(w));
  }
  BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
         mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  EndPaint(hwnd, &ps);

  // La ventana toma el alto del contenido.
  const int want = static_cast<int>(std::ceil(g.content_bottom));
  if (want > 0 && std::abs(want - h) > 1) {
    RECT wr{0, 0, w, want};
    AdjustWindowRectEx(&wr, GetWindowLongW(hwnd, GWL_STYLE), FALSE, GetWindowLongW(hwnd, GWL_EXSTYLE));
    SetWindowPos(hwnd, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER);
  }
}

int HitAt(int x, int y) {
  for (size_t i = 0; i < g.hits.size(); ++i) {
    if (g.hits[i].rect.Contains(static_cast<float>(x), static_cast<float>(y))) return static_cast<int>(i);
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
  AppendMenuW(m, MF_STRING, kMenuOpen, L"Abrir DeskLink");
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
    case WM_KEYDOWN:
      if (wp == VK_SPACE || wp == VK_RETURN) ToggleHost();
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
        g.pulse += 0.25f;
        RECT r{static_cast<LONG>(g.pulse_rect.X), static_cast<LONG>(g.pulse_rect.Y),
               static_cast<LONG>(g.pulse_rect.X + g.pulse_rect.Width + 1),
               static_cast<LONG>(g.pulse_rect.Y + g.pulse_rect.Height + 1)};
        InvalidateRect(hwnd, &r, FALSE);
      }
      return 0;
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case kMenuOpen: ShowMain(); return 0;
        case kMenuToggle: ToggleHost(); return 0;
        case kMenuVirtual: SetMode(true, true); return 0;
        case kMenuMirror: SetMode(false, true); return 0;
        case kMenuAutostart:
          SetStartWithWindows(!StartsWithWindows());
          g.autostart = StartsWithWindows();
          Redraw();
          return 0;
        case kMenuExit: ExitApp(); return 0;
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
        Notify(L"DeskLink sigue abierto", L"Está junto al reloj. Para cerrarlo: clic derecho en el icono > Salir.");
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
  DarkMenus();
  g.inst = inst;
  HDC dc = GetDC(nullptr);
  g.dpi = GetDeviceCaps(dc, LOGPIXELSY);
  ReleaseDC(nullptr, dc);
  g.scale = g.dpi / 96.0f;

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
  g.log_bg = CreateSolidBrush(color::kSurface);

  // Iconos de la bandeja: la tableta en el color del estado.
  const COLORREF tray_colors[4] = {color::kTextMuted, color::kWarning, color::kSuccess, color::kError};
  for (int i = 0; i < 4; ++i) {
    g.tray_icons[i] = CreateTabletIcon(tray_colors[i], tray_colors[i], GetSystemMetrics(SM_CXSMICON));
  }
  g.app_icon = LoadIconW(inst, L"APP");
  if (!g.app_icon) g.app_icon = CreateTabletIcon(color::kAccent, color::kAccentAlt, GetSystemMetrics(SM_CXICON));
  g.app_icon_small = static_cast<HICON>(LoadImageW(inst, L"APP", IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                   GetSystemMetrics(SM_CYSMICON), 0));
  if (!g.app_icon_small) {
    g.app_icon_small = CreateTabletIcon(color::kAccent, color::kAccentAlt, GetSystemMetrics(SM_CXSMICON));
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
  DarkTitleBar(g.wnd);

  // Primera pasada de dibujo fuera de pantalla para saber el alto del contenido y centrar la ventana ya con él.
  {
    HDC screen = GetDC(g.wnd);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, static_cast<int>(F(kClientW)), static_cast<int>(F(900)));
    HGDIOBJ old = SelectObject(mem, bmp);
    for (int pass = 0; pass < 2; ++pass) {  // la segunda ya usa el alto de la tarjeta de ajustes
      gp::Graphics gr(mem);
      Paint(gr, F(kClientW));
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(g.wnd, screen);
    RECT wr{0, 0, static_cast<LONG>(F(kClientW)), static_cast<LONG>(std::ceil(g.content_bottom))};
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
  gp::GdiplusShutdown(g.gdiplus_token);
  ReleaseMutex(mutex);
  return 0;
}
