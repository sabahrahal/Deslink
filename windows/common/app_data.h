// Carpeta de ajustes de Extendesk en %APPDATA%, compartida por la app de bandeja y el host.
// Hasta el cambio de nombre era %APPDATA%\DeskLink: la primera vez se renombra con todo lo que tiene dentro.
#pragma once

#include <windows.h>

#include <string>

// Sin barra final; vacía si no hay %APPDATA%. No crea la carpeta.
inline std::wstring AppDataDir() {
  wchar_t base[MAX_PATH];
  if (!GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH)) return {};
  const std::wstring dir = std::wstring(base) + L"\\Extendesk";
  const std::wstring legacy = std::wstring(base) + L"\\DeskLink";
  if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES &&
      GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES && !MoveFileW(legacy.c_str(), dir.c_str()) &&
      GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return legacy;  // no se pudo renombrar (algo la tiene abierta): se sigue usando la antigua hasta la próxima vez
  }
  return dir;
}
