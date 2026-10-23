#include "i18n.h"

#include <cstring>
#include <cwctype>

namespace {

struct LangInfo {
  const wchar_t* code;
  const wchar_t* name;
};

constexpr LangInfo kLangs[kLangCount] = {
    {L"en", L"English"},
    {L"es", L"Español"},
};

// Cada texto en todos los idiomas, en el orden de Lang. «Dispositivo» y no «tablet»: vale igual un móvil.
struct Entry {
  const char* key;
  const wchar_t* text[kLangCount];
};

constexpr Entry kTexts[] = {
    // Estado (view_model.cpp)
    {"size_auto", {L"Automatic (the device's)", L"Automática (la del dispositivo)"}},
    {"st_stopped", {L"Stopped", L"Detenido"}},
    {"st_stopped_detail", {L"Press Start to use the device as a screen.", L"Pulsa Iniciar para usar el dispositivo como pantalla."}},
    {"st_starting", {L"Starting…", L"Iniciando…"}},
    {"st_starting_virtual", {L"Creating the extra screen…", L"Creando la pantalla extra…"}},
    {"st_starting_mirror", {L"Preparing the capture…", L"Preparando la captura…"}},
    {"st_reconnecting", {L"Reconnecting…", L"Reconectando…"}},
    {"st_reconnecting_detail",
     {L"The connection dropped. The extra screen stays {1} s longer in case the device comes back.",
      L"Se cortó la conexión. La pantalla extra se queda {1} s más por si el dispositivo vuelve."}},
    {"st_waiting", {L"Waiting for the device", L"Esperando al dispositivo"}},
    {"st_waiting_detail",
     {L"Open Extendesk on the device or plug it in with a cable. The extra screen appears when it connects.",
      L"Abre Extendesk en el dispositivo o enchúfalo por cable. La pantalla extra aparece al conectar."}},
    {"st_usb", {L"Connected by USB cable", L"Conectado por cable USB"}},
    {"st_usb_detail", {L"The most stable connection.", L"La conexión más estable."}},
    {"st_wifi", {L"Connected over Wi-Fi", L"Conectado por Wi-Fi"}},
    {"st_wifi_detail", {L"Device {1}.", L"Dispositivo {1}."}},
    {"st_stopping", {L"Stopping…", L"Deteniendo…"}},
    {"st_stopping_detail", {L"Removing the extra screen and the USB tunnel…", L"Quitando la pantalla extra y el túnel USB…"}},
    {"st_restarting", {L"Restarting…", L"Reiniciando…"}},
    {"st_restarting_detail", {L"The stream closed; starting it again.", L"Se cerró la transmisión; vuelvo a arrancarla."}},
    {"st_error", {L"Couldn't stream", L"No se pudo transmitir"}},
    {"st_error_detail", {L"Fix this and press Start:", L"Corrige esto y pulsa Iniciar:"}},
    {"still_image", {L"Still image", L"Imagen quieta"}},
    {"notice_wifi", {L"Quality lowered for a moment: the Wi-Fi is struggling.", L"Calidad reducida un momento: el Wi-Fi va justo."}},
    {"notice_battery",
     {L"Smoothness lowered: the device's battery can't keep up.", L"Fluidez reducida: la batería del dispositivo no aguanta."}},
    {"notice_load", {L"Smoothness lowered: the PC's graphics card is busy.", L"Fluidez reducida: la gráfica del PC va cargada."}},
    {"usb_on_start", {L"USB cable: turns on when you press Start.", L"Cable USB: se activa al iniciar."}},
    {"usb_no_adb", {L"USB cable: adb.exe is missing next to extendesk.exe.", L"Cable USB: falta adb.exe junto a extendesk.exe."}},
    {"usb_accept",
     {L"USB cable: accept \"Allow USB debugging?\" on the device.",
      L"Cable USB: acepta «¿Permitir depuración USB?» en el dispositivo."}},
    {"usb_ready", {L"USB cable: device plugged in and ready.", L"Cable USB: dispositivo enchufado y listo."}},
    {"usb_plug",
     {L"USB cable: plug in the device and follow the prompt on it.",
      L"Cable USB: enchufa el dispositivo y sigue el aviso que sale en él."}},
    {"wifi_no_network", {L"Wi-Fi: this PC has no network.", L"Wi-Fi: este PC no tiene red."}},
    {"wifi_same",
     {L"Wi-Fi: same network as the device (this PC's IP: {1}).", L"Wi-Fi: misma red que el dispositivo (IP de este PC: {1})."}},
    {"start", {L"Start", L"Iniciar"}},
    {"stop", {L"Stop", L"Detener"}},

    // Opciones a la vista
    {"mode", {L"Mode", L"Modo"}},
    {"mode_extend", {L"Extra screen", L"Pantalla extra"}},
    {"mode_mirror", {L"Mirror", L"Duplicar"}},
    {"mode_extend_hint", {L"The device is one more monitor, sized to fit it.", L"El dispositivo es un monitor más, a su medida."}},
    {"mode_mirror_hint",
     {L"The device shows the same as one of the PC's monitors.", L"El dispositivo muestra lo mismo que un monitor del PC."}},
    {"driver_missing", {L"The extra screen driver is missing", L"Falta el driver de la pantalla extra"}},
    {"driver_installing", {L"Installing the extra screen…", L"Instalando la pantalla extra…"}},
    {"driver_install", {L"Install the extra screen", L"Instalar la pantalla extra"}},
    {"resolution", {L"Resolution", L"Resolución"}},
    {"resolution_change", {L"Change in Windows", L"Cambiar en Windows"}},
    {"resolution_hint",
     {L"Changed in Windows with the device connected. To make everything bigger, raise \"Scale\": it looks sharper.",
      L"Se cambia en Windows con el dispositivo conectado. Para ver todo más grande, sube la «Escala»: se ve más nítido."}},
    {"monitor", {L"Monitor", L"Monitor"}},
    {"monitor_missing", {L"{1}: (not connected)", L"{1}: (no conectado)"}},
    {"quality", {L"Quality", L"Calidad"}},
    {"q_high", {L"High", L"Alta"}},
    {"q_balanced", {L"Balanced", L"Equilibrada"}},
    {"q_saver", {L"Saver", L"Ahorro"}},
    {"q_high_hint", {L"The smoothest, sharpest picture. Almost always the best.", L"La imagen más fluida y nítida. Casi siempre, la mejor."}},
    {"q_balanced_hint", {L"Up to 60 fps: for Wi-Fi that stutters.", L"Hasta 60 fps: para un Wi-Fi que da tirones."}},
    {"q_saver_hint", {L"30 fps: the device's battery lasts longer.", L"30 fps: dura más la batería del dispositivo."}},
    {"sound", {L"Sound", L"Sonido"}},
    {"sound_pc", {L"On the PC", L"En el PC"}},
    {"sound_device", {L"On the device", L"En el dispositivo"}},
    {"sound_hint", {L"Where the PC's sound plays while connected.", L"Dónde suena el PC mientras está conectado."}},

    // Configuración avanzada
    {"advanced", {L"Advanced settings", L"Configuración avanzada"}},
    {"touch", {L"Touch", L"Táctil"}},
    {"touch_on", {L"Touch", L"Táctil"}},
    {"touch_view", {L"View only", L"Solo ver"}},
    {"touch_on_hint", {L"Tap and type like on a touchscreen.", L"Toca y escribe como en una pantalla táctil."}},
    {"touch_view_hint", {L"Touches don't reach the PC: view only.", L"Los toques no llegan al PC: solo se mira."}},
    {"direct", {L"The PC's own Wi-Fi network", L"Red Wi-Fi propia del PC"}},
    {"direct_busy", {L"Changing…", L"Cambiando…"}},
    {"direct_on_hint",
     {L"Connect the device to the network \"{1}\", password \"{2}\". The video goes straight there, without the router.",
      L"Conecta el dispositivo a la red «{1}», contraseña «{2}». El vídeo va directo, sin pasar por el router."}},
    {"direct_off_hint",
     {L"No router in between: less delay and fewer stutters over Wi-Fi. The PC needs Wi-Fi.",
      L"Sin router de por medio: menos retardo y menos tirones por Wi-Fi. Hace falta Wi-Fi en el PC."}},
    {"autostart", {L"Open Extendesk when Windows starts", L"Abrir Extendesk al iniciar Windows"}},
    {"autostart_hint", {L"So the device connects without touching the PC.", L"Así el dispositivo conecta sin tocar el PC."}},
    {"language", {L"Language", L"Idioma"}},
    {"language_auto", {L"Automatic", L"Automático"}},
    {"language_hint", {L"Automatic uses the Windows language.", L"Automático usa el idioma de Windows."}},
    {"help_link", {L"Help: every option explained", L"Ayuda: cada opción explicada"}},
    {"view_log", {L"View log", L"Ver registro"}},

    // Ventana, avisos y menú (main.cpp)
    {"tagline", {L"Your device as a second screen", L"Tu dispositivo como segunda pantalla"}},
    {"apply", {L"Apply changes (restarts the stream)", L"Aplicar cambios (reinicia la transmisión)"}},
    {"log_title", {L"Extendesk: log", L"Extendesk: registro"}},
    {"copy_all", {L"Copy all", L"Copiar todo"}},
    {"err_driver_missing",
     {L"The extra screen driver is missing. Press \"Install the extra screen\" (Windows will ask for permission) or "
      L"choose \"Mirror\".",
      L"Falta el driver de la pantalla extra. Pulsa «Instalar la pantalla extra» (Windows pedirá permiso) o elige "
      L"«Duplicar»."}},
    {"err_driver_open",
     {L"Windows won't open the extra screen driver. Reinstall Extendesk with the latest installer.",
      L"Windows no deja abrir el driver de la pantalla extra. Reinstala Extendesk con el instalador de la última versión."}},
    {"err_not_enabled",
     {L"Windows didn't turn on the extra screen. In Settings > Display, choose \"Extend these displays\".",
      L"Windows no activó la pantalla extra. En Configuración > Pantalla, elige «Extender estas pantallas»."}},
    {"err_driver_create",
     {L"The driver couldn't create the extra screen. Try another resolution (in \"Resolution\") or reinstall the driver.",
      L"El driver no pudo crear la pantalla extra. Prueba otra resolución (en «Resolución») o reinstala el driver."}},
    {"err_port",
     {L"Port {1} is already in use. Is another Extendesk open (for example extendesk-host.exe in a console)? Close it "
      L"and press Start.",
      L"El puerto {1} ya está en uso. ¿Hay otro Extendesk abierto (por ejemplo extendesk-host.exe en una consola)? "
      L"Ciérralo y pulsa Iniciar."}},
    {"err_encoder",
     {L"The graphics card couldn't start the video encoder. Update the graphics card driver and press Start.",
      L"La tarjeta gráfica no pudo iniciar el codificador de vídeo. Actualiza el driver de la tarjeta gráfica y pulsa "
      L"Iniciar."}},
    {"err_no_output",
     {L"That monitor is no longer connected. Choose another one in \"Monitor\".",
      L"Ese monitor ya no está conectado. Elige otro en «Monitor»."}},
    {"err_capture",
     {L"Windows won't let the screen be captured right now (locked screen, a UAC prompt or a full-screen game).",
      L"Windows no deja capturar la pantalla ahora mismo (pantalla bloqueada, aviso de UAC o un juego a pantalla "
      L"completa)."}},
    {"err_no_driver_files",
     {L"Can't find the driver next to Extendesk. Reinstall Extendesk with Extendesk-Setup.exe.",
      L"No encuentro el driver junto a Extendesk. Vuelve a instalar Extendesk con Extendesk-Setup.exe."}},
    {"err_uac",
     {L"To install the extra screen, accept the Windows permission prompt. Press the button again.",
      L"Para instalar la pantalla extra hace falta aceptar el permiso de Windows. Vuelve a pulsar el botón."}},
    {"err_install_launch",
     {L"Couldn't start installing the extra screen.", L"No se pudo lanzar la instalación de la pantalla extra."}},
    {"err_installed_not_started",
     {L"The driver was installed but Windows didn't start the extra screen. Restart the PC and press Start.",
      L"El driver se instaló pero Windows no arrancó la pantalla extra. Reinicia el PC y pulsa Iniciar."}},
    {"err_install_failed",
     {L"Couldn't install the extra screen (code {1}). The details are in Advanced settings > View log. Meanwhile you "
      L"can use \"Mirror\".",
      L"No se pudo instalar la pantalla extra (código {1}). El detalle está en Configuración avanzada > Ver registro. "
      L"Mientras tanto puedes usar «Duplicar»."}},
    {"err_host_crash",
     {L"The host closed unexpectedly (code {1}). Open \"View log\" for the details.",
      L"El host se cerró inesperadamente (código {1}). Abre «Ver registro» para el detalle."}},
    {"n_disconnected", {L"Device disconnected", L"Dispositivo desconectado"}},
    {"n_disconnected_body",
     {L"Extendesk keeps waiting for it to connect again.", L"Extendesk sigue esperando a que vuelva a conectarse."}},
    {"n_connected", {L"Device connected", L"Dispositivo conectado"}},
    {"n_connected_usb", {L"By USB cable.", L"Por cable USB."}},
    {"n_connected_wifi", {L"Over Wi-Fi ({1}).", L"Por Wi-Fi ({1})."}},
    {"n_restarting", {L"Extendesk is restarting", L"Extendesk se reinicia"}},
    {"n_failed", {L"Extendesk couldn't stream", L"Extendesk no pudo transmitir"}},
    {"n_still_open", {L"Extendesk is still open", L"Extendesk sigue abierto"}},
    {"n_still_open_body",
     {L"It's next to the clock. To close it: right-click the icon > Exit.",
      L"Está junto al reloj. Para cerrarlo: clic derecho en el icono > Salir."}},
    {"menu_open", {L"Open Extendesk", L"Abrir Extendesk"}},
    {"menu_start", {L"Start streaming", L"Iniciar la transmisión"}},
    {"menu_stop", {L"Stop streaming", L"Detener la transmisión"}},
    {"menu_mirror", {L"Mirror a monitor", L"Duplicar un monitor"}},
    {"menu_autostart", {L"Open when Windows starts", L"Abrir al iniciar Windows"}},
    {"menu_exit", {L"Exit", L"Salir"}},
};

Lang g_lang = Lang::kEn;

std::wstring Lower(std::wstring s) {
  for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
  return s;
}

}  // namespace

const wchar_t* LangCode(Lang lang) { return kLangs[static_cast<int>(lang)].code; }
const wchar_t* LangName(Lang lang) { return kLangs[static_cast<int>(lang)].name; }

bool IsLangCode(const std::wstring& code) {
  for (const auto& l : kLangs) {
    if (code == l.code) return true;
  }
  return false;
}

Lang LangFromCode(const std::wstring& code) {
  const std::wstring lower = Lower(code);
  const std::wstring base = lower.substr(0, lower.find_first_of(L"-_"));
  for (int i = 0; i < kLangCount; ++i) {
    if (base == kLangs[i].code) return static_cast<Lang>(i);
  }
  return Lang::kEn;
}

Lang ResolveLang(const std::wstring& setting, const std::wstring& system) {
  return LangFromCode(setting.empty() ? system : setting);
}

void SetLang(Lang lang) { g_lang = lang; }
Lang CurrentLang() { return g_lang; }

std::wstring Tr(const char* key, std::initializer_list<std::wstring> args) {
  std::wstring text;
  for (const auto& e : kTexts) {
    if (std::strcmp(e.key, key) != 0) continue;
    const wchar_t* t = e.text[static_cast<int>(g_lang)];
    text = t && *t ? t : e.text[0];
    break;
  }
  if (text.empty()) text = std::wstring(key, key + std::strlen(key));  // se nota en la ventana y en las pruebas
  int n = 1;
  for (const auto& arg : args) {
    const std::wstring mark = L"{" + std::to_wstring(n++) + L"}";
    for (size_t at = text.find(mark); at != std::wstring::npos; at = text.find(mark, at + arg.size())) {
      text.replace(at, mark.size(), arg);
    }
  }
  return text;
}

wchar_t DecimalSeparator() { return g_lang == Lang::kEn ? L'.' : L','; }

std::vector<std::string> MissingTranslations() {
  std::vector<std::string> missing;
  for (const auto& e : kTexts) {
    for (const wchar_t* t : e.text) {
      if (!t || !*t) {
        missing.push_back(e.key);
        break;
      }
    }
  }
  return missing;
}
