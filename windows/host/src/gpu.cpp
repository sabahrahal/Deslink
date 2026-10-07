#include "gpu.h"

#include <d3d11_4.h>

#include <cstdio>
#include <cwchar>

namespace {

// Recorre todas las salidas de todos los adaptadores y crea el dispositivo en la primera que cumpla `match`.
template <typename Match>
bool CreateGpuForMatchingOutput(Match match, Gpu* gpu) {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

  int index = 0;
  for (UINT a = 0;; ++a) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;

    for (UINT o = 0;; ++o) {
      ComPtr<IDXGIOutput> output;
      if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_OUTPUT_DESC od{};
      output->GetDesc(&od);
      if (!match(index++, od)) continue;

      DXGI_ADAPTER_DESC1 ad{};
      adapter->GetDesc1(&ad);
      std::wprintf(L"GPU: %s\n", ad.Description);

      if (FAILED(output.As(&gpu->output))) return false;
      gpu->adapter = adapter;
      gpu->desktop_rect = od.DesktopCoordinates;

      const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
      D3D_FEATURE_LEVEL got{};
      HRESULT hr = D3D11CreateDevice(
          adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
          D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels,
          ARRAYSIZE(levels), D3D11_SDK_VERSION, &gpu->device, &got, &gpu->context);
      if (FAILED(hr)) {
        std::fprintf(stderr, "D3D11CreateDevice falló: 0x%08lx\n", hr);
        return false;
      }

      // El codificador de Media Foundation usa el dispositivo desde otro hilo.
      ComPtr<ID3D11Multithread> mt;
      if (SUCCEEDED(gpu->device.As(&mt))) mt->SetMultithreadProtected(TRUE);
      return true;
    }
  }
  return false;
}

}  // namespace

bool CreateGpuForOutput(int output_index, Gpu* gpu) {
  if (CreateGpuForMatchingOutput([&](int index, const DXGI_OUTPUT_DESC&) { return index == output_index; }, gpu)) {
    return true;
  }
  std::fprintf(stderr, "No existe la salida %d\n", output_index);
  return false;
}

bool CreateGpuForDeviceName(const wchar_t* device_name, Gpu* gpu) {
  if (CreateGpuForMatchingOutput(
          [&](int, const DXGI_OUTPUT_DESC& od) { return _wcsicmp(od.DeviceName, device_name) == 0; }, gpu)) {
    return true;
  }
  std::fprintf(stderr, "DXGI no encuentra la salida %ls\n", device_name);
  return false;
}
