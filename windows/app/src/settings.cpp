#include "settings.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <vector>

#include "../../common/app_data.h"

namespace {

constexpr wchar_t kSection[] = L"Extendesk";
constexpr wchar_t kLegacySection[] = L"DeskLink";
// 3: el PC elige todo (calidad, sonido, táctil, resolución, fps, Mbps, códec). En la 2 lo elegía la tablet.
constexpr int kConfigVersion = 3;

int ReadInt(const std::wstring& path, const wchar_t* key, int def) {
  return static_cast<int>(GetPrivateProfileIntW(kSection, key, def, path.c_str()));
}

std::wstring ReadString(const std::wstring& path, const wchar_t* key) {
  wchar_t buf[1024] = {};
  GetPrivateProfileStringW(kSection, key, L"", buf, 1024, path.c_str());
  return buf;
}

void WriteInt(const std::wstring& path, const wchar_t* key, int v) {
  WritePrivateProfileStringW(kSection, key, std::to_wstring(v).c_str(), path.c_str());
}

void WriteString(const std::wstring& path, const wchar_t* key, const std::wstring& v) {
  WritePrivateProfileStringW(kSection, key, v.c_str(), path.c_str());
}

// Antes del nombre Extendesk el archivo era desklink.ini, con la sección [DeskLink]: se pasa la primera vez.
void MigrateLegacyIni(const std::wstring& path, const std::wstring& legacy_path) {
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES || !MoveFileW(legacy_path.c_str(), path.c_str())) {
    return;
  }
  std::vector<wchar_t> section(32768);
  if (GetPrivateProfileSectionW(kLegacySection, section.data(), static_cast<DWORD>(section.size()), path.c_str()) == 0) {
    return;
  }
  WritePrivateProfileSectionW(kSection, section.data(), path.c_str());
  WritePrivateProfileStringW(kLegacySection, nullptr, nullptr, path.c_str());
}

}  // namespace

std::wstring SettingsPath() {
  const std::wstring dir = AppDataDir();
  if (dir.empty()) return {};
  const std::wstring path = dir + L"\\extendesk.ini";
  MigrateLegacyIni(path, dir + L"\\desklink.ini");
  return path;
}

bool LoadSettings(Settings* s) {
  const std::wstring path = SettingsPath();
  if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
  s->virtual_display = ReadInt(path, L"virtual", s->virtual_display) != 0;
  s->output = ReadInt(path, L"output", s->output);
  const std::wstring quality = ReadString(path, L"quality");
  if (!quality.empty()) s->quality = quality;
  s->audio_tablet = ReadInt(path, L"audio_tablet", s->audio_tablet) != 0;
  s->touch = ReadInt(path, L"touch", s->touch);
  s->size = ReadString(path, L"size");
  s->fps = ReadInt(path, L"fps", s->fps);
  s->bitrate = ReadInt(path, L"bitrate", s->bitrate);
  s->codec = ReadString(path, L"codec");
  s->port = ReadInt(path, L"port", s->port);
  s->adb = ReadString(path, L"adb");
  s->extra = ReadString(path, L"extra");
  s->language = ReadString(path, L"language");
  s->window_w = ReadInt(path, L"window_w", 0);
  s->window_h = ReadInt(path, L"window_h", 0);
  Sanitize(s);
  if (ReadInt(path, L"config", 1) < kConfigVersion) SaveSettings(*s);
  return true;
}

void SaveSettings(const Settings& s) {
  const std::wstring path = SettingsPath();
  if (path.empty()) return;
  CreateDirectoryW(path.substr(0, path.find_last_of(L'\\')).c_str(), nullptr);
  // WritePrivateProfileString solo escribe en Unicode si el archivo ya es UTF-16 (con BOM); si no, rutas con
  // caracteres fuera de la página de códigos se perderían.
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      const wchar_t bom = 0xFEFF;
      DWORD written = 0;
      WriteFile(f, &bom, sizeof(bom), &written, nullptr);
      CloseHandle(f);
    }
  }
  WriteInt(path, L"config", kConfigVersion);
  WriteInt(path, L"virtual", s.virtual_display);
  WriteInt(path, L"output", s.output);
  WriteString(path, L"quality", s.quality);
  WriteInt(path, L"audio_tablet", s.audio_tablet);
  WriteInt(path, L"touch", s.touch);
  WriteString(path, L"size", s.size);
  WriteInt(path, L"fps", s.fps);
  WriteInt(path, L"bitrate", s.bitrate);
  WriteString(path, L"codec", s.codec);
  WriteInt(path, L"port", s.port);
  WriteString(path, L"adb", s.adb);
  WriteString(path, L"extra", s.extra);
  WriteString(path, L"language", s.language);
  WriteInt(path, L"window_w", s.window_w);
  WriteInt(path, L"window_h", s.window_h);
}
