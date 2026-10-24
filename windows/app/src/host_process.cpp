#include "host_process.h"

#include <string>

namespace {

std::wstring FromUtf8(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

struct Reader {
  HANDLE pipe;
  HWND hwnd;
  WPARAM tag;
};

void Post(HWND hwnd, WPARAM tag, std::string line) {
  if (!line.empty() && line.back() == '\r') line.pop_back();
  auto* text = new std::wstring(FromUtf8(line));
  if (!PostMessageW(hwnd, WM_HOST_LINE, tag, reinterpret_cast<LPARAM>(text))) delete text;
}

// Lee la tubería hasta que se cierra y manda cada línea a la ventana.
DWORD WINAPI ReadLines(void* param) {
  const Reader r = *static_cast<Reader*>(param);
  delete static_cast<Reader*>(param);
  std::string buf;
  char chunk[4096];
  DWORD n = 0;
  while (ReadFile(r.pipe, chunk, sizeof(chunk), &n, nullptr) && n > 0) {
    buf.append(chunk, n);
    size_t pos;
    while ((pos = buf.find('\n')) != std::string::npos) {
      Post(r.hwnd, r.tag, buf.substr(0, pos));
      buf.erase(0, pos + 1);
    }
  }
  if (!buf.empty()) Post(r.hwnd, r.tag, buf);
  CloseHandle(r.pipe);
  return 0;
}

HANDLE StartReader(HANDLE pipe, HWND hwnd, WPARAM tag) {
  auto* r = new Reader{pipe, hwnd, tag};
  HANDLE t = CreateThread(nullptr, 0, ReadLines, r, 0, nullptr);
  if (!t) {
    delete r;
    CloseHandle(pipe);
  }
  return t;
}

// Espera al host y luego a que se vacíen sus tuberías, para que WM_HOST_EXIT llegue después de la última línea
// (normalmente el error). Si un hijo del host heredó la tubería y la mantiene abierta (el servidor de adb), la
// lectura se cancela.
struct Watcher {
  HANDLE process;
  HANDLE readers[2];
  HWND hwnd;
  unsigned generation;
};

DWORD WINAPI Watch(void* param) {
  const Watcher w = *static_cast<Watcher*>(param);
  delete static_cast<Watcher*>(param);
  WaitForSingleObject(w.process, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(w.process, &code);
  CloseHandle(w.process);
  for (HANDLE t : w.readers) {
    if (!t) continue;
    if (WaitForSingleObject(t, 500) == WAIT_TIMEOUT) {
      while (WaitForSingleObject(t, 100) == WAIT_TIMEOUT) CancelSynchronousIo(t);
    }
    CloseHandle(t);
  }
  PostMessageW(w.hwnd, WM_HOST_EXIT, w.generation, static_cast<LPARAM>(code));
  return 0;
}

}  // namespace

HostProcess::HostProcess() {
  job_ = CreateJobObjectW(nullptr, nullptr);
  if (job_) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    // Solo el host: lo que él lance (el servidor de adb, que puede estar usando otro programa) queda fuera del job.
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
    SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &info, sizeof(info));
  }
}

HostProcess::~HostProcess() {
  CloseHandles();
  if (job_) CloseHandle(job_);
}

void HostProcess::CloseHandles() {
  if (process_) CloseHandle(process_);
  if (stop_event_) CloseHandle(stop_event_);
  if (stdin_) CloseHandle(stdin_);
  process_ = nullptr;
  stop_event_ = nullptr;
  stdin_ = nullptr;
  pid_ = 0;
}

bool HostProcess::running() const { return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT; }

bool HostProcess::Start(HWND hwnd, const std::wstring& exe, const std::wstring& args, std::wstring* error) {
  if (running()) return true;
  CloseHandles();
  const unsigned gen = ++generation_;

  const std::wstring event_name =
      L"Local\\ExtendeskHostStop-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(gen);
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());

  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr, in_r = nullptr, in_w = nullptr;
  if (!CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&err_r, &err_w, &sa, 0) || !CreatePipe(&in_r, &in_w, &sa, 0)) {
    *error = L"No se pudo crear la tubería para hablar con el host.";
    for (HANDLE h : {out_r, out_w, err_r, err_w, in_r, in_w}) {
      if (h) CloseHandle(h);
    }
    return false;
  }
  SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in_r;
  si.hStdOutput = out_w;
  si.hStdError = err_w;
  std::wstring cmd = L"\"" + exe + L"\" " + args + L" --status --stop-event " + event_name;
  const std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
  PROCESS_INFORMATION pi{};
  const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                 nullptr, dir.c_str(), &si, &pi);
  const DWORD create_error = GetLastError();
  CloseHandle(out_w);
  CloseHandle(err_w);
  CloseHandle(in_r);
  if (!ok) {
    CloseHandle(out_r);
    CloseHandle(err_r);
    CloseHandle(in_w);
    *error = create_error == ERROR_FILE_NOT_FOUND
                 ? L"No encuentro extendesk-host.exe: debe estar en la misma carpeta que extendesk.exe."
                 : L"No se pudo abrir extendesk-host.exe (error " + std::to_wstring(create_error) + L").";
    return false;
  }
  if (job_) AssignProcessToJobObject(job_, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  process_ = pi.hProcess;
  pid_ = pi.dwProcessId;
  stdin_ = in_w;

  auto* w = new Watcher{};
  w->hwnd = hwnd;
  w->generation = gen;
  w->readers[0] = StartReader(out_r, hwnd, gen * 2);
  w->readers[1] = StartReader(err_r, hwnd, gen * 2 + 1);
  DuplicateHandle(GetCurrentProcess(), process_, GetCurrentProcess(), &w->process, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                  FALSE, 0);
  if (HANDLE t = CreateThread(nullptr, 0, Watch, w, 0, nullptr)) {
    CloseHandle(t);
  } else {
    delete w;
  }
  return true;
}

void HostProcess::RequestStop() {
  if (stop_event_) SetEvent(stop_event_);
}

bool HostProcess::Send(const std::wstring& line) {
  if (!stdin_ || !running()) return false;
  std::wstring w = line + L"\n";
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  DWORD written = 0;
  // Las líneas son cortas y el host lee en su propio hilo: la escritura no se queda bloqueada.
  return WriteFile(stdin_, s.data(), static_cast<DWORD>(s.size()), &written, nullptr) && written == s.size();
}

void HostProcess::StopAndWait(DWORD ms) {
  if (!running()) return;
  RequestStop();
  if (WaitForSingleObject(process_, ms) == WAIT_TIMEOUT) TerminateProcess(process_, 1);
}
