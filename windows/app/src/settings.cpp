#include "settings.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>

namespace {

constexpr wchar_t kSection[] = L"DeskLink";

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

}  // namespace

std::wstring SettingsPath() {
  wchar_t base[MAX_PATH];
  if (!GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH)) return {};
  return std::wstring(base) + L"\\DeskLink\\desklink.ini";
}

bool LoadSettings(Settings* s) {
  const std::wstring path = SettingsPath();
  if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
  s->virtual_display = ReadInt(path, L"virtual", s->virtual_display) != 0;
  s->output = ReadInt(path, L"output", s->output);
  s->size = ReadString(path, L"size");
  s->fps = ReadInt(path, L"fps", s->fps);
  s->bitrate = ReadInt(path, L"bitrate", s->bitrate);
  s->hevc = ReadString(path, L"codec") != L"h264";
  s->touch = ReadInt(path, L"touch", s->touch);
  s->cursor = ReadInt(path, L"cursor", s->cursor) != 0;
  s->usb = ReadInt(path, L"usb", s->usb) != 0;
  s->stream_on_open = ReadInt(path, L"stream_on_open", s->stream_on_open) != 0;
  s->port = ReadInt(path, L"port", s->port);
  s->adb = ReadString(path, L"adb");
  s->extra = ReadString(path, L"extra");
  if (s->fps < 1 || s->fps > 240) s->fps = 60;
  if (s->bitrate < 1 || s->bitrate > 1000) s->bitrate = 50;
  if (s->touch < 0 || s->touch > 2) s->touch = 0;
  if (s->port < 1 || s->port > 65535) s->port = 47800;
  unsigned w, h;
  if (!s->size.empty() && !ParseSize(s->size, &w, &h)) s->size.clear();
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
  WriteInt(path, L"virtual", s.virtual_display);
  WriteInt(path, L"output", s.output);
  WriteString(path, L"size", s.size);
  WriteInt(path, L"fps", s.fps);
  WriteInt(path, L"bitrate", s.bitrate);
  WriteString(path, L"codec", s.hevc ? L"hevc" : L"h264");
  WriteInt(path, L"touch", s.touch);
  WriteInt(path, L"cursor", s.cursor);
  WriteInt(path, L"usb", s.usb);
  WriteInt(path, L"stream_on_open", s.stream_on_open);
  WriteInt(path, L"port", s.port);
  WriteString(path, L"adb", s.adb);
  WriteString(path, L"extra", s.extra);
}

bool ParseSize(const std::wstring& text, unsigned* w, unsigned* h) {
  std::wstring t;
  for (wchar_t c : text) {
    if (c == L' ') continue;
    t += (c == L'×' || c == L'X') ? L'x' : c;
  }
  unsigned a = 0, b = 0;
  wchar_t rest = 0;
  if (swscanf(t.c_str(), L"%ux%u%lc", &a, &b, &rest) != 2) return false;
  if (a < 640 || b < 480 || a > 7680 || b > 4320) return false;
  *w = a;
  *h = b;
  return true;
}

std::wstring HostArguments(const Settings& s) {
  std::wstring a = L"--port " + std::to_wstring(s.port) + L" --fps " + std::to_wstring(s.fps) + L" --bitrate " +
                   std::to_wstring(s.bitrate) + L" --codec " + (s.hevc ? L"hevc" : L"h264");
  if (s.virtual_display) {
    a += L" --virtual";
    unsigned w, h;
    if (ParseSize(s.size, &w, &h)) a += L" --size " + std::to_wstring(w) + L"x" + std::to_wstring(h);
  } else {
    a += L" --output " + std::to_wstring(s.output);
  }
  if (s.touch == 1) a += L" --mouse";
  if (s.touch == 2) a += L" --no-touch";
  if (!s.cursor) a += L" --no-cursor";
  if (!s.usb) a += L" --no-usb";
  if (!s.adb.empty()) a += L" --adb \"" + s.adb + L"\"";
  if (!s.extra.empty()) a += L" " + s.extra;
  return a;
}
