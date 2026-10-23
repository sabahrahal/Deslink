// Comprueba que la ventana del PC enseña cada cosa donde debe (view_model.h) y que los ajustes llegan bien al host.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../src/settings.h"
#include "../src/view_model.h"

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FALLO %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                                        \
    }                                                                      \
  } while (0)

static std::vector<std::string> Ids(const std::vector<Row>& rows) {
  std::vector<std::string> ids;
  for (const auto& r : rows) ids.push_back(r.id);
  return ids;
}

static bool Has(const std::vector<Row>& rows, const char* id) {
  const auto ids = Ids(rows);
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

static const Row& Get(const std::vector<Row>& rows, const char* id) {
  for (const auto& r : rows) {
    if (r.id == id) return r;
  }
  std::fprintf(stderr, "FALLO: no está la fila %s\n", id);
  std::exit(1);
}

static bool Contains(const std::wstring& text, const wchar_t* part) { return text.find(part) != std::wstring::npos; }

static void TestMainRowsAlwaysVisible() {
  for (State st : {State::kStopped, State::kWaiting, State::kConnected, State::kError}) {
    ViewInput in;
    in.state = st;
    const View v = BuildView(in);
    // A la vista y en este orden; nada más. La resolución, arriba junto al modo: es lo que más se cambia.
    CHECK((Ids(v.main) == std::vector<std::string>{"modo", "resolucion", "calidad", "sonido", "tactil"}));
    CHECK(Get(v.main, "calidad").options.size() == 3);
    // Táctil y Solo ver: sin «Ratón».
    CHECK(Get(v.main, "tactil").options.size() == 2);
    // Lo avanzado no aparece plegado.
    CHECK(v.advanced.empty());
  }
}

static void TestUsbAndWifiWhileWaiting() {
  ViewInput in;
  in.state = State::kWaiting;
  in.stats[L"adb"] = L"1";
  in.stats[L"adb_pendientes"] = L"0";
  in.addresses = L"192.168.1.20";
  const View v = BuildView(in);
  CHECK(v.title == L"Esperando al dispositivo");
  // Los dos caminos a la vez: cable USB listo y Wi-Fi con la IP.
  CHECK(v.connection.size() == 2);
  CHECK(Contains(v.connection[0].text, L"Cable USB") && v.connection[0].tone == Tone::kSuccess);
  CHECK(Contains(v.connection[1].text, L"Wi-Fi") && Contains(v.connection[1].text, L"192.168.1.20"));

  // Falta aceptar la depuración USB: aviso en ámbar.
  in.stats[L"adb_pendientes"] = L"1";
  CHECK(BuildView(in).connection[0].tone == Tone::kWarning);
  // Sin red: el Wi-Fi lo dice.
  in.addresses.clear();
  CHECK(BuildView(in).connection[1].tone == Tone::kWarning);
}

static void TestConnectedShowsStatsAndNotice() {
  ViewInput in;
  in.state = State::kConnected;
  in.stats = {{L"enlace", L"usb"}, {L"codificados", L"119.6"}, {L"mbps", L"48.2"}, {L"rtt_ms", L"3.4"},
              {L"size", L"2944x1840"}, {L"reducido", L"no"}};
  View v = BuildView(in);
  CHECK(v.title == L"Conectado por cable USB" && v.dot == Tone::kSuccess);
  CHECK(v.stats == L"120 fps · 48,2 Mbps · 3 ms");
  CHECK(v.notice.text.empty());
  CHECK(v.connection.empty());  // conectado no hace falta
  CHECK(v.primary == L"Detener" && v.primary_stop);

  in.stats[L"enlace"] = L"wifi";
  in.stats[L"ip"] = L"192.168.1.33";
  in.stats[L"reducido"] = L"wifi";
  v = BuildView(in);
  CHECK(v.title == L"Conectado por Wi-Fi" && Contains(v.detail, L"192.168.1.33"));
  CHECK(Contains(v.notice.text, L"Wi-Fi") && v.notice.tone == Tone::kWarning);
  in.stats[L"reducido"] = L"carga";
  CHECK(Contains(BuildView(in).notice.text, L"gráfica"));
  // Que el dispositivo decodifica lento es técnico: no sale en la ventana, va al registro.
  in.stats[L"reducido"] = L"tablet";
  CHECK(BuildView(in).notice.text.empty());
  CHECK(Contains(BuildDiagnostics(in.stats).notice, L"dispositivo"));

  // El anclaje USB de la tablet no es Wi-Fi: va por el cable, como red.
  in.stats[L"enlace"] = L"anclaje";
  in.stats[L"reducido"] = L"no";
  v = BuildView(in);
  CHECK(v.title == L"Conectado por cable USB (anclaje de red)" && Contains(v.detail, L"depuración USB"));
  // La resolución no se repite en el texto del estado: está en su fila, a la vista.
  CHECK(!Contains(v.detail, L"2944"));
  CHECK(Get(v.main, "resolucion").value == L"2944 × 1840");
}

static void TestDiagnosticsGoToTheLog() {
  ViewInput in;
  in.state = State::kConnected;
  in.stats = {{L"enlace", L"usb"}, {L"capturados", L"60.2"}, {L"codificados", L"59.8"}, {L"mbps", L"40"},
              {L"rtt_ms", L"3.4"}, {L"size", L"2944x1840"}, {L"codec", L"hevc"}, {L"reducido", L"no"},
              {L"tablet_fps", L"58"}};
  Diagnostics d = BuildDiagnostics(in.stats);
  CHECK(d.fluency == L"Captura 60 · Codificación 60 · Dispositivo 58 fps");
  CHECK(d.stats == L"40,0 Mbps · RTT 3 ms · 2944x1840 · hevc");
  CHECK(d.notice.empty());
  // La tablet se queda atrás: se dice dónde, en el registro y no en la ventana.
  in.stats[L"tablet_fps"] = L"21";
  d = BuildDiagnostics(in.stats);
  CHECK(Contains(d.notice, L"decodificador"));
  CHECK(BuildView(in).notice.text.empty());
  // Tablet antigua (no lo cuenta) o imagen quieta: sin cifra de la tablet, sin aviso, o sin línea.
  in.stats[L"tablet_fps"] = L"-";
  d = BuildDiagnostics(in.stats);
  CHECK(d.fluency == L"Captura 60 · Codificación 60 fps" && d.notice.empty());
  in.stats[L"codificados"] = L"0";
  CHECK(BuildDiagnostics(in.stats).fluency.empty());
}

static void TestErrorAndButtons() {
  ViewInput in;
  in.state = State::kError;
  in.error = L"Falta el driver de la pantalla extra.";
  View v = BuildView(in);
  CHECK(v.error == in.error && v.dot == Tone::kError && v.primary == L"Iniciar" && !v.primary_stop);
  in.state = State::kStopping;
  v = BuildView(in);
  CHECK(!v.primary_enabled && v.error.empty());
  // «Aplicar cambios» solo con cambios que piden reiniciar y la transmisión en marcha.
  in.state = State::kConnected;
  in.dirty = true;
  CHECK(BuildView(in).show_apply);
  in.state = State::kStopped;
  CHECK(!BuildView(in).show_apply);
}

static void TestAdvancedRows() {
  ViewInput in;
  in.advanced_open = true;
  in.settings.virtual_display = true;
  View v = BuildView(in);
  // La resolución y el monitor ya no están aquí: van a la vista.
  CHECK(!Has(v.advanced, "resolucion") && !Has(v.advanced, "monitor"));
  for (const char* id :
       {"fps", "mbps", "codec", "bateria", "usb", "abrir_tablet", "autostart", "abrir", "registro"}) {
    CHECK(Has(v.advanced, id));
  }
  // El puntero se dibuja siempre: ya no es una opción.
  CHECK(!Has(v.advanced, "puntero"));
  // La resolución solo se cambia en Windows: es un enlace, no un desplegable con otro valor que se pise.
  CHECK(Get(v.main, "resolucion").control == Control::kLink);
  CHECK(Contains(Get(v.main, "resolucion").value, L"Windows"));  // sin conectar, no hay resolución que enseñar
  // El consejo de la escala va junto al enlace: más grande sin perder nitidez.
  CHECK(Contains(Get(v.main, "resolucion").hint, L"Escala"));
  CHECK(Has(v.advanced, "wifi_directo") && !Get(v.advanced, "wifi_directo").on);
  {
    ViewInput w = in;
    w.direct_wifi_on = true;
    w.direct_wifi_ssid = L"PC-Sabah";
    w.direct_wifi_pass = L"clave1234";
    const Row r = Get(BuildView(w).advanced, "wifi_directo");
    CHECK(r.on && Contains(r.hint, L"PC-Sabah") && Contains(r.hint, L"clave1234"));
    w.direct_wifi_on = false;
    w.direct_wifi_error = L"Este PC no puede crear su propia red Wi-Fi";
    CHECK(Get(BuildView(w).advanced, "wifi_directo").hint_tone == Tone::kWarning);
  }
  CHECK(Get(v.advanced, "abrir_tablet").on);  // de serie, la app de la tablet se abre al enchufarla
  in.settings.usb = false;
  CHECK(!Has(BuildView(in).advanced, "abrir_tablet"));  // sin cable no tiene sentido
  in.settings.usb = true;
  CHECK(Contains(Get(v.advanced, "fps").value, L"Automática"));
  CHECK(!Get(v.advanced, "bateria").on);  // de serie, la batería no baja la calidad

  // Duplicar: se elige el monitor y no hay resolución de pantalla extra.
  in.settings.virtual_display = false;
  in.monitors = {L"1: DELL U2720Q"};
  v = BuildView(in);
  CHECK(Has(v.main, "monitor") && !Has(v.main, "resolucion") && !Has(v.advanced, "monitor"));
  CHECK(Get(v.main, "monitor").value == L"1: DELL U2720Q");
  CHECK(Ids(v.main)[1] == "monitor");

  in.settings.fps = 90;
  in.settings.bitrate = 50;
  in.settings.codec = L"h264";
  v = BuildView(in);
  CHECK(Get(v.advanced, "fps").value == L"90 fps");
  CHECK(Get(v.advanced, "mbps").value == L"50 Mbps");
  CHECK(Get(v.advanced, "codec").value == L"H.264");

  // Sin driver con pantalla extra: el aviso va en el modo, a la vista.
  in.settings.virtual_display = true;
  in.driver_installed = false;
  CHECK(Get(BuildView(in).main, "modo").hint_tone == Tone::kWarning);
  // …y un botón para instalarlo desde la app, que solo sale cuando hace falta.
  CHECK(BuildView(in).repair == L"Instalar la pantalla extra");
  in.repairing_driver = true;
  CHECK(BuildView(in).repair == L"Instalando la pantalla extra…");
  in.repairing_driver = false;
  in.settings.virtual_display = false;
  CHECK(BuildView(in).repair.empty());
  in.settings.virtual_display = true;
  in.driver_installed = true;
  CHECK(BuildView(in).repair.empty());
}

static void TestHostArguments() {
  Settings s;
  CHECK(HostArguments(s) == L"--port 47800 --profile alta --fps auto --bitrate auto --codec auto --touch --virtual");
  CHECK(LiveSettingsLine(s) ==
        L"@ajustes calidad=alta fps=auto mbps=auto codec=auto sonido=pc tactil=tactil bateria=0 abrir_tablet=1");
  s.quality = L"equilibrada";
  s.audio_tablet = true;
  s.touch = 2;
  s.battery_adapt = true;
  s.fps = 120;
  s.bitrate = 50;
  s.codec = L"hevc";
  s.size = L"2560 × 1600";  // de un .ini viejo: ya no se pasa, la resolución la decide Windows
  s.open_tablet = false;
  CHECK(HostArguments(s) ==
        L"--port 47800 --profile equilibrada --fps 120 --bitrate 50 --codec hevc --audio-tablet --view-only "
        L"--battery-adapt --virtual --no-open-tablet");
  CHECK(LiveSettingsLine(s) ==
        L"@ajustes calidad=equilibrada fps=120 mbps=50 codec=hevc sonido=tablet tactil=solo-ver bateria=1 abrir_tablet=0");
  s.virtual_display = false;
  s.output = 1;
  CHECK(HostArguments(s).find(L"--output 1") != std::wstring::npos);
  CHECK(HostArguments(s).find(L"--no-cursor") == std::wstring::npos);

  // Calidad, sonido, táctil y demás se aplican en marcha; el modo o el USB reinician.
  Settings a, b;
  b.quality = L"ahorro";
  b.audio_tablet = true;
  b.touch = 2;
  b.fps = 60;
  b.open_tablet = false;
  CHECK(!NeedsRestart(a, b));
  b.virtual_display = false;
  CHECK(NeedsRestart(a, b));

  Settings bad;
  bad.quality = L"ultra";
  bad.touch = 7;
  bad.fps = 999;
  bad.codec = L"av1";
  bad.size = L"10x10";
  bad.window_w = 120;  // tamaño de ventana imposible: vuelve al del contenido
  bad.window_h = 900;
  Sanitize(&bad);
  CHECK(bad.window_w == 0 && bad.window_h == 0);
  Settings sized;
  sized.window_w = 720;
  sized.window_h = 860;
  Sanitize(&sized);
  CHECK(sized.window_w == 720 && sized.window_h == 860);
  CHECK(bad.quality == L"alta" && bad.touch == 0 && bad.fps == 0 && bad.codec.empty() && bad.size.empty());
  // «Ratón» (1) de un .ini viejo vuelve a táctil.
  Settings old;
  old.touch = 1;
  Sanitize(&old);
  CHECK(old.touch == 0);
}

int main() {
  TestMainRowsAlwaysVisible();
  TestUsbAndWifiWhileWaiting();
  TestConnectedShowsStatsAndNotice();
  TestDiagnosticsGoToTheLog();
  TestErrorAndButtons();
  TestAdvancedRows();
  TestHostArguments();
  std::puts("test_view_model: OK");
  return 0;
}
