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
    // A la vista, siempre los cuatro y en este orden; nada más.
    CHECK((Ids(v.main) == std::vector<std::string>{"modo", "calidad", "sonido", "tactil"}));
    CHECK(Get(v.main, "calidad").options.size() == 3);
    CHECK(Get(v.main, "tactil").options.size() == 3);
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
  CHECK(v.title == L"Esperando a la tablet");
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
  CHECK(Has(v.advanced, "resolucion") && !Has(v.advanced, "monitor"));
  for (const char* id : {"fps", "mbps", "codec", "bateria", "puntero", "usb", "autostart", "abrir", "registro"}) {
    CHECK(Has(v.advanced, id));
  }
  CHECK(Get(v.advanced, "resolucion").value == L"Automática (la de la tablet)");
  CHECK(Contains(Get(v.advanced, "fps").value, L"Automática"));
  CHECK(!Get(v.advanced, "bateria").on);  // de serie, la batería no baja la calidad

  // Duplicar: se elige el monitor y no hay resolución de pantalla extra.
  in.settings.virtual_display = false;
  in.monitors = {L"1: DELL U2720Q"};
  v = BuildView(in);
  CHECK(Has(v.advanced, "monitor") && !Has(v.advanced, "resolucion"));
  CHECK(Get(v.advanced, "monitor").value == L"1: DELL U2720Q");

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
}

static void TestHostArguments() {
  Settings s;
  CHECK(HostArguments(s) == L"--port 47800 --profile alta --fps auto --bitrate auto --codec auto --touch --virtual");
  CHECK(LiveSettingsLine(s) ==
        L"@ajustes calidad=alta fps=auto mbps=auto codec=auto sonido=pc tactil=tactil bateria=0 size=auto");
  s.quality = L"equilibrada";
  s.audio_tablet = true;
  s.touch = 2;
  s.battery_adapt = true;
  s.fps = 120;
  s.bitrate = 50;
  s.codec = L"hevc";
  s.size = L"2560 × 1600";
  CHECK(HostArguments(s) ==
        L"--port 47800 --profile equilibrada --fps 120 --bitrate 50 --codec hevc --audio-tablet --view-only "
        L"--battery-adapt --virtual --size 2560x1600");
  CHECK(LiveSettingsLine(s) ==
        L"@ajustes calidad=equilibrada fps=120 mbps=50 codec=hevc sonido=tablet tactil=solo-ver bateria=1 size=2560x1600");
  s.virtual_display = false;
  s.output = 1;
  s.cursor = false;
  CHECK(HostArguments(s).find(L"--output 1 --no-cursor") != std::wstring::npos);

  // Calidad, sonido, táctil y demás se aplican en marcha; el modo, el puntero o el USB reinician.
  Settings a, b;
  b.quality = L"ahorro";
  b.audio_tablet = true;
  b.touch = 1;
  b.fps = 60;
  CHECK(!NeedsRestart(a, b));
  b.virtual_display = false;
  CHECK(NeedsRestart(a, b));

  Settings bad;
  bad.quality = L"ultra";
  bad.touch = 7;
  bad.fps = 999;
  bad.codec = L"av1";
  bad.size = L"10x10";
  Sanitize(&bad);
  CHECK(bad.quality == L"alta" && bad.touch == 0 && bad.fps == 0 && bad.codec.empty() && bad.size.empty());
}

int main() {
  TestMainRowsAlwaysVisible();
  TestUsbAndWifiWhileWaiting();
  TestConnectedShowsStatsAndNotice();
  TestErrorAndButtons();
  TestAdvancedRows();
  TestHostArguments();
  std::puts("test_view_model: OK");
  return 0;
}
