// La parte de los ajustes que no depende de Windows: se prueba en tests/test_view_model.cpp.
#include <cwchar>

#include "settings.h"

bool ParseSize(const std::wstring& text, unsigned* w, unsigned* h) {
  std::wstring t;
  for (wchar_t c : text) {
    if (c == L' ') continue;
    t += (c == L'×' || c == L'X') ? L'x' : c;
  }
  unsigned a = 0, b = 0;
  wchar_t rest = 0;
  if (std::swscanf(t.c_str(), L"%ux%u%lc", &a, &b, &rest) != 2) return false;
  if (a < 640 || b < 480 || a > 7680 || b > 4320) return false;
  *w = a;
  *h = b;
  return true;
}

void Sanitize(Settings* s) {
  if (s->quality != L"alta" && s->quality != L"equilibrada" && s->quality != L"ahorro") s->quality = L"alta";
  if (s->touch < 0 || s->touch > 2) s->touch = 0;
  if (s->fps < 0 || s->fps > 240) s->fps = 0;
  if (s->bitrate < 0 || s->bitrate > 1000) s->bitrate = 0;
  if (s->codec != L"hevc" && s->codec != L"h264") s->codec.clear();
  if (s->port < 1 || s->port > 65535) s->port = 47800;
  unsigned w, h;
  if (!s->size.empty() && !ParseSize(s->size, &w, &h)) s->size.clear();
}

namespace {

const wchar_t* TouchArg(int touch) { return touch == 1 ? L"raton" : touch == 2 ? L"solo-ver" : L"tactil"; }

std::wstring SizeArg(const Settings& s) {
  unsigned w, h;
  if (ParseSize(s.size, &w, &h)) return std::to_wstring(w) + L"x" + std::to_wstring(h);
  return L"auto";
}

}  // namespace

std::wstring HostArguments(const Settings& s) {
  std::wstring a = L"--port " + std::to_wstring(s.port);
  a += L" --profile " + s.quality;
  a += L" --fps " + (s.fps > 0 ? std::to_wstring(s.fps) : std::wstring(L"auto"));
  a += L" --bitrate " + (s.bitrate > 0 ? std::to_wstring(s.bitrate) : std::wstring(L"auto"));
  a += L" --codec " + (s.codec.empty() ? std::wstring(L"auto") : s.codec);
  if (s.audio_tablet) a += L" --audio-tablet";
  a += s.touch == 1 ? L" --mouse" : s.touch == 2 ? L" --view-only" : L" --touch";
  if (s.battery_adapt) a += L" --battery-adapt";
  if (s.virtual_display) {
    a += L" --virtual";
    const std::wstring size = SizeArg(s);
    if (size != L"auto") a += L" --size " + size;
  } else {
    a += L" --output " + std::to_wstring(s.output);
  }
  if (!s.cursor) a += L" --no-cursor";
  if (!s.usb) a += L" --no-usb";
  if (!s.adb.empty()) a += L" --adb \"" + s.adb + L"\"";
  if (!s.extra.empty()) a += L" " + s.extra;
  return a;
}

std::wstring LiveSettingsLine(const Settings& s) {
  std::wstring l = L"@ajustes calidad=" + s.quality;
  l += L" fps=" + (s.fps > 0 ? std::to_wstring(s.fps) : std::wstring(L"auto"));
  l += L" mbps=" + (s.bitrate > 0 ? std::to_wstring(s.bitrate) : std::wstring(L"auto"));
  l += L" codec=" + (s.codec.empty() ? std::wstring(L"auto") : s.codec);
  l += L" sonido=" + std::wstring(s.audio_tablet ? L"tablet" : L"pc");
  l += L" tactil=" + std::wstring(TouchArg(s.touch));
  l += L" bateria=" + std::wstring(s.battery_adapt ? L"1" : L"0");
  l += L" size=" + SizeArg(s);
  return l;
}

bool NeedsRestart(const Settings& a, const Settings& b) {
  return a.virtual_display != b.virtual_display || (!a.virtual_display && a.output != b.output) ||
         a.cursor != b.cursor || a.usb != b.usb || a.port != b.port || a.adb != b.adb || a.extra != b.extra;
}
