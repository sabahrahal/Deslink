// extendesk-driver: instala o desinstala el monitor virtual de Extendesk.
//
//   extendesk-driver instalar      (como administrador)
//   extendesk-driver desinstalar   (como administrador)
//   extendesk-driver estado        (0 si el monitor virtual está instalado y funcionando)
//
// Con --sin-pausa no espera a Enter al terminar (lo usa el instalador de Extendesk, que lo lanza sin ventana).
//
// Instalar hace tres cosas: confía en el certificado con el que se firmó el driver (desklink-idd.cer,
// en los almacenes «Entidades de certificación raíz de confianza» y «Editores de confianza» del equipo),
// crea el dispositivo Root\DeskLinkIdd y le instala el driver. Desinstalar lo deshace todo.
#include <windows.h>
#include <cfgmgr32.h>
#include <newdev.h>
#include <setupapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "../include/extendesk_idd.h"

namespace {

std::wstring ExeDir() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring dir(path);
  return dir.substr(0, dir.find_last_of(L"\\/") + 1);
}

void PrintError(const char* what, DWORD error) {
  wchar_t* msg = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                 error, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
  std::printf("Error: %s (0x%08lx) %ls\n", what, error, msg ? msg : L"");
  if (msg) LocalFree(msg);
}

bool ReadFileBytes(const std::wstring& path, std::vector<BYTE>* out) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  GetFileSizeEx(f, &size);
  out->resize(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  const bool ok = ReadFile(f, out->data(), static_cast<DWORD>(out->size()), &read, nullptr) && read == out->size();
  CloseHandle(f);
  return ok;
}

// Certificados del driver: sujeto «Extendesk Driver...» o, los de antes del nombre Extendesk, «DeskLink Driver...».
// Se usan para quitar los de instalaciones anteriores.
void RemoveDriverCerts(const wchar_t* store_name) {
  HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, store_name);
  if (!store) return;
  for (const wchar_t* subject : {L"Extendesk Driver", L"DeskLink Driver"}) {
    PCCERT_CONTEXT cert = nullptr;
    while ((cert = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR_W, subject,
                                              cert)) != nullptr) {
      PCCERT_CONTEXT dup = CertDuplicateCertificateContext(cert);
      CertDeleteCertificateFromStore(dup);  // libera dup
    }
  }
  CertCloseStore(store, 0);
}

bool AddCert(const std::vector<BYTE>& der, const wchar_t* store_name) {
  HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, store_name);
  if (!store) {
    PrintError("no se pudo abrir el almacén de certificados", GetLastError());
    return false;
  }
  const BOOL ok = CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, der.data(), static_cast<DWORD>(der.size()),
                                                   CERT_STORE_ADD_REPLACE_EXISTING, nullptr);
  if (!ok) PrintError("no se pudo añadir el certificado", GetLastError());
  CertCloseStore(store, 0);
  return ok != FALSE;
}

// Recorre los dispositivos (también los que no están conectados) con nuestro id de hardware.
template <typename Fn>
int ForEachDriverDevice(Fn fn) {
  HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
  if (set == INVALID_HANDLE_VALUE) return 0;
  int found = 0;
  SP_DEVINFO_DATA dev{sizeof(dev)};
  for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
    wchar_t ids[1024] = {};
    if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(ids),
                                           sizeof(ids) - 2 * sizeof(wchar_t), nullptr)) {
      continue;
    }
    for (const wchar_t* id = ids; *id; id += wcslen(id) + 1) {
      if (_wcsicmp(id, EXTENDESK_IDD_HARDWARE_ID) == 0) {
        ++found;
        fn(set, &dev);
        break;
      }
    }
  }
  SetupDiDestroyDeviceInfoList(set);
  return found;
}

// Dice si el dispositivo existe y Windows lo tiene arrancado. Si no, el código de problema del Administrador de
// dispositivos (p. ej. 52: firma no aceptada) ayuda a saber por qué.
int Status() {
  int ok = 0;
  const int found = ForEachDriverDevice([&](HDEVINFO, SP_DEVINFO_DATA* dev) {
    ULONG status = 0, problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, dev->DevInst, 0) != CR_SUCCESS) {
      std::printf("Dispositivo presente pero desconectado.\n");
      return;
    }
    if ((status & DN_STARTED) && !(status & DN_HAS_PROBLEM)) {
      ++ok;
      std::printf("Monitor virtual instalado y funcionando.\n");
    } else {
      std::printf("Monitor virtual con problema (código %lu del Administrador de dispositivos).\n", problem);
    }
  });
  if (found == 0) std::printf("El monitor virtual no está instalado.\n");
  return ok > 0 ? 0 : 1;
}

int Install() {
  const std::wstring dir = ExeDir();
  const std::wstring inf = dir + L"desklink-idd.inf";
  const std::wstring cer = dir + L"desklink-idd.cer";

  std::vector<BYTE> der;
  if (!ReadFileBytes(cer, &der)) {
    std::printf("Error: no encuentro desklink-idd.cer junto a este programa.\n");
    return 1;
  }
  if (GetFileAttributesW(inf.c_str()) == INVALID_FILE_ATTRIBUTES) {
    std::printf("Error: no encuentro desklink-idd.inf junto a este programa.\n");
    return 1;
  }

  std::printf("1/3 Confiando en el certificado del driver...\n");
  RemoveDriverCerts(L"Root");
  RemoveDriverCerts(L"TrustedPublisher");
  if (!AddCert(der, L"Root") || !AddCert(der, L"TrustedPublisher")) return 1;

  std::printf("2/3 Creando el dispositivo del monitor virtual...\n");
  if (ForEachDriverDevice([](HDEVINFO, SP_DEVINFO_DATA*) {}) == 0) {
    GUID class_guid;
    wchar_t class_name[MAX_CLASS_NAME_LEN];
    if (!SetupDiGetINFClassW(inf.c_str(), &class_guid, class_name, MAX_CLASS_NAME_LEN, nullptr)) {
      PrintError("no se pudo leer el .inf", GetLastError());
      return 1;
    }
    HDEVINFO set = SetupDiCreateDeviceInfoList(&class_guid, nullptr);
    SP_DEVINFO_DATA dev{sizeof(dev)};
    const wchar_t hwid[] = EXTENDESK_IDD_HARDWARE_ID L"\0";
    bool ok = set != INVALID_HANDLE_VALUE &&
              SetupDiCreateDeviceInfoW(set, class_name, &class_guid, nullptr, nullptr, DICD_GENERATE_ID, &dev) &&
              SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, reinterpret_cast<const BYTE*>(hwid),
                                                sizeof(hwid)) &&
              SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &dev);
    const DWORD error = GetLastError();
    if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
    if (!ok) {
      PrintError("no se pudo crear el dispositivo", error);
      return 1;
    }
  }

  std::printf("3/3 Instalando el driver...\n");
  BOOL reboot = FALSE;
  if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, EXTENDESK_IDD_HARDWARE_ID, inf.c_str(), INSTALLFLAG_FORCE, &reboot)) {
    const DWORD error = GetLastError();
    PrintError("no se pudo instalar el driver", error);
    if (error == static_cast<DWORD>(TRUST_E_NOSIGNATURE) || error == static_cast<DWORD>(ERROR_INVALID_DATA) ||
        error == static_cast<DWORD>(TRUST_E_SUBJECT_NOT_TRUSTED)) {
      std::printf("Windows no acepta la firma del driver. Comprueba que desklink-idd.cer viene del mismo paquete.\n");
    }
    return 1;
  }
  std::printf("\nListo. El monitor virtual aparece cuando arrancas extendesk-host con --virtual.\n");
  if (reboot) std::printf("Windows pide reiniciar para terminar la instalación.\n");
  Status();
  return 0;
}

int Uninstall() {
  std::printf("1/3 Quitando el dispositivo...\n");
  bool reboot_needed = false;
  const int removed = ForEachDriverDevice([&](HDEVINFO set, SP_DEVINFO_DATA* dev) {
    BOOL reboot = FALSE;
    if (!DiUninstallDevice(nullptr, set, dev, 0, &reboot)) {
      PrintError("no se pudo quitar el dispositivo", GetLastError());
    }
    if (reboot) reboot_needed = true;
  });
  std::printf("   %d dispositivo(s) quitado(s).\n", removed);

  std::printf("2/3 Quitando el driver del almacén de Windows...\n");
  const std::wstring inf = ExeDir() + L"desklink-idd.inf";
  BOOL reboot = FALSE;
  if (GetFileAttributesW(inf.c_str()) != INVALID_FILE_ATTRIBUTES &&
      !DiUninstallDriverW(nullptr, inf.c_str(), 0, &reboot)) {
    const DWORD error = GetLastError();
    // El paquete puede no estar ya en el almacén: no es un error.
    if (error != ERROR_NOT_FOUND && error != ERROR_NO_SUCH_DEVINST) PrintError("no se pudo quitar el driver", error);
  }
  if (reboot) reboot_needed = true;

  std::printf("3/3 Quitando el certificado...\n");
  RemoveDriverCerts(L"Root");
  RemoveDriverCerts(L"TrustedPublisher");

  std::printf("\nListo, Extendesk ya no tiene nada instalado en Windows.\n");
  if (reboot_needed) std::printf("Windows pide reiniciar para terminar.\n");
  return 0;
}

int Run(int argc, wchar_t** argv) {
  const std::wstring cmd = argc > 1 ? argv[1] : L"";
  if (cmd == L"estado") return Status();
  if (cmd != L"instalar" && cmd != L"desinstalar") {
    std::printf("Uso: extendesk-driver instalar | desinstalar   (como administrador)\n"
                "     extendesk-driver estado\n");
    return 2;
  }
  if (!IsUserAnAdmin()) {
    std::printf("Hace falta permiso de administrador: abre una consola con «Ejecutar como administrador»\n"
                "o haz doble clic en instalar-driver.bat.\n");
    return 1;
  }
  return cmd == L"instalar" ? Install() : Uninstall();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  const int code = Run(argc, argv);
  const bool no_pause = argc > 2 && _wcsicmp(argv[2], L"--sin-pausa") == 0;
  DWORD pids[2];
  if (!no_pause && GetConsoleProcessList(pids, 2) == 1) {
    std::printf("\nPulsa Enter para salir.\n");
    std::getchar();
  }
  return code;
}
