#include "view_model.h"

#include <cstdio>
#include <cwchar>

#include "i18n.h"

namespace {

using Stats = std::map<std::wstring, std::wstring>;

std::wstring Stat(const Stats& stats, const wchar_t* key) {
  const auto it = stats.find(key);
  return it == stats.end() ? std::wstring() : it->second;
}

std::wstring Stat(const ViewInput& in, const wchar_t* key) { return Stat(in.stats, key); }

double StatNum(const Stats& stats, const wchar_t* key) {
  const std::wstring v = Stat(stats, key);
  return v.empty() ? 0.0 : std::wcstod(v.c_str(), nullptr);
}

double StatNum(const ViewInput& in, const wchar_t* key) { return StatNum(in.stats, key); }

int StatInt(const ViewInput& in, const wchar_t* key, int def) {
  const std::wstring v = Stat(in, key);
  return v.empty() ? def : static_cast<int>(std::wcstol(v.c_str(), nullptr, 10));
}

std::wstring Fmt(const wchar_t* fmt, double v) {
  wchar_t buf[64];
  std::swprintf(buf, 64, fmt, v);
  std::wstring s = buf;
  for (auto& c : s) {
    if (c == L'.') c = DecimalSeparator();  // «48,0» en español, «48.0» en inglés
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

std::wstring SizeLabel(const std::wstring& size) {
  unsigned w, h;
  if (ParseSize(size, &w, &h)) return std::to_wstring(w) + L" × " + std::to_wstring(h);
  return Tr("size_auto");
}

std::wstring HelpUrl(const std::string& anchor) {
  // La misma ayuda en cada idioma, con las mismas secciones (anchor).
  std::wstring url = L"https://www.extendesk.app/" + std::wstring(LangCode(CurrentLang())) + L"/help";
  if (!anchor.empty()) url += L"#" + std::wstring(anchor.begin(), anchor.end());
  return url;
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
      v.title = Tr("st_stopped");
      v.detail = Tr("st_stopped_detail");
      break;
    case State::kStarting:
      v.title = Tr("st_starting");
      v.detail = Tr(s.virtual_display ? "st_starting_virtual" : "st_starting_mirror");
      v.dot = Tone::kWarning;
      break;
    case State::kWaiting:
      if (const int grace = StatInt(in, L"gracia", 0); grace > 0) {
        // Se cortó sin avisar o se bloqueó: la pantalla extra aguanta un poco para que las ventanas no se muevan.
        v.title = Tr("st_reconnecting");
        v.detail = Tr("st_reconnecting_detail", {std::to_wstring(grace)});
      } else {
        v.title = Tr("st_waiting");
        v.detail = Tr("st_waiting_detail");
      }
      v.dot = Tone::kWarning;
      break;
    case State::kConnected:
      // Por cable es lo mismo por anclaje que por depuración USB: el dispositivo elige solo la mejor vía.
      if (link == L"usb" || link == L"anclaje") {
        v.title = Tr("st_usb");
        v.detail = Tr("st_usb_detail");
      } else {
        v.title = Tr("st_wifi");
        v.detail = Tr("st_wifi_detail", {Stat(in, L"ip")});
      }
      v.dot = Tone::kSuccess;
      break;
    case State::kStopping:
      v.title = Tr("st_stopping");
      v.detail = Tr("st_stopping_detail");
      v.dot = Tone::kWarning;
      break;
    case State::kRestarting:
      v.title = Tr("st_restarting");
      v.detail = Tr("st_restarting_detail");
      v.dot = Tone::kWarning;
      break;
    case State::kError:
      v.title = Tr("st_error");
      v.detail = Tr("st_error_detail");
      v.dot = Tone::kError;
      break;
  }
  if (!in.error.empty() && (in.state == State::kError || in.state == State::kRestarting)) v.error = in.error;

  if (connected) {
    const double fps = StatNum(in, L"codificados");
    v.stats = fps < 1 ? Tr("still_image") : Fmt(L"%.0f fps", fps);
    v.stats += L" · " + Fmt(L"%.1f Mbps", StatNum(in, L"mbps"));
    if (StatNum(in, L"rtt_ms") > 0) v.stats += L" · " + Fmt(L"%.0f ms", StatNum(in, L"rtt_ms"));
    // Lo que el usuario puede entender y le explica por qué baja la imagen. Los fps de cada etapa y el aviso del
    // decodificador del dispositivo van al registro (BuildDiagnostics).
    const std::wstring reduced = Stat(in, L"reducido");
    if (reduced == L"wifi") v.notice = {Tr("notice_wifi"), Tone::kWarning};
    else if (reduced == L"bateria") v.notice = {Tr("notice_battery"), Tone::kWarning};
    else if (reduced == L"carga") v.notice = {Tr("notice_load"), Tone::kWarning};
  }

  // Por dónde puede llegar la tablet: el cable USB y el Wi-Fi, los dos a la vez si los hay.
  if (in.state == State::kWaiting || in.state == State::kStarting || in.state == State::kStopped ||
      in.state == State::kError) {
    if (in.state != State::kWaiting || in.stats.find(L"adb") == in.stats.end()) {
      v.connection.push_back({Tr("usb_on_start"), Tone::kSecondary});
    } else {
      const int ready = StatInt(in, L"adb", 0);
      const int pending = StatInt(in, L"adb_pendientes", 0);
      if (ready < 0) {
        v.connection.push_back({Tr("usb_no_adb"), Tone::kWarning});
      } else if (pending > 0) {
        v.connection.push_back({Tr("usb_accept"), Tone::kWarning});
      } else if (ready > 0) {
        v.connection.push_back({Tr("usb_ready"), Tone::kSuccess});
      } else {
        v.connection.push_back({Tr("usb_plug"), Tone::kSecondary});
      }
    }
    v.connection.push_back(in.addresses.empty()
                               ? Line{Tr("wifi_no_network"), Tone::kWarning}
                               : Line{Tr("wifi_same", {in.addresses}), Tone::kSecondary});
  }

  switch (in.state) {
    case State::kStopped:
    case State::kError:
      v.primary = Tr("start");
      break;
    case State::kStopping:
      v.primary = Tr("st_stopping");
      v.primary_enabled = false;
      break;
    default:
      v.primary = Tr("stop");
      v.primary_stop = true;
      break;
  }
  v.show_apply = in.dirty && in.state != State::kStopped && in.state != State::kError && in.state != State::kStopping;

  // A la vista: lo que casi todo el mundo toca.
  // Cada opción con una línea que dice qué gana el usuario; lo largo, en la ayuda de la web («?»).
  Row mode = Segmented("modo", Tr("mode"), {Tr("mode_extend"), Tr("mode_mirror")}, s.virtual_display ? 0 : 1);
  mode.help = "modo";
  mode.hint = Tr(s.virtual_display ? "mode_extend_hint" : "mode_mirror_hint");
  if (s.virtual_display && !in.driver_installed) {
    mode.hint = Tr("driver_missing");
    mode.hint_tone = Tone::kWarning;
    v.repair = Tr(in.repairing_driver ? "driver_installing" : "driver_install");
  }
  v.main.push_back(mode);
  if (s.virtual_display) {
    // Un solo sitio para la resolución: la de Windows. Extendesk la recuerda para este dispositivo.
    Row resolution;
    resolution.id = "resolucion";
    resolution.control = Control::kLink;
    resolution.label = Tr("resolution");
    const std::wstring size = Stat(in, L"size");
    resolution.value = connected && !size.empty() ? SizeLabel(size) : Tr("resolution_change");
    resolution.hint = Tr("resolution_hint");
    v.main.push_back(resolution);
  } else {
    const std::wstring monitor = s.output < static_cast<int>(in.monitors.size())
                                     ? in.monitors[s.output]
                                     : Tr("monitor_missing", {std::to_wstring(s.output + 1)});
    v.main.push_back(Choice("monitor", Tr("monitor"), monitor));
  }
  // La única perilla de imagen: los fps, la tasa y el códec los elige el host para este dispositivo.
  Row quality = Segmented("calidad", Tr("quality"), {Tr("q_high"), Tr("q_balanced"), Tr("q_saver")}, QualityIndex(s.quality));
  quality.help = "calidad";
  const char* quality_hints[] = {"q_high_hint", "q_balanced_hint", "q_saver_hint"};
  quality.hint = Tr(quality_hints[quality.selected]);
  v.main.push_back(quality);
  Row sound = Segmented("sonido", Tr("sound"), {Tr("sound_pc"), Tr("sound_device")}, s.audio_tablet ? 1 : 0);
  sound.hint = Tr("sound_hint");
  v.main.push_back(sound);

  // Configuración avanzada, plegada: lo que casi nadie cambia.
  v.advanced_open = in.advanced_open;
  if (in.advanced_open) {
    // Sin «Ratón»: en un dispositivo táctil se toca. Si Windows rechaza los toques, el host pasa solo a ratón por debajo.
    Row touch = Segmented("tactil", Tr("touch"), {Tr("touch_on"), Tr("touch_view")}, s.touch == 2 ? 1 : 0);
    touch.hint = Tr(s.touch == 2 ? "touch_view_hint" : "touch_on_hint");
    v.advanced.push_back(touch);
    Row direct = Toggle("wifi_directo", Tr("direct"), in.direct_wifi_on);
    direct.help = "wifi-propia";
    if (in.direct_wifi_busy) {
      direct.hint = Tr("direct_busy");
    } else if (!in.direct_wifi_error.empty()) {
      direct.hint = in.direct_wifi_error;
      direct.hint_tone = Tone::kWarning;
    } else if (in.direct_wifi_on) {
      direct.hint = Tr("direct_on_hint", {in.direct_wifi_ssid, in.direct_wifi_pass});
    } else {
      direct.hint = Tr("direct_off_hint");
    }
    v.advanced.push_back(direct);
    Row autostart = Toggle("autostart", Tr("autostart"), in.autostart);
    autostart.hint = Tr("autostart_hint");
    v.advanced.push_back(autostart);
    // Automático (el de Windows) y cada idioma con su nombre en ese idioma.
    std::vector<std::wstring> langs = {Tr("language_auto")};
    int lang_selected = 0;
    for (int i = 0; i < kLangCount; ++i) {
      langs.push_back(LangName(static_cast<Lang>(i)));
      if (s.language == LangCode(static_cast<Lang>(i))) lang_selected = i + 1;
    }
    Row language = Segmented("idioma", Tr("language"), std::move(langs), lang_selected);
    language.hint = Tr("language_hint");
    v.advanced.push_back(language);
    Row help;
    help.id = "ayuda";
    help.control = Control::kLink;
    help.label = Tr("help_link");
    v.advanced.push_back(help);
    Row log;
    log.id = "registro";
    log.control = Control::kLink;
    log.label = Tr("view_log");
    v.advanced.push_back(log);
  }
  return v;
}

Diagnostics BuildDiagnostics(const std::map<std::wstring, std::wstring>& stats) {
  Diagnostics d;
  const double fps = StatNum(stats, L"codificados");
  const std::wstring tablet = Stat(stats, L"tablet_fps");
  const bool has_tablet = !tablet.empty() && tablet != L"-";  // las versiones viejas del dispositivo no lo cuentan
  // Por dónde se pierden los fps, de punta a punta: lo que da Windows, lo que sale del codificador y lo que muestra
  // el dispositivo.
  if (fps >= 1) {
    d.fluency = Fmt(L"Captura %.0f", StatNum(stats, L"capturados")) + L" · " + Fmt(L"Codificación %.0f", fps);
    if (has_tablet) d.fluency += L" · " + Fmt(L"Dispositivo %.0f", StatNum(stats, L"tablet_fps"));
    d.fluency += L" fps";
  }
  d.stats = Fmt(L"%.1f Mbps", StatNum(stats, L"mbps"));
  if (StatNum(stats, L"rtt_ms") > 0) d.stats += L" · " + Fmt(L"RTT %.0f ms", StatNum(stats, L"rtt_ms"));
  if (!Stat(stats, L"size").empty()) d.stats += L" · " + Stat(stats, L"size");
  if (!Stat(stats, L"codec").empty()) d.stats += L" · " + Stat(stats, L"codec");
  if (Stat(stats, L"reducido") == L"tablet") {
    d.notice = L"Fluidez ajustada a lo que el dispositivo decodifica a tiempo.";
  } else if (fps >= 20 && has_tablet && StatNum(stats, L"tablet_fps") < 0.8 * fps) {
    d.notice = L"El dispositivo muestra menos imágenes de las que recibe: su decodificador no da abasto.";
  }
  return d;
}
