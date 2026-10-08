// DeskLink: app de bandeja para Windows. Arranca desklink-host.exe sin consola con los ajustes guardados, muestra
// el estado de la conexión (cable USB o WiFi, fps, Mbps, RTT) y explica los errores con palabras normales.
// La captura y la codificación siguen en desklink-host.exe, que también se puede usar solo desde una consola.
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>

#include <cstdarg>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "host_process.h"
#include "settings.h"
#include "system_info.h"

#if defined(_MSC_VER)
#pragma comment(lib, "comctl32.lib")
#endif

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
constexpr ULONGLONG kStableRunMs = 15000;  // si el host cae después de funcionar este tiempo, se reinicia solo

enum Id {
  kIdStatusIcon = 100, kIdTitle, kIdDetail, kIdUsbLine, kIdIpLine, kIdError,
  kIdStart, kIdApply, kIdLog,
  kIdMode, kIdMonitor, kIdSize, kIdFps, kIdCodec, kIdBitrate, kIdTouch,
  kIdCursor, kIdUsb, kIdAutostart, kIdStreamOnOpen,
  kIdLogEdit, kIdLogCopy,
  kMenuOpen = 200, kMenuToggle, kMenuVirtual, kMenuMirror, kMenuAutostart, kMenuExit,
};

enum class State { kStopped, kStarting, kWaiting, kConnected, kStopping, kRestarting, kError };

const wchar_t* const kSizePresets[] = {L"2944x1840 (Lenovo Idea Tab Pro)", L"2560x1600", L"2000x1200",
                                       L"1920x1200", L"1920x1080", L"1280x800"};
constexpr wchar_t kSizeRemembered[] = L"La última usada";
const int kFpsChoices[] = {30, 60, 90, 120, 144};
const int kBitrateChoices[] = {10, 20, 30, 50, 80, 100, 150};

struct App {
  HINSTANCE inst = nullptr;
  HWND wnd = nullptr;
  HWND log_wnd = nullptr;
  HWND log_edit = nullptr;
  int dpi = 96;
  HFONT font = nullptr, title_font = nullptr, mono_font = nullptr;
  HBRUSH bg = nullptr;
  HICON tray_icons[4] = {};    // gris, ámbar, verde, rojo
  HICON status_icons[4] = {};
  HICON app_icon = nullptr, app_icon_small = nullptr;
  UINT taskbar_created = 0;

  Settings settings;
  HostProcess host;
  State state = State::kStopped;
  bool restart_after_stop = false;  // «Aplicar cambios»: al terminar el host se vuelve a arrancar
  ULONGLONG started_at = 0;
  std::wstring args;                // opciones con las que corre el host
  std::wstring error;               // mensaje para el usuario (rojo)
  bool error_mapped = false;        // error ya traducido a algo claro: no lo pisan las líneas siguientes
  std::map<std::wstring, std::wstring> stats;  // última línea «@estado»
  std::wstring link = L"ninguno";
  std::deque<std::wstring> log;
  bool dirty = false;               // ajustes cambiados con el host en marcha
  bool updating = false;            // rellenando controles: no son cambios del usuario
  bool tray_hint_shown = false;
} g;

int S(int v) { return MulDiv(v, g.dpi, 96); }

HWND Item(int id) { return GetDlgItem(g.wnd, id); }

// Solo si cambia: los textos se refrescan cada segundo y reescribirlos igual parpadea.
void SetText(int id, const std::wstring& text) {
  HWND c = Item(id);
  std::wstring old(GetWindowTextLengthW(c) + 1, L'\0');
  old.resize(GetWindowTextW(c, old.data(), static_cast<int>(old.size())));
  if (text != old) SetWindowTextW(c, text.c_str());
}

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

// ---------------------------------------------------------------------------------------------------------------
// Mensajes del host traducidos a algo que el usuario pueda resolver.

std::wstring FriendlyError(const std::wstring& line) {
  auto has = [&](const wchar_t* s) { return line.find(s) != std::wstring::npos; };
  if (has(L"driver del monitor virtual no está instalado")) {
    return L"Falta el driver del monitor virtual. Instálalo con instalar-driver.bat (paquete "
           L"desklink-driver-windows) o elige «Duplicar un monitor» en Modo.";
  }
  if (has(L"no se pudo abrir el driver")) {
    return L"Windows no deja abrir el driver del monitor virtual. Vuelve a ejecutar instalar-driver.bat con el "
           L"driver de la última versión.";
  }
  if (has(L"no activó el monitor virtual")) {
    return L"Windows no activó el monitor virtual. En Configuración > Pantalla, elige «Extender estas pantallas».";
  }
  if (has(L"el driver no pudo crear el monitor")) {
    return L"El driver no pudo crear el monitor virtual. Prueba otra resolución o reinstala el driver.";
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

std::wstring Title() {
  switch (g.state) {
    case State::kStopped: return L"Detenido";
    case State::kStarting: return L"Iniciando…";
    case State::kWaiting: return L"Esperando a la tablet";
    case State::kConnected:
      if (g.link == L"usb") return L"Conectado por cable USB";
      return L"Conectado por WiFi (" + g.stats[L"ip"] + L")";
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
      g.log_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                   0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kIdLogEdit), g.inst, nullptr);
      CreateWindowExW(0, L"BUTTON", L"Copiar todo", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0,
                      hwnd, reinterpret_cast<HMENU>(kIdLogCopy), g.inst, nullptr);
      SendMessageW(g.log_edit, WM_SETFONT, reinterpret_cast<WPARAM>(g.mono_font), TRUE);
      SendMessageW(GetDlgItem(hwnd, kIdLogCopy), WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
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
      MoveWindow(g.log_edit, S(8), S(8), w - S(16), h - S(52), TRUE);
      MoveWindow(GetDlgItem(hwnd, kIdLogCopy), w - S(128), h - S(38), S(120), S(30), TRUE);
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
    case WM_CTLCOLORSTATIC:  // edit de solo lectura: fondo blanco, como el resto
      SetBkColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_WINDOW));
      return reinterpret_cast<LRESULT>(g.bg);
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
                              S(760), S(440), nullptr, nullptr, g.inst, nullptr);
  ShowWindow(g.log_wnd, SW_SHOW);
}

// ---------------------------------------------------------------------------------------------------------------
// Estado en la ventana principal.

void UpdateStatus() {
  SendMessageW(Item(kIdStatusIcon), STM_SETICON, reinterpret_cast<WPARAM>(g.status_icons[ColorIndex(g.state)]), 0);
  SetText(kIdTitle, Title());

  std::wstring detail;
  switch (g.state) {
    case State::kStopped:
      detail = L"Pulsa Iniciar para usar la tablet como pantalla.";
      break;
    case State::kStarting:
      detail = g.settings.virtual_display ? L"Creando el monitor virtual…" : L"Preparando la captura…";
      break;
    case State::kWaiting:
      detail = L"Abre DeskLink en la tablet y pulsa Conectar.";
      if (!g.stats[L"size"].empty()) detail += L" Pantalla de " + g.stats[L"size"] + L".";
      break;
    case State::kConnected: {
      const double fps = Stat(L"codificados");
      detail = g.stats[L"size"] + Format(L" · %.0f fps", fps);
      if (fps < 1) detail += L" (imagen quieta)";
      detail += Format(L" · %.1f Mbps · codificación %.1f ms", Stat(L"mbps"), Stat(L"cod_ms"));
      if (Stat(L"rtt_ms") > 0) detail += Format(L" · RTT %.0f ms", Stat(L"rtt_ms"));
      if (Stat(L"perdidos") > 0) detail += Format(L" · pierde %.0f frames/s", Stat(L"perdidos"));
      break;
    }
    case State::kStopping:
      detail = L"Quitando el monitor virtual y el túnel USB…";
      break;
    case State::kRestarting:
      detail = L"El host se cerró; vuelvo a arrancarlo.";
      break;
    case State::kError:
      detail = L"Corrige lo de abajo y pulsa Iniciar. «Registro» tiene el detalle.";
      break;
  }
  SetText(kIdDetail, detail);

  std::wstring usb;
  if (!g.settings.usb) {
    usb = L"Cable USB: desactivado en los ajustes.";
  } else if ((g.state == State::kWaiting || g.state == State::kConnected) && g.stats.count(L"adb")) {
    const int ready = _wtoi(g.stats[L"adb"].c_str());
    const int pending = _wtoi(g.stats[L"adb_pendientes"].c_str());
    if (ready < 0) {
      usb = L"Cable USB: falta adb.exe junto a desklink.exe (ver docs/usb.md).";
    } else if (pending > 0) {
      usb = L"Cable USB: acepta «¿Permitir depuración USB?» en la tablet.";
    } else if (ready > 0) {
      usb = ready == 1 ? L"Cable USB: tablet enchufada y lista." : Format(L"Cable USB: %d tablets listas.", ready);
    } else {
      usb = L"Cable USB: ninguna tablet enchufada con depuración USB.";
    }
  }
  SetText(kIdUsbLine, usb);
  SetText(kIdError, g.error);
  SetText(kIdStart, Running() ? L"Detener" : L"Iniciar");
  ShowWindow(Item(kIdApply), g.dirty && Running() ? SW_SHOW : SW_HIDE);
  TrayCommand(NIM_MODIFY);
}

void UpdateAddresses() {
  const auto ips = LocalAddresses();
  std::wstring text = L"IP de este PC: ";
  if (ips.empty()) text += L"sin red";
  for (size_t i = 0; i < ips.size() && i < 3; ++i) text += (i ? L", " : L"") + ips[i];
  SetText(kIdIpLine, text);
}

void SetState(State s) {
  g.state = s;
  UpdateStatus();
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
  unsigned w, h;
  if (g.settings.virtual_display && !VirtualDriverInstalled()) {
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
  if (Running()) {
    g.restart_after_stop = false;
    StopHost();
  } else {
    StartHost();
  }
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
      g.state = link == L"ninguno" ? State::kWaiting : State::kConnected;
    }
    UpdateStatus();
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
  UpdateStatus();
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
    g.error = Format(L"El host se cerró inesperadamente (código %lu). Abre «Registro» para ver el detalle.", code);
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

// ---------------------------------------------------------------------------------------------------------------
// Ajustes.

void FillMonitors() {
  HWND c = Item(kIdMonitor);
  SendMessageW(c, CB_RESETCONTENT, 0, 0);
  auto names = MonitorNames();
  if (g.settings.output >= static_cast<int>(names.size())) {
    for (int i = static_cast<int>(names.size()); i <= g.settings.output; ++i) {
      names.push_back(std::to_wstring(i + 1) + L": (no conectado)");
    }
  }
  for (const auto& n : names) SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n.c_str()));
  SendMessageW(c, CB_SETCURSEL, g.settings.output, 0);
}

void SelectOrAdd(HWND combo, int value, const wchar_t* suffix = L"") {
  const std::wstring text = std::to_wstring(value) + suffix;
  LRESULT i = SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(text.c_str()));
  if (i == CB_ERR) i = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
  SendMessageW(combo, CB_SETCURSEL, i, 0);
}

void UpdateEnabled() {
  EnableWindow(Item(kIdMonitor), !g.settings.virtual_display);
  EnableWindow(Item(kIdSize), g.settings.virtual_display);
}

void FillControls() {
  g.updating = true;
  const Settings& s = g.settings;
  HWND mode = Item(kIdMode);
  SendMessageW(mode, CB_RESETCONTENT, 0, 0);
  const std::wstring virtual_label = VirtualDriverInstalled() ? L"Monitor virtual (pantalla extra)"
                                                              : L"Monitor virtual (falta instalar el driver)";
  SendMessageW(mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(virtual_label.c_str()));
  SendMessageW(mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Duplicar un monitor"));
  SendMessageW(mode, CB_SETCURSEL, s.virtual_display ? 0 : 1, 0);

  FillMonitors();
  SetWindowTextW(Item(kIdSize), s.size.empty() ? kSizeRemembered : s.size.c_str());
  SelectOrAdd(Item(kIdFps), s.fps);
  SendMessageW(Item(kIdCodec), CB_SETCURSEL, s.hevc ? 0 : 1, 0);
  SetWindowTextW(Item(kIdBitrate), std::to_wstring(s.bitrate).c_str());
  SendMessageW(Item(kIdTouch), CB_SETCURSEL, s.touch, 0);
  Button_SetCheck(Item(kIdCursor), s.cursor ? BST_CHECKED : BST_UNCHECKED);
  Button_SetCheck(Item(kIdUsb), s.usb ? BST_CHECKED : BST_UNCHECKED);
  Button_SetCheck(Item(kIdAutostart), StartsWithWindows() ? BST_CHECKED : BST_UNCHECKED);
  Button_SetCheck(Item(kIdStreamOnOpen), s.stream_on_open ? BST_CHECKED : BST_UNCHECKED);
  UpdateEnabled();
  g.updating = false;
}

std::wstring ComboText(HWND combo, bool from_selection) {
  wchar_t buf[256] = {};
  if (from_selection) {
    // Al elegir de la lista, el texto del cuadro aún es el anterior.
    const LRESULT i = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (i != CB_ERR && SendMessageW(combo, CB_GETLBTEXTLEN, i, 0) < 256) {
      SendMessageW(combo, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(buf));
    }
  } else {
    GetWindowTextW(combo, buf, 256);
  }
  return buf;
}

// Un ajuste cambió en la ventana: se guarda; con el host en marcha se ofrece «Aplicar cambios».
void SettingsChanged() {
  SaveSettings(g.settings);
  if (Running()) g.dirty = true;
  UpdateEnabled();
  UpdateStatus();
}

void OnSettingCommand(int id, int code) {
  if (g.updating) return;
  Settings& s = g.settings;
  HWND c = Item(id);
  const bool selected = code == CBN_SELCHANGE;
  switch (id) {
    case kIdMode:
      if (!selected) return;
      s.virtual_display = SendMessageW(c, CB_GETCURSEL, 0, 0) == 0;
      break;
    case kIdMonitor:
      if (!selected) return;
      s.output = static_cast<int>(SendMessageW(c, CB_GETCURSEL, 0, 0));
      break;
    case kIdSize: {
      if (code == CBN_KILLFOCUS) {  // texto no válido: vuelve a lo guardado
        g.updating = true;
        SetWindowTextW(c, s.size.empty() ? kSizeRemembered : s.size.c_str());
        g.updating = false;
        return;
      }
      if (!selected && code != CBN_EDITCHANGE) return;
      const std::wstring text = ComboText(c, selected);
      unsigned w, h;
      std::wstring size;
      if (text != kSizeRemembered) {
        if (!ParseSize(text.substr(0, text.find(L" (")), &w, &h)) return;  // a medio escribir
        size = std::to_wstring(w) + L"x" + std::to_wstring(h);
      }
      if (size == s.size) return;
      s.size = size;
      break;
    }
    case kIdFps:
      if (!selected) return;
      s.fps = _wtoi(ComboText(c, true).c_str());
      break;
    case kIdCodec:
      if (!selected) return;
      s.hevc = SendMessageW(c, CB_GETCURSEL, 0, 0) == 0;
      break;
    case kIdBitrate: {
      if (code == CBN_KILLFOCUS) {
        g.updating = true;
        SetWindowTextW(c, std::to_wstring(s.bitrate).c_str());
        g.updating = false;
        return;
      }
      if (!selected && code != CBN_EDITCHANGE) return;
      const int v = _wtoi(ComboText(c, selected).c_str());
      if (v < 1 || v > 1000 || v == s.bitrate) return;
      s.bitrate = v;
      break;
    }
    case kIdTouch:
      if (!selected) return;
      s.touch = static_cast<int>(SendMessageW(c, CB_GETCURSEL, 0, 0));
      break;
    case kIdCursor:
      s.cursor = Button_GetCheck(c) == BST_CHECKED;
      break;
    case kIdUsb:
      s.usb = Button_GetCheck(c) == BST_CHECKED;
      break;
    case kIdStreamOnOpen:
      s.stream_on_open = Button_GetCheck(c) == BST_CHECKED;
      SaveSettings(s);  // no afecta al host en marcha
      return;
    case kIdAutostart:
      SetStartWithWindows(Button_GetCheck(c) == BST_CHECKED);
      return;
    default:
      return;
  }
  SettingsChanged();
}

// ---------------------------------------------------------------------------------------------------------------
// Ventana principal.

HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
  return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g.wnd,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g.inst, nullptr);
}

HWND Combo(int id, int x, int y, int w, bool editable = false) {
  return Make(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | (editable ? CBS_DROPDOWN : CBS_DROPDOWNLIST), x, y, w,
              240, id);
}

void AddItems(HWND combo, std::initializer_list<const wchar_t*> items) {
  for (const wchar_t* i : items) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i));
}

void CreateControls() {
  constexpr int kW = 480;  // ancho del área cliente a 96 ppp
  Make(L"STATIC", L"", SS_ICON | SS_REALSIZECONTROL, 16, 14, 32, 32, kIdStatusIcon);
  Make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 60, 12, kW - 76, 24, kIdTitle);
  Make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 60, 38, kW - 76, 18, kIdDetail);
  Make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 60, 58, kW - 76, 18, kIdUsbLine);
  Make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 60, 78, kW - 76, 18, kIdIpLine);
  Make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 16, 102, kW - 32, 34, kIdError);

  Make(L"BUTTON", L"Iniciar", WS_TABSTOP | BS_PUSHBUTTON, 16, 142, 130, 30, kIdStart);
  Make(L"BUTTON", L"Aplicar cambios", WS_TABSTOP | BS_PUSHBUTTON, 154, 142, 150, 30, kIdApply);
  Make(L"BUTTON", L"Registro…", WS_TABSTOP | BS_PUSHBUTTON, kW - 116, 142, 100, 30, kIdLog);

  Make(L"BUTTON", L"Ajustes", BS_GROUPBOX, 10, 184, kW - 20, 304, 0);
  constexpr int kLabelX = 26, kCtrlX = 136, kCtrlW = kW - kCtrlX - 26;
  int y = 208;
  auto label = [&](const wchar_t* text, int x, int w) { Make(L"STATIC", text, SS_LEFT, x, y + 3, w, 20, 0); };

  label(L"Modo", kLabelX, 100);
  Combo(kIdMode, kCtrlX, y, kCtrlW);
  y += 32;
  label(L"Monitor", kLabelX, 100);
  Combo(kIdMonitor, kCtrlX, y, kCtrlW);
  y += 32;
  label(L"Resolución", kLabelX, 100);
  HWND size = Combo(kIdSize, kCtrlX, y, kCtrlW, true);
  AddItems(size, {kSizeRemembered});
  for (const wchar_t* p : kSizePresets) SendMessageW(size, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p));
  y += 32;
  label(L"Fotogramas/s", kLabelX, 100);
  HWND fps = Combo(kIdFps, kCtrlX, y, 80);
  for (int f : kFpsChoices) SendMessageW(fps, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(std::to_wstring(f).c_str()));
  Make(L"STATIC", L"Códec", SS_RIGHT, kCtrlX + 90, y + 3, 50, 20, 0);
  AddItems(Combo(kIdCodec, kCtrlX + 148, y, kCtrlW - 148), {L"HEVC (mejor calidad)", L"H.264 (más compatible)"});
  y += 32;
  label(L"Calidad", kLabelX, 100);
  HWND bitrate = Combo(kIdBitrate, kCtrlX, y, 80, true);
  for (int b : kBitrateChoices) {
    SendMessageW(bitrate, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(std::to_wstring(b).c_str()));
  }
  Make(L"STATIC", L"Mbps como máximo (baja sola si hace falta)", SS_LEFT | SS_ENDELLIPSIS, kCtrlX + 88, y + 3,
       kCtrlW - 88, 20, 0);
  y += 32;
  label(L"Táctil", kLabelX, 100);
  AddItems(Combo(kIdTouch, kCtrlX, y, kCtrlW),
           {L"Táctil (varios dedos)", L"Ratón (un dedo mueve el puntero)", L"Desactivado"});
  y += 36;
  Make(L"BUTTON", L"Dibujar el puntero", WS_TABSTOP | BS_AUTOCHECKBOX, kLabelX, y, 200, 22, kIdCursor);
  Make(L"BUTTON", L"Cable USB (adb)", WS_TABSTOP | BS_AUTOCHECKBOX, kCtrlX + 110, y, kCtrlW - 110, 22, kIdUsb);
  y += 26;
  Make(L"BUTTON", L"Abrir DeskLink al iniciar Windows", WS_TABSTOP | BS_AUTOCHECKBOX, kLabelX, y, kW - 2 * kLabelX,
       22, kIdAutostart);
  y += 26;
  Make(L"BUTTON", L"Empezar a transmitir al abrir DeskLink", WS_TABSTOP | BS_AUTOCHECKBOX, kLabelX, y,
       kW - 2 * kLabelX, 22, kIdStreamOnOpen);

  EnumChildWindows(
      g.wnd,
      [](HWND child, LPARAM) -> BOOL {
        SendMessageW(child, WM_SETFONT,
                     reinterpret_cast<WPARAM>(GetDlgCtrlID(child) == kIdTitle ? g.title_font : g.font), TRUE);
        return TRUE;
      },
      0);
}

void ShowMain() {
  FillMonitors();
  UpdateAddresses();
  ShowWindow(g.wnd, IsIconic(g.wnd) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(g.wnd);
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
  AppendMenuW(m, MF_STRING, kMenuToggle, Running() ? L"Detener la transmisión" : L"Iniciar la transmisión");
  AppendMenuW(m, MF_STRING, kMenuVirtual, L"Monitor virtual");
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

void SetMode(bool virtual_display) {
  if (g.settings.virtual_display == virtual_display) return;
  g.settings.virtual_display = virtual_display;
  FillControls();
  SaveSettings(g.settings);
  if (Running()) {  // desde la bandeja se aplica enseguida
    g.restart_after_stop = true;
    StopHost();
  } else {
    UpdateStatus();
  }
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == g.taskbar_created && msg != 0) {  // el Explorador se reinició: hay que volver a poner el icono
    TrayCommand(NIM_ADD);
    return 0;
  }
  switch (msg) {
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
      }
      return 0;
    case WM_COMMAND: {
      const int id = LOWORD(wp);
      switch (id) {
        case kIdStart:
        case kMenuToggle:
          ToggleHost();
          return 0;
        case kIdApply:
          g.restart_after_stop = true;
          StopHost();
          return 0;
        case kIdLog:
          ShowLog();
          return 0;
        case kMenuOpen:
          ShowMain();
          return 0;
        case kMenuVirtual:
        case kMenuMirror:
          SetMode(id == kMenuVirtual);
          return 0;
        case kMenuAutostart:
          SetStartWithWindows(!StartsWithWindows());
          Button_SetCheck(Item(kIdAutostart), StartsWithWindows() ? BST_CHECKED : BST_UNCHECKED);
          return 0;
        case kMenuExit:
          ExitApp();
          return 0;
        default:
          OnSettingCommand(id, HIWORD(wp));
          return 0;
      }
    }
    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(wp);
      SetBkMode(dc, TRANSPARENT);
      if (GetDlgCtrlID(reinterpret_cast<HWND>(lp)) == kIdError) SetTextColor(dc, RGB(196, 30, 30));
      return reinterpret_cast<LRESULT>(g.bg);
    }
    case WM_DISPLAYCHANGE:
      if (IsWindowVisible(hwnd)) FillMonitors();
      return 0;
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
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);
  g.inst = inst;
  HDC dc = GetDC(nullptr);
  g.dpi = GetDeviceCaps(dc, LOGPIXELSY);
  ReleaseDC(nullptr, dc);

  NONCLIENTMETRICSW ncm{};
  ncm.cbSize = sizeof(ncm);
  SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
  g.font = CreateFontIndirectW(&ncm.lfMessageFont);
  LOGFONTW title = ncm.lfMessageFont;
  title.lfHeight = title.lfHeight * 13 / 10;
  title.lfWeight = FW_SEMIBOLD;
  g.title_font = CreateFontIndirectW(&title);
  g.mono_font = CreateFontW(-MulDiv(9, g.dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
  g.bg = GetSysColorBrush(COLOR_WINDOW);

  const COLORREF colors[4] = {RGB(107, 114, 128), RGB(217, 119, 6), RGB(22, 163, 74), RGB(220, 38, 38)};
  for (int i = 0; i < 4; ++i) {
    g.tray_icons[i] = CreateTabletIcon(colors[i], GetSystemMetrics(SM_CXSMICON));
    g.status_icons[i] = CreateTabletIcon(colors[i], S(32));
  }
  g.app_icon = LoadIconW(inst, L"APP");
  if (!g.app_icon) g.app_icon = CreateTabletIcon(RGB(37, 99, 235), GetSystemMetrics(SM_CXICON));
  g.app_icon_small = static_cast<HICON>(LoadImageW(inst, L"APP", IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                   GetSystemMetrics(SM_CYSMICON), 0));
  if (!g.app_icon_small) g.app_icon_small = CreateTabletIcon(RGB(37, 99, 235), GetSystemMetrics(SM_CXSMICON));

  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = MainProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = g.bg;
  wc.hIcon = g.app_icon;
  wc.hIconSm = g.app_icon_small;
  wc.lpszClassName = kMainClass;
  RegisterClassExW(&wc);
  wc.lpfnWndProc = LogProc;
  wc.lpszClassName = kLogClass;
  RegisterClassExW(&wc);
  g.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT r{0, 0, S(480), S(500)};
  AdjustWindowRectEx(&r, style, FALSE, WS_EX_CONTROLPARENT);
  RECT work;
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int w = r.right - r.left, h = r.bottom - r.top;
  g.wnd = CreateWindowExW(WS_EX_CONTROLPARENT, kMainClass, kAppName, style, work.left + (work.right - work.left - w) / 2,
                          work.top + (work.bottom - work.top - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
  if (!g.wnd) return 1;
  CreateControls();

  if (!LoadSettings(&g.settings)) {
    // Primera vez: monitor virtual si el driver está instalado; si no, duplicar el monitor principal.
    g.settings.virtual_display = VirtualDriverInstalled();
    SaveSettings(g.settings);
  }
  FillControls();
  UpdateAddresses();
  TrayCommand(NIM_ADD);
  UpdateStatus();
  SetTimer(g.wnd, kTimerRefresh, 5000, nullptr);
  if (!tray_only) ShowMain();
  if (g.settings.stream_on_open) StartHost();

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (IsDialogMessageW(g.wnd, &msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  ReleaseMutex(mutex);
  return 0;
}
