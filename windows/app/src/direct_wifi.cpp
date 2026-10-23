#include "direct_wifi.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.NetworkOperators.h>

namespace {

using namespace winrt::Windows::Networking::Connectivity;
using namespace winrt::Windows::Networking::NetworkOperators;

// Windows comparte la conexión del PC con su zona con cobertura: hace falta una (la de Internet o, sin Internet,
// cualquier red a la que esté conectado, p. ej. un cable al router).
NetworkOperatorTetheringManager Manager(std::wstring* error) {
  ConnectionProfile profile = NetworkInformation::GetInternetConnectionProfile();
  if (!profile) {
    for (const auto& p : NetworkInformation::GetConnectionProfiles()) {
      if (p.GetNetworkConnectivityLevel() != NetworkConnectivityLevel::None) {
        profile = p;
        break;
      }
    }
  }
  if (!profile) {
    *error = L"El PC tiene que estar conectado a alguna red (por cable o Wi-Fi) para crear la suya.";
    return nullptr;
  }
  if (NetworkOperatorTetheringManager::GetTetheringCapabilityFromConnectionProfile(profile) !=
      TetheringCapability::Enabled) {
    *error = L"Este PC no puede crear su propia red Wi-Fi (sin Wi-Fi, o lo impide su adaptador o la empresa).";
    return nullptr;
  }
  return NetworkOperatorTetheringManager::CreateFromConnectionProfile(profile);
}

DirectWifi Describe(const NetworkOperatorTetheringManager& m) {
  DirectWifi d;
  d.on = m.TetheringOperationalState() == TetheringOperationalState::On;
  const auto cfg = m.GetCurrentAccessPointConfiguration();
  d.ssid = cfg.Ssid().c_str();
  d.passphrase = cfg.Passphrase().c_str();
  return d;
}

std::wstring StatusText(TetheringOperationStatus s) {
  switch (s) {
    case TetheringOperationStatus::WiFiDeviceOff: return L"El Wi-Fi del PC está apagado: enciéndelo y vuelve a probar.";
    case TetheringOperationStatus::OperationInProgress: return L"Windows aún está cambiando la red; prueba en unos segundos.";
    case TetheringOperationStatus::EntitlementCheckFailure:
    case TetheringOperationStatus::EntitlementCheckTimeout: return L"Windows no permite compartir esta conexión.";
    default: return L"Windows no pudo crear la red Wi-Fi del PC (código " + std::to_wstring(static_cast<int>(s)) + L").";
  }
}

template <typename F>
DirectWifi Run(F&& body) {
  DirectWifi d;
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
  } catch (...) {
  }
  try {
    d = body();
  } catch (const winrt::hresult_error& e) {
    d.error = L"Windows no pudo usar la red Wi-Fi del PC: " + std::wstring(e.message().c_str());
  } catch (...) {
    d.error = L"Windows no pudo usar la red Wi-Fi del PC.";
  }
  winrt::uninit_apartment();
  return d;
}

}  // namespace

DirectWifi QueryDirectWifi() {
  return Run([] {
    DirectWifi d;
    auto m = Manager(&d.error);
    return m ? Describe(m) : d;
  });
}

DirectWifi StartDirectWifi() {
  return Run([] {
    DirectWifi d;
    auto m = Manager(&d.error);
    if (!m) return d;
    if (m.TetheringOperationalState() != TetheringOperationalState::On) {
      const auto r = m.StartTetheringAsync().get();
      if (r.Status() != TetheringOperationStatus::Success) {
        d = Describe(m);
        d.error = StatusText(r.Status());
        return d;
      }
    }
    return Describe(m);
  });
}

DirectWifi StopDirectWifi() {
  return Run([] {
    DirectWifi d;
    auto m = Manager(&d.error);
    if (!m) return d;
    if (m.TetheringOperationalState() != TetheringOperationalState::Off) m.StopTetheringAsync().get();
    return Describe(m);
  });
}
