#include "view_model.h"

#include <cstdio>
#include <cwchar>

namespace {

std::wstring Stat(const ViewInput& in, const wchar_t* key) {
  const auto it = in.stats.find(key);
  return it == in.stats.end() ? std::wstring() : it->second;
}

double StatNum(const ViewInput& in, const wchar_t* key) {
  const std::wstring v = Stat(in, key);
  return v.empty() ? 0.0 : std::wcstod(v.c_str(), nullptr);
}

int StatInt(const ViewInput& in, const wchar_t* key, int def) {
  const std::wstring v = Stat(in, key);
  return v.empty() ? def : static_cast<int>(std::wcstol(v.c_str(), nullptr, 10));
}

std::wstring Fmt(const wchar_t* fmt, double v) {
  wchar_t buf[64];
  std::swprintf(buf, 64, fmt, v);
  std::wstring s = buf;
  for (auto& c : s) {
    if (c == L'.') c = L',';  // coma decimal, como escribe el usuario
  }
  return s;
}

Row Segmented(const char* id, const std::wstring& label, std::vector<std::wstring> options, int selected) {
  Row r;
  r.id = id;
  r.control = Control::kSegmented;
  r.label = label;
  r.options = std::move(options);
  r.selected = selected;
  return r;
}

Row Choice(const char* id, const std::wstring& label, const std::wstring& value) {
  Row r;
  r.id = id;
  r.control = Control::kChoice;
  r.label = label;
  r.value = value;
  return r;
}

Row Toggle(const char* id, const std::wstring& label, bool on) {
  Row r;
  r.id = id;
  r.control = Control::kToggle;
  r.label = label;
  r.on = on;
  return r;
}

}  // namespace

const std::vector<int>& FpsChoices() {
  static const std::vector<int> v = {0, 30, 60, 90, 120};
  return v;
}

const std::vector<int>& BitrateChoices() {
  static const std::vector<int> v = {0, 20, 30, 50, 80};
  return v;
}

const std::vector<std::wstring>& SizeChoices() {
  static const std::vector<std::wstring> v = {L"", L"3840x2400", L"2944x1840", L"2560x1600", L"1920x1200", L"1920x1080"};
  return v;
}

const std::vector<std::wstring>& CodecChoices() {
  static const std::vector<std::wstring> v = {L"", L"hevc", L"h264"};
  return v;
}

std::wstring FpsLabel(int fps) {
  return fps > 0 ? std::to_wstring(fps) + L" fps" : L"Automática (lo máximo que aguanten el dispositivo y el PC)";
}

std::wstring BitrateLabel(int mbps) {
  return mbps > 0 ? std::to_wstring(mbps) + L" Mbps" : L"Automática (según resolución y fps)";
}

std::wstring SizeLabel(const std::wstring& size) {
  unsigned w, h;
  if (ParseSize(size, &w, &h)) return std::to_wstring(w) + L" × " + std::to_wstring(h);
  return L"Automática (la del dispositivo)";
}

std::wstring CodecLabel(const std::wstring& codec) {
  if (codec == L"hevc") return L"HEVC";
  if (codec == L"h264") return L"H.264";
  return L"Automático (HEVC si el dispositivo puede)";
}

int QualityIndex(const std::wstring& quality) {
  if (quality == L"equilibrada") return 1;
  if (quality == L"ahorro") return 2;
  return 0;
}

std::wstring QualityAt(int index) {
  return index == 1 ? L"equilibrada" : index == 2 ? L"ahorro" : L"alta";
}

View BuildView(const ViewInput& in) {
  View v;
  const Settings& s = in.settings;
  const std::wstring link = Stat(in, L"enlace");
  const bool connected = in.state == State::kConnected;

  switch (in.state) {
    case State::kStopped:
      v.title = L"Detenido";
      v.detail = L"Pulsa Iniciar para usar el dispositivo como pantalla.";
      break;
    case State::kStarting:
      v.title = L"Iniciando…";
      v.detail = s.virtual_display ? L"Creando la pantalla extra…" : L"Preparando la captura…";
      v.dot = Tone::kWarning;
      break;
    case State::kWaiting:
      v.title = L"Esperando al dispositivo";
      v.detail = L"Abre DeskLink en el dispositivo y pulsa Conectar.";
      v.dot = Tone::kWarning;
      break;
    case State::kConnected:
      if (link == L"usb") {
        v.title = L"Conectado por cable USB";
        v.detail = L"La conexión más rápida.";
      } else if (link == L"anclaje") {
        v.title = L"Conectado por cable USB (anclaje de red)";
        v.detail = L"Funciona, pero con la depuración USB va más fluido y no hay que activar el anclaje cada vez.";
      } else {
        v.title = L"Conectado por Wi-Fi";
        v.detail = L"Tablet " + Stat(in, L"ip") + L".";
      }
      if (!Stat(in, L"size").empty()) v.detail += L" Pantalla de " + Stat(in, L"size") + L".";
      v.dot = Tone::kSuccess;
      break;
    case State::kStopping:
      v.title = L"Deteniendo…";
      v.detail = L"Quitando la pantalla extra y el túnel USB…";
      v.dot = Tone::kWarning;
      break;
    case State::kRestarting:
      v.title = L"Reiniciando…";
      v.detail = L"Se cerró la transmisión; vuelvo a arrancarla.";
      v.dot = Tone::kWarning;
      break;
    case State::kError:
      v.title = L"No se pudo transmitir";
      v.detail = L"Corrige esto y pulsa Iniciar:";
      v.dot = Tone::kError;
      break;
  }
  if (!in.error.empty() && (in.state == State::kError || in.state == State::kRestarting)) v.error = in.error;

  if (connected) {
    const double fps = StatNum(in, L"codificados");
    v.stats = fps < 1 ? std::wstring(L"Imagen quieta") : Fmt(L"%.0f fps", fps);
    v.stats += L" · " + Fmt(L"%.1f Mbps", StatNum(in, L"mbps"));
    if (StatNum(in, L"rtt_ms") > 0) v.stats += L" · " + Fmt(L"%.0f ms", StatNum(in, L"rtt_ms"));
    // Por dónde se pierden los fps, de punta a punta: lo que da Windows, lo que sale del codificador y lo que muestra
    // la tablet (si su versión lo cuenta).
    if (fps >= 1) {
      v.fluency = Fmt(L"Captura %.0f", StatNum(in, L"capturados")) + L" · " + Fmt(L"Codificación %.0f", fps);
      const std::wstring tablet = Stat(in, L"tablet_fps");
      if (!tablet.empty() && tablet != L"-") v.fluency += L" · " + Fmt(L"Dispositivo %.0f", StatNum(in, L"tablet_fps"));
      v.fluency += L" fps";
    }
    const std::wstring reduced = Stat(in, L"reducido");
    if (reduced == L"wifi") v.notice = {L"Calidad reducida un momento: el Wi-Fi va justo.", Tone::kWarning};
    else if (reduced == L"bateria") v.notice = {L"Fluidez reducida: la batería del dispositivo no aguanta.", Tone::kWarning};
    else if (reduced == L"carga") v.notice = {L"Fluidez reducida: la gráfica del PC va cargada.", Tone::kWarning};
    const std::wstring tablet = Stat(in, L"tablet_fps");
    if (v.notice.text.empty() && fps >= 20 && !tablet.empty() && tablet != L"-" &&
        StatNum(in, L"tablet_fps") < 0.8 * fps) {
      v.notice = {L"El dispositivo muestra menos imágenes de las que recibe: su decodificador no da abasto.", Tone::kWarning};
    }
  }

  // Por dónde puede llegar la tablet: el cable USB y el Wi-Fi, los dos a la vez si los hay.
  if (in.state == State::kWaiting || in.state == State::kStarting || in.state == State::kStopped ||
      in.state == State::kError) {
    if (!s.usb) {
      v.connection.push_back({L"Cable USB: desactivado en Configuración avanzada.", Tone::kSecondary});
    } else if (in.state != State::kWaiting || in.stats.find(L"adb") == in.stats.end()) {
      v.connection.push_back({L"Cable USB: se activa al iniciar.", Tone::kSecondary});
    } else {
      const int ready = StatInt(in, L"adb", 0);
      const int pending = StatInt(in, L"adb_pendientes", 0);
      if (ready < 0) {
        v.connection.push_back({L"Cable USB: falta adb.exe junto a desklink.exe.", Tone::kWarning});
      } else if (pending > 0) {
        v.connection.push_back({L"Cable USB: acepta «¿Permitir depuración USB?» en el dispositivo.", Tone::kWarning});
      } else if (ready > 0) {
        v.connection.push_back({L"Cable USB: dispositivo enchufado y listo.", Tone::kSuccess});
      } else {
        v.connection.push_back({L"Cable USB: sin dispositivo enchufado (activa la depuración USB).", Tone::kSecondary});
      }
    }
    v.connection.push_back(in.addresses.empty()
                               ? Line{L"Wi-Fi: este PC no tiene red.", Tone::kWarning}
                               : Line{L"Wi-Fi: misma red que el dispositivo (IP de este PC: " + in.addresses + L").",
                                      Tone::kSecondary});
  }

  switch (in.state) {
    case State::kStopped:
    case State::kError:
      v.primary = L"Iniciar";
      break;
    case State::kStopping:
      v.primary = L"Deteniendo…";
      v.primary_enabled = false;
      break;
    default:
      v.primary = L"Detener";
      v.primary_stop = true;
      break;
  }
  v.show_apply = in.dirty && in.state != State::kStopped && in.state != State::kError && in.state != State::kStopping;

  // A la vista: lo que casi todo el mundo toca.
  Row mode = Segmented("modo", L"Modo", {L"Pantalla extra", L"Duplicar"}, s.virtual_display ? 0 : 1);
  if (s.virtual_display && !in.driver_installed) {
    mode.hint = L"Falta el driver de la pantalla extra";
    mode.hint_tone = Tone::kWarning;
    v.repair = in.repairing_driver ? L"Instalando la pantalla extra…" : L"Instalar la pantalla extra";
  }
  v.main.push_back(mode);
  Row quality = Segmented("calidad", L"Calidad", {L"Alta", L"Equilibrada", L"Ahorro"}, QualityIndex(s.quality));
  const wchar_t* quality_hints[] = {L"La mejor imagen y la máxima fluidez", L"Hasta 60 fps y 30 Mbps",
                                    L"30 fps: gasta menos en el dispositivo"};
  quality.hint = quality_hints[quality.selected];
  v.main.push_back(quality);
  v.main.push_back(Segmented("sonido", L"Sonido", {L"En el PC", L"En el dispositivo"}, s.audio_tablet ? 1 : 0));
  // Sin «Ratón»: en una tablet se toca. Si Windows rechaza los toques, el host pasa solo a ratón por debajo.
  v.main.push_back(Segmented("tactil", L"Táctil", {L"Táctil", L"Solo ver"}, s.touch == 2 ? 1 : 0));

  // Configuración avanzada, plegada.
  v.advanced_open = in.advanced_open;
  if (in.advanced_open) {
    if (s.virtual_display) {
      // Un solo sitio para la resolución: la de Windows. DeskLink la recuerda para esta tablet.
      Row resolution;
      resolution.id = "resolucion";
      resolution.control = Control::kLink;
      resolution.label = L"Cambiar la resolución en Windows";
      resolution.hint = L"Para ver todo más grande, sube la «Escala» en vez de bajar la resolución: se ve más nítido. "
                        L"Cámbialo con el dispositivo conectado y DeskLink lo recuerda.";
      v.advanced.push_back(resolution);
    } else {
      const std::wstring monitor = s.output < static_cast<int>(in.monitors.size())
                                       ? in.monitors[s.output]
                                       : std::to_wstring(s.output + 1) + L": (no conectado)";
      v.advanced.push_back(Choice("monitor", L"Monitor que se duplica", monitor));
    }
    v.advanced.push_back(Choice("fps", L"Fluidez", FpsLabel(s.fps)));
    v.advanced.push_back(Choice("mbps", L"Calidad de vídeo", BitrateLabel(s.bitrate)));
    v.advanced.push_back(Choice("codec", L"Códec", CodecLabel(s.codec)));
    v.advanced.push_back(Toggle("bateria", L"Bajar la calidad si la batería del dispositivo no aguanta", s.battery_adapt));
    v.advanced.push_back(Toggle("usb", L"Conexión por cable USB", s.usb));
    if (s.usb) v.advanced.push_back(Toggle("abrir_tablet", L"Abrir DeskLink en el dispositivo al enchufarlo", s.open_tablet));
    v.advanced.push_back(Toggle("autostart", L"Abrir DeskLink al iniciar Windows", in.autostart));
    v.advanced.push_back(Toggle("abrir", L"Transmitir nada más abrir", s.stream_on_open));
    Row port;
    port.id = "puerto";
    port.label = L"Puerto";
    port.value = L"UDP " + std::to_wstring(s.port) + L" · se cambia en desklink.ini";
    v.advanced.push_back(port);
    Row log;
    log.id = "registro";
    log.control = Control::kLink;
    log.label = L"Ver registro";
    v.advanced.push_back(log);
  }
  return v;
}
