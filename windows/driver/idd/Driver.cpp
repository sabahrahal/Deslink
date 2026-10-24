// desklink-idd: driver de pantalla indirecta (IddCx, UMDF) que crea el monitor virtual de Extendesk.
//
// El driver no muestra nada por sí mismo: Windows dibuja el escritorio en el monitor virtual, el driver
// devuelve cada imagen al sistema sin tocarla y el host la captura con Desktop Duplication, igual que
// un monitor físico. El monitor solo existe mientras el host lo pide (IOCTL_EXTENDESK_PLUG) y le manda
// IOCTL_EXTENDESK_PING; si el host se cierra o se cuelga, el monitor desaparece a los pocos segundos.
//
// Basado en el ejemplo IddSampleDriver de Microsoft (Windows-driver-samples, licencia MIT).
#define NOMINMAX
#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <avrt.h>
#include <d3d11_2.h>
#include <dxgi1_5.h>
#include <wrl.h>

#include <initguid.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

#include "../include/extendesk_idd.h"
#include "edid.h"

using Microsoft::WRL::ComPtr;
using extendesk::Mode;

namespace {

void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& s, DWORD width, DWORD height, DWORD vsync, bool monitor_mode) {
  s.totalSize.cx = s.activeSize.cx = width;
  s.totalSize.cy = s.activeSize.cy = height;
  s.AdditionalSignalInfo.vSyncFreqDivider = monitor_mode ? 0 : 1;
  s.AdditionalSignalInfo.videoStandard = 255;
  s.vSyncFreq.Numerator = vsync;
  s.vSyncFreq.Denominator = 1;
  s.hSyncFreq.Numerator = vsync * height;
  s.hSyncFreq.Denominator = 1;
  s.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
  s.pixelRate = static_cast<UINT64>(vsync) * width * height;
}

IDDCX_MONITOR_MODE MakeMonitorMode(const Mode& m, IDDCX_MONITOR_MODE_ORIGIN origin) {
  IDDCX_MONITOR_MODE mode = {};
  mode.Size = sizeof(mode);
  mode.Origin = origin;
  FillSignalInfo(mode.MonitorVideoSignalInfo, m.width, m.height, m.refresh, true);
  return mode;
}

IDDCX_TARGET_MODE MakeTargetMode(const Mode& m) {
  IDDCX_TARGET_MODE mode = {};
  mode.Size = sizeof(mode);
  FillSignalInfo(mode.TargetVideoSignalInfo.targetVideoSignalInfo, m.width, m.height, m.refresh, false);
  return mode;
}

// Frecuencias que ofrece cada resolución. Todas desde el principio: así el host cambia los Hz (p. ej. a 120 fps)
// con un cambio de modo normal, sin quitar el monitor. Al quitarlo Windows lo tomaba por otro y se perdían su
// resolución, su escala y su posición.
constexpr uint32_t kRefreshRates[] = {60, 90, 120, 144};

// Modos que ofrece el monitor: el tamaño pedido primero (el preferido, a 60 Hz) y, por si el usuario quiere menos
// resolución en Configuración de pantalla, algunos estándar más pequeños; cada uno con todas las frecuencias.
std::vector<Mode> ModeList(const Mode& preferred) {
  std::vector<Mode> sizes{{preferred.width, preferred.height, 0}};
  const Mode standard[] = {{2560, 1600, 0}, {2560, 1440, 0}, {1920, 1200, 0}, {1920, 1080, 0},
                           {1680, 1050, 0}, {1280, 800, 0}};
  for (const auto& s : standard) {
    if (s.width < preferred.width && s.height < preferred.height) sizes.push_back(s);
  }
  std::vector<Mode> modes;
  for (const auto& s : sizes) {
    for (uint32_t hz : kRefreshRates) modes.push_back({s.width, s.height, hz});
  }
  return modes;
}

constexpr Mode kDefaultMode{1920, 1080, 60};

}  // namespace

class DeviceContext;
class MonitorContext;

// WDF guarda en cada objeto (dispositivo, adaptador, monitor) un puntero a nuestra clase.
struct DeviceContextWrapper {
  DeviceContext* context;
};
struct MonitorContextWrapper {
  MonitorContext* context;
};
WDF_DECLARE_CONTEXT_TYPE(DeviceContextWrapper);
WDF_DECLARE_CONTEXT_TYPE(MonitorContextWrapper);

// Dispositivo D3D en la GPU que renderiza el escritorio del monitor virtual.
struct Direct3DDevice {
  explicit Direct3DDevice(LUID luid) : adapter_luid(luid) {}
  HRESULT Init() {
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    hr = factory->EnumAdapterByLuid(adapter_luid, IID_PPV_ARGS(&adapter));
    if (FAILED(hr)) return hr;
    return D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                             nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
  }
  LUID adapter_luid;
  ComPtr<IDXGIFactory5> factory;
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
};

// Hilo que recibe cada imagen del monitor virtual y la devuelve al sistema al momento. No copia
// nada: el host captura la imagen por su lado, así que aquí el coste por frame es casi nulo.
class SwapChainProcessor {
 public:
  SwapChainProcessor(IDDCX_SWAPCHAIN swap_chain, std::shared_ptr<Direct3DDevice> device, HANDLE new_frame)
      : swap_chain_(swap_chain), device_(std::move(device)), new_frame_(new_frame) {
    terminate_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
  }
  ~SwapChainProcessor() {
    SetEvent(terminate_);
    if (thread_) {
      WaitForSingleObject(thread_, INFINITE);
      CloseHandle(thread_);
    }
    CloseHandle(terminate_);
  }

 private:
  static DWORD CALLBACK ThreadProc(LPVOID self) {
    static_cast<SwapChainProcessor*>(self)->Run();
    return 0;
  }

  void Run() {
    DWORD task_index = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Distribution", &task_index);
    RunCore();
    // Borrar la cadena al salir hace que el sistema entregue otra nueva si aún hace falta.
    WdfObjectDelete(static_cast<WDFOBJECT>(swap_chain_));
    swap_chain_ = nullptr;
    if (task) AvRevertMmThreadCharacteristics(task);
  }

  void RunCore() {
    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(device_->device.As(&dxgi_device))) return;
    IDARG_IN_SWAPCHAINSETDEVICE set_device = {};
    set_device.pDevice = dxgi_device.Get();
    if (FAILED(IddCxSwapChainSetDevice(swap_chain_, &set_device))) return;

    for (;;) {
      IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = {};
      HRESULT hr = IddCxSwapChainReleaseAndAcquireBuffer(swap_chain_, &buffer);
      if (hr == E_PENDING) {
        HANDLE waits[] = {new_frame_, terminate_};
        const DWORD r = WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE, 16);
        if (r == WAIT_OBJECT_0 || r == WAIT_TIMEOUT) continue;
        break;
      }
      if (FAILED(hr)) break;
      ComPtr<IDXGIResource> surface;
      surface.Attach(buffer.MetaData.pSurface);
      surface.Reset();
      if (FAILED(IddCxSwapChainFinishedProcessingFrame(swap_chain_))) break;
    }
  }

  IDDCX_SWAPCHAIN swap_chain_;
  std::shared_ptr<Direct3DDevice> device_;
  HANDLE new_frame_;
  HANDLE terminate_ = nullptr;
  HANDLE thread_ = nullptr;
};

class MonitorContext {
 public:
  MonitorContext(IDDCX_MONITOR monitor, const Mode& mode) : monitor_(monitor), mode_(mode) {}

  void AssignSwapChain(IDDCX_SWAPCHAIN swap_chain, LUID render_adapter, HANDLE new_frame) {
    processor_.reset();
    auto device = std::make_shared<Direct3DDevice>(render_adapter);
    if (FAILED(device->Init())) {
      WdfObjectDelete(swap_chain);
    } else {
      processor_ = std::make_unique<SwapChainProcessor>(swap_chain, device, new_frame);
    }
  }
  void UnassignSwapChain() { processor_.reset(); }
  const Mode& mode() const { return mode_; }

 private:
  IDDCX_MONITOR monitor_;
  Mode mode_;
  std::unique_ptr<SwapChainProcessor> processor_;
};

class DeviceContext {
 public:
  explicit DeviceContext(WDFDEVICE device) : device_(device) {
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    watchdog_ = CreateThread(nullptr, 0, WatchdogProc, this, 0, nullptr);
  }
  ~DeviceContext() {
    SetEvent(stop_);
    if (watchdog_) {
      WaitForSingleObject(watchdog_, INFINITE);
      CloseHandle(watchdog_);
    }
    CloseHandle(stop_);
  }

  void InitAdapter() {
    IDDCX_ADAPTER_CAPS caps = {};
    caps.Size = sizeof(caps);
    caps.MaxMonitorsSupported = 1;
    caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
    caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
    caps.EndPointDiagnostics.pEndPointFriendlyName = L"Extendesk";
    caps.EndPointDiagnostics.pEndPointManufacturerName = L"Extendesk";
    caps.EndPointDiagnostics.pEndPointModelName = L"Monitor virtual";
    IDDCX_ENDPOINT_VERSION version = {};
    version.Size = sizeof(version);
    version.MajorVer = 1;
    caps.EndPointDiagnostics.pFirmwareVersion = &version;
    caps.EndPointDiagnostics.pHardwareVersion = &version;

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DeviceContextWrapper);
    IDARG_IN_ADAPTER_INIT in = {};
    in.WdfDevice = device_;
    in.pCaps = &caps;
    in.ObjectAttributes = &attr;
    IDARG_OUT_ADAPTER_INIT out = {};
    if (NT_SUCCESS(IddCxAdapterInitAsync(&in, &out))) {
      adapter_ = out.AdapterObject;
      WdfObjectGet_DeviceContextWrapper(out.AdapterObject)->context = this;
    }
  }

  void AdapterReady() {
    std::lock_guard<std::mutex> lock(mutex_);
    adapter_ready_ = true;
  }

  NTSTATUS Plug(const Mode& mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_ping_ms_ = GetTickCount64();
    if (!adapter_ready_) return STATUS_DEVICE_NOT_READY;
    // Solo cuenta el tamaño: los Hz los cambia el host con un cambio de modo (todos están en ModeList).
    if (monitor_ && current_.width == mode.width && current_.height == mode.height) {
      current_.refresh = mode.refresh;
      return STATUS_SUCCESS;
    }
    UnplugLocked();

    // El EDID siempre a 60 Hz: si cambiara con los Hz, Windows lo vería como otro monitor y no recordaría sus ajustes.
    const extendesk::Edid edid = extendesk::BuildEdid({mode.width, mode.height, 60});
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, MonitorContextWrapper);
    attr.EvtCleanupCallback = [](WDFOBJECT object) {
      auto* w = WdfObjectGet_MonitorContextWrapper(object);
      delete w->context;
      w->context = nullptr;
    };

    IDDCX_MONITOR_INFO info = {};
    info.Size = sizeof(info);
    info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    info.ConnectorIndex = 0;
    info.MonitorDescription.Size = sizeof(info.MonitorDescription);
    info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    info.MonitorDescription.DataSize = static_cast<UINT>(edid.size());
    info.MonitorDescription.pData = const_cast<BYTE*>(edid.data());
    // Mismo id de contenedor siempre: Windows recuerda la posición y la escala del monitor entre usos.
    info.MonitorContainerId = GUID_DEVINTERFACE_EXTENDESK_IDD;

    IDARG_IN_MONITORCREATE in = {};
    in.ObjectAttributes = &attr;
    in.pMonitorInfo = &info;
    IDARG_OUT_MONITORCREATE out = {};
    NTSTATUS status = IddCxMonitorCreate(adapter_, &in, &out);
    if (!NT_SUCCESS(status)) return status;
    WdfObjectGet_MonitorContextWrapper(out.MonitorObject)->context = new MonitorContext(out.MonitorObject, mode);

    IDARG_OUT_MONITORARRIVAL arrival = {};
    status = IddCxMonitorArrival(out.MonitorObject, &arrival);
    if (!NT_SUCCESS(status)) {
      WdfObjectDelete(out.MonitorObject);
      return status;
    }
    monitor_ = out.MonitorObject;
    current_ = mode;
    return STATUS_SUCCESS;
  }

  void Unplug() {
    std::lock_guard<std::mutex> lock(mutex_);
    UnplugLocked();
  }

  void Ping() {
    std::lock_guard<std::mutex> lock(mutex_);
    last_ping_ms_ = GetTickCount64();
  }

 private:
  void UnplugLocked() {
    if (!monitor_) return;
    IddCxMonitorDeparture(monitor_);
    monitor_ = nullptr;
  }

  static DWORD CALLBACK WatchdogProc(LPVOID self) {
    auto* ctx = static_cast<DeviceContext*>(self);
    while (WaitForSingleObject(ctx->stop_, 500) == WAIT_TIMEOUT) {
      std::lock_guard<std::mutex> lock(ctx->mutex_);
      if (ctx->monitor_ && GetTickCount64() - ctx->last_ping_ms_ > kExtendeskWatchdogMs) ctx->UnplugLocked();
    }
    return 0;
  }

  WDFDEVICE device_;
  IDDCX_ADAPTER adapter_ = nullptr;
  bool adapter_ready_ = false;
  IDDCX_MONITOR monitor_ = nullptr;
  Mode current_{};
  uint64_t last_ping_ms_ = 0;
  std::mutex mutex_;
  HANDLE stop_ = nullptr;
  HANDLE watchdog_ = nullptr;
};

// ---------------------------------------------------------------------------------------------
// Callbacks de WDF e IddCx
// ---------------------------------------------------------------------------------------------

extern "C" DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD ExtendeskDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY ExtendeskDeviceD0Entry;
EVT_IDD_CX_DEVICE_IO_CONTROL ExtendeskIoDeviceControl;
EVT_IDD_CX_ADAPTER_INIT_FINISHED ExtendeskAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES ExtendeskAdapterCommitModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION ExtendeskParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES ExtendeskMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES ExtendeskMonitorQueryModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN ExtendeskMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN ExtendeskMonitorUnassignSwapChain;

extern "C" BOOL WINAPI DllMain(HINSTANCE, UINT, LPVOID) { return TRUE; }

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object, PUNICODE_STRING registry_path) {
  WDF_DRIVER_CONFIG config;
  WDF_OBJECT_ATTRIBUTES attributes;
  WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
  WDF_DRIVER_CONFIG_INIT(&config, ExtendeskDeviceAdd);
  return WdfDriverCreate(driver_object, registry_path, &attributes, &config, WDF_NO_HANDLE);
}

_Use_decl_annotations_
NTSTATUS ExtendeskDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT device_init) {
  WDF_PNPPOWER_EVENT_CALLBACKS pnp;
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
  pnp.EvtDeviceD0Entry = ExtendeskDeviceD0Entry;
  WdfDeviceInitSetPnpPowerEventCallbacks(device_init, &pnp);

  IDD_CX_CLIENT_CONFIG config;
  IDD_CX_CLIENT_CONFIG_INIT(&config);
  config.EvtIddCxDeviceIoControl = ExtendeskIoDeviceControl;
  config.EvtIddCxAdapterInitFinished = ExtendeskAdapterInitFinished;
  config.EvtIddCxParseMonitorDescription = ExtendeskParseMonitorDescription;
  config.EvtIddCxMonitorGetDefaultDescriptionModes = ExtendeskMonitorGetDefaultModes;
  config.EvtIddCxMonitorQueryTargetModes = ExtendeskMonitorQueryModes;
  config.EvtIddCxAdapterCommitModes = ExtendeskAdapterCommitModes;
  config.EvtIddCxMonitorAssignSwapChain = ExtendeskMonitorAssignSwapChain;
  config.EvtIddCxMonitorUnassignSwapChain = ExtendeskMonitorUnassignSwapChain;
  NTSTATUS status = IddCxDeviceInitConfig(device_init, &config);
  if (!NT_SUCCESS(status)) return status;

  WDF_OBJECT_ATTRIBUTES attr;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DeviceContextWrapper);
  attr.EvtCleanupCallback = [](WDFOBJECT object) {
    auto* w = WdfObjectGet_DeviceContextWrapper(object);
    delete w->context;
    w->context = nullptr;
  };
  WDFDEVICE device = nullptr;
  status = WdfDeviceCreate(&device_init, &attr, &device);
  if (!NT_SUCCESS(status)) return status;

  // Interfaz por la que el host abre el dispositivo para enchufar el monitor.
  status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_EXTENDESK_IDD, nullptr);
  if (!NT_SUCCESS(status)) return status;

  status = IddCxDeviceInitialize(device);
  if (!NT_SUCCESS(status)) return status;
  WdfObjectGet_DeviceContextWrapper(device)->context = new DeviceContext(device);
  return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS ExtendeskDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
  WdfObjectGet_DeviceContextWrapper(device)->context->InitAdapter();
  return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID ExtendeskIoDeviceControl(WDFDEVICE device, WDFREQUEST request, size_t, size_t input_length,
                             ULONG io_control_code) {
  DeviceContext* ctx = WdfObjectGet_DeviceContextWrapper(device)->context;
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
  switch (io_control_code) {
    case IOCTL_EXTENDESK_PLUG: {
      ExtendeskPlugArgs* args = nullptr;
      if (input_length < sizeof(ExtendeskPlugArgs)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      status = WdfRequestRetrieveInputBuffer(request, sizeof(ExtendeskPlugArgs), reinterpret_cast<PVOID*>(&args),
                                             nullptr);
      if (!NT_SUCCESS(status)) break;
      Mode mode{args->width, args->height, args->refresh_hz};
      if (mode.width < 640 || mode.height < 480 || mode.width > 7680 || mode.height > 4320 ||
          mode.refresh < 24 || mode.refresh > 240) {
        status = STATUS_INVALID_PARAMETER;
        break;
      }
      mode.width &= ~1u;  // los codificadores de vídeo piden tamaños pares
      mode.height &= ~1u;
      status = ctx->Plug(mode);
      break;
    }
    case IOCTL_EXTENDESK_UNPLUG:
      ctx->Unplug();
      status = STATUS_SUCCESS;
      break;
    case IOCTL_EXTENDESK_PING:
      ctx->Ping();
      status = STATUS_SUCCESS;
      break;
    default:
      break;
  }
  WdfRequestComplete(request, status);
}

_Use_decl_annotations_
NTSTATUS ExtendeskAdapterInitFinished(IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED* in) {
  // El monitor no se crea aquí: espera a que el host lo pida.
  if (NT_SUCCESS(in->AdapterInitStatus)) WdfObjectGet_DeviceContextWrapper(adapter)->context->AdapterReady();
  return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS ExtendeskAdapterCommitModes(IDDCX_ADAPTER, const IDARG_IN_COMMITMODES*) { return STATUS_SUCCESS; }

_Use_decl_annotations_
NTSTATUS ExtendeskParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* in,
                                         IDARG_OUT_PARSEMONITORDESCRIPTION* out) {
  Mode preferred = kDefaultMode;
  extendesk::ParseEdidMode(static_cast<const uint8_t*>(in->MonitorDescription.pData),
                          in->MonitorDescription.DataSize, &preferred);
  const auto modes = ModeList(preferred);
  out->MonitorModeBufferOutputCount = static_cast<UINT>(modes.size());
  if (in->MonitorModeBufferInputCount < modes.size()) {
    return in->MonitorModeBufferInputCount > 0 ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
  }
  for (size_t i = 0; i < modes.size(); ++i) {
    in->pMonitorModes[i] = MakeMonitorMode(modes[i], IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
  }
  out->PreferredMonitorModeIdx = 0;
  return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS ExtendeskMonitorGetDefaultModes(IDDCX_MONITOR, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* in,
                                        IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* out) {
  // Solo se usa si un monitor llega sin EDID (no debería pasar: siempre se manda uno).
  const auto modes = ModeList(kDefaultMode);
  out->DefaultMonitorModeBufferOutputCount = static_cast<UINT>(modes.size());
  if (in->DefaultMonitorModeBufferInputCount == 0) return STATUS_SUCCESS;
  if (in->DefaultMonitorModeBufferInputCount < modes.size()) return STATUS_BUFFER_TOO_SMALL;
  for (size_t i = 0; i < modes.size(); ++i) {
    in->pDefaultMonitorModes[i] = MakeMonitorMode(modes[i], IDDCX_MONITOR_MODE_ORIGIN_DRIVER);
  }
  out->PreferredMonitorModeIdx = 0;
  return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS ExtendeskMonitorQueryModes(IDDCX_MONITOR monitor, const IDARG_IN_QUERYTARGETMODES* in,
                                   IDARG_OUT_QUERYTARGETMODES* out) {
  MonitorContext* ctx = WdfObjectGet_MonitorContextWrapper(monitor)->context;
  const auto modes = ModeList(ctx ? ctx->mode() : kDefaultMode);
  out->TargetModeBufferOutputCount = static_cast<UINT>(modes.size());
  if (in->TargetModeBufferInputCount >= modes.size()) {
    for (size_t i = 0; i < modes.size(); ++i) in->pTargetModes[i] = MakeTargetMode(modes[i]);
  }
  return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS ExtendeskMonitorAssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN* in) {
  WdfObjectGet_MonitorContextWrapper(monitor)->context->AssignSwapChain(in->hSwapChain, in->RenderAdapterLuid,
                                                                       in->hNextSurfaceAvailable);
  return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS ExtendeskMonitorUnassignSwapChain(IDDCX_MONITOR monitor) {
  WdfObjectGet_MonitorContextWrapper(monitor)->context->UnassignSwapChain();
  return STATUS_SUCCESS;
}
