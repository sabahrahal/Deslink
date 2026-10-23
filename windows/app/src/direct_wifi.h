#pragma once

#include <string>

// Red Wi-Fi propia del PC (la «zona con cobertura inalámbrica móvil» de Windows) para que la tablet conecte sin pasar
// por el router: un salto de radio en vez de dos y sin competir con el resto de la casa. La tablet entra a esa red
// una vez desde sus ajustes de Wi-Fi (Android la recuerda) y Extendesk encuentra el PC como en cualquier red.
// Las funciones esperan a Windows (hasta unos segundos): se llaman desde otro hilo.
struct DirectWifi {
  bool on = false;
  std::wstring ssid;
  std::wstring passphrase;
  std::wstring error;  // por qué no se pudo (sin Wi-Fi en el PC, sin red para compartir…)
};

DirectWifi QueryDirectWifi();
DirectWifi StartDirectWifi();
DirectWifi StopDirectWifi();
