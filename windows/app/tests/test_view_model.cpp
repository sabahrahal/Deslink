// Comprueba que la ventana del PC enseña cada cosa donde debe (view_model.h) y que los ajustes llegan bien al host.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../src/i18n.h"
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
    CHECK((Ids(v.main) == std::vector<std::string>{"modo", "resolucion", "calidad", "sonido"}));
    CHECK(Get(v.main, "calidad").options.size() == 3);
    // Cada opción a la vista explica qué hace, y las que tienen más que contar llevan su «?» a la ayuda.
    for (const auto& r : v.main) CHECK(!r.hint.empty());
    CHECK(Get(v.main, "modo").help == "modo" && Get(v.main, "calidad").help == "calidad");
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
  // Solo lo útil: el cable ya detectado. Las IP de este PC no salen (van al registro).
  CHECK(v.connection.size() == 1);
  CHECK(Contains(v.connection[0].text, L"Cable USB") && v.connection[0].tone == Tone::kSuccess);
  CHECK(!Contains(v.connection[0].text, L"192.168"));
  // Sin dispositivo enchufado no hay nada que decir más allá del texto del estado.
  in.stats[L"adb"] = L"0";
  CHECK(BuildView(in).connection.empty());
  in.stats[L"adb"] = L"1";
  // Se cortó sin avisar: la pantalla extra aguanta unos segundos y la ventana lo cuenta.
  {
    ViewInput g = in;
    g.stats[L"gracia"] = L"12";
    const View gv = BuildView(g);
    CHECK(gv.title == L"Reconectando…" && Contains(gv.detail, L"12 s") && gv.connection.empty());
  }

  // Falta aceptar la depuración USB: aviso en ámbar.
  in.stats[L"adb_pendientes"] = L"1";
  CHECK(BuildView(in).connection[0].tone == Tone::kWarning);
  // Sin red: el Wi-Fi lo dice.
  in.addresses.clear();
  CHECK(BuildView(in).connection[1].tone == Tone::kWarning);
  // Parado, iniciando o con error: nada de cable ni Wi-Fi en la tarjeta.
  for (State st : {State::kStopped, State::kStarting, State::kError}) {
    in.state = st;
    CHECK(BuildView(in).connection.empty());
  }
}

static void TestConnectedShowsStatsAndNotice() {
  ViewInput in;
  in.state = State::kConnected;
  in.stats = {{L"enlace", L"usb"}, {L"codificados", L"119.6"}, {L"mbps", L"48.2"}, {L"rtt_ms", L"3.4"},
              {L"size", L"2944x1840"}, {L"reducido", L"no"}};
  View v = BuildView(in);
  CHECK(v.title == L"Conectado por cable USB" && v.dot == Tone::kSuccess);
  // Sin cifras en la tarjeta: fps, Mbps y retardo van al registro.
  CHECK(!Contains(v.title + v.detail, L"fps") && !Contains(v.title + v.detail, L"Mbps"));
  CHECK(v.notice.text.empty());
  CHECK(v.connection.empty());  // conectado no hace falta
  CHECK(v.primary == L"Detener" && v.primary_stop);

  in.stats[L"enlace"] = L"wifi";
  in.stats[L"ip"] = L"192.168.1.33";
  in.stats[L"reducido"] = L"wifi";
  v = BuildView(in);
  CHECK(v.title == L"Conectado por Wi-Fi" && !Contains(v.detail, L"192.168.1.33"));
  CHECK(Contains(v.notice.text, L"Wi-Fi") && v.notice.tone == Tone::kWarning);
  // La batería: el aviso dice qué hacer. Con la imagen quieta no se avisa (no hay fluidez que perder).
  in.stats[L"reducido"] = L"bateria";
  CHECK(Contains(BuildView(in).notice.text, L"cargador"));
  // Bajar por la batería viene apagado (bajaba a Ahorro y el brillo con el cable del PC) y se enciende en avanzada.
  CHECK(!Settings().battery_adapt);
  in.stats[L"codificados"] = L"0";
  CHECK(BuildView(in).notice.text.empty());
  in.stats[L"codificados"] = L"119.6";
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
  // Por anclaje o por depuración USB es el mismo «cable USB»: nada que elegir ni que activar.
  CHECK(v.title == L"Conectado por cable USB" && !Contains(v.detail, L"depuración") && !Contains(v.detail, L"anclaje"));
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
  // Una cifra de 0 con imagen llegando es que el dispositivo aún no la midió: no es aviso.
  in.stats[L"tablet_fps"] = L"0";
  CHECK(BuildDiagnostics(in.stats).notice.empty());
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
  // Solo lo que casi nadie cambia, en este orden. Fps, Mbps, códec, cable USB, abrir la app de la tablet,
  // transmitir al abrir y el puerto ya no se eligen: son automáticos (o se tocan a mano en el .ini). Bajar por la
  // batería vuelve como opción, apagada de serie.
  CHECK((Ids(v.advanced) ==
         std::vector<std::string>{"tactil", "wifi_directo", "bateria", "autostart", "idioma", "ayuda", "registro"}));
  CHECK(!Get(v.advanced, "bateria").on);
  // Táctil y Solo ver: sin «Ratón».
  CHECK(Get(v.advanced, "tactil").options.size() == 2 && Get(v.advanced, "tactil").selected == 0);
  CHECK(Get(v.advanced, "wifi_directo").help == "wifi-propia");
  CHECK(Get(v.advanced, "ayuda").control == Control::kLink);
  CHECK(HelpUrl() == L"https://www.extendesk.app/es/help");
  CHECK(HelpUrl("calidad") == L"https://www.extendesk.app/es/help#calidad");
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

  // Duplicar: se elige el monitor y no hay resolución de pantalla extra.
  in.settings.virtual_display = false;
  in.monitors = {L"1: DELL U2720Q"};
  v = BuildView(in);
  CHECK(Has(v.main, "monitor") && !Has(v.main, "resolucion") && !Has(v.advanced, "monitor"));
  CHECK(Get(v.main, "monitor").value == L"1: DELL U2720Q");
  CHECK(Ids(v.main)[1] == "monitor");


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

// Inglés y español: el de Windows por defecto, o el elegido en Configuración avanzada.
static void TestLanguages() {
  // Cada texto está en todos los idiomas.
  CHECK(MissingTranslations().empty());
  CHECK(ResolveLang(L"", L"es-ES") == Lang::kEs && ResolveLang(L"", L"es-419") == Lang::kEs);
  CHECK(ResolveLang(L"", L"en-US") == Lang::kEn);
  // Un idioma sin traducir aún (portugués) sale en inglés; lo elegido manda sobre Windows.
  CHECK(ResolveLang(L"", L"pt-BR") == Lang::kEn);
  CHECK(ResolveLang(L"en", L"es-ES") == Lang::kEn && ResolveLang(L"es", L"en-GB") == Lang::kEs);

  SetLang(Lang::kEn);
  ViewInput in;
  in.state = State::kConnected;
  in.stats = {{L"enlace", L"wifi"}, {L"ip", L"192.168.1.33"}, {L"codificados", L"59.6"}, {L"mbps", L"48.2"}};
  View v = BuildView(in);
  CHECK(v.title == L"Connected over Wi-Fi" && Contains(v.detail, L"same network"));
  CHECK(BuildDiagnostics(in.stats).stats == L"48.2 Mbps");  // punto decimal en inglés
  CHECK(v.primary == L"Stop");
  CHECK((Get(v.main, "calidad").options == std::vector<std::wstring>{L"High", L"Balanced", L"Saver"}));
  CHECK(HelpUrl("calidad") == L"https://www.extendesk.app/en/help#calidad");  // las mismas secciones en cada idioma
  in.state = State::kWaiting;
  in.stats.clear();
  CHECK(BuildView(in).title == L"Waiting for the device");
  // «Dispositivo», no «tablet», en todo lo que se ve.
  in.advanced_open = true;
  in.addresses = L"192.168.1.20";
  for (State st : {State::kStopped, State::kWaiting, State::kError}) {
    in.state = st;
    v = BuildView(in);
    std::wstring all = v.title + v.detail;
    for (const auto& l : v.connection) all += l.text;
    for (const auto* rows : {&v.main, &v.advanced}) {
      for (const auto& r : *rows) {
        all += r.label + r.hint + r.value;
        for (const auto& o : r.options) all += o;
      }
    }
    CHECK(!Contains(all, L"ablet"));
  }

  // El selector: Automático y cada idioma con su propio nombre.
  Row lang = Get(v.advanced, "idioma");
  CHECK((lang.options == std::vector<std::wstring>{L"Automatic", L"English", L"Español"}) && lang.selected == 0);
  in.settings.language = L"es";
  SetLang(Lang::kEs);
  lang = Get(BuildView(in).advanced, "idioma");
  CHECK(lang.label == L"Idioma" && lang.options[0] == L"Automático" && lang.selected == 2);

  // Un idioma escrito a mano en el .ini que no existe vuelve a automático.
  Settings bad;
  bad.language = L"klingon";
  Sanitize(&bad);
  CHECK(bad.language.empty());
  Settings ok;
  ok.language = L"en";
  Sanitize(&ok);
  CHECK(ok.language == L"en");
}

static void TestHostArguments() {
  Settings s;
  // Abrir la app de la tablet al enchufarla va siempre; el cable USB también (sin --no-usb). La batería, solo si se
  // enciende.
  CHECK(HostArguments(s) ==
        L"--port 47800 --profile alta --fps auto --bitrate auto --codec auto --touch --virtual");
  CHECK(LiveSettingsLine(s) ==
        L"@ajustes calidad=alta fps=auto mbps=auto codec=auto sonido=pc tactil=tactil bateria=0 abrir_tablet=1");
  s.quality = L"equilibrada";
  s.audio_tablet = true;
  s.touch = 2;
  s.fps = 120;  // fps, Mbps y códec solo desde el .ini
  s.bitrate = 50;
  s.codec = L"hevc";
  s.size = L"2560 × 1600";  // de un .ini viejo: ya no se pasa, la resolución la decide Windows
  s.battery_adapt = true;
  CHECK(HostArguments(s) ==
        L"--port 47800 --profile equilibrada --fps 120 --bitrate 50 --codec hevc --audio-tablet --view-only "
        L"--battery-adapt --virtual");
  CHECK(LiveSettingsLine(s) ==
        L"@ajustes calidad=equilibrada fps=120 mbps=50 codec=hevc sonido=tablet tactil=solo-ver bateria=1 abrir_tablet=1");
  s.virtual_display = false;
  s.output = 1;
  CHECK(HostArguments(s).find(L"--output 1") != std::wstring::npos);
  CHECK(HostArguments(s).find(L"--no-cursor") == std::wstring::npos);

  // Calidad, sonido, táctil y demás se aplican en marcha; el modo o el monitor reinician.
  Settings a, b;
  b.quality = L"ahorro";
  b.audio_tablet = true;
  b.touch = 2;
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
  // Las pruebas de la ventana miran los textos en español; TestLanguages, los de inglés.
  SetLang(Lang::kEs);
  TestMainRowsAlwaysVisible();
  TestUsbAndWifiWhileWaiting();
  TestConnectedShowsStatsAndNotice();
  TestDiagnosticsGoToTheLog();
  TestErrorAndButtons();
  TestAdvancedRows();
  TestHostArguments();
  TestLanguages();
  std::puts("test_view_model: OK");
  return 0;
}
