#include "capture.h"

#include <chrono>
#include <cstdio>

bool DesktopCapture::Init(Gpu& gpu) {
  Release();
  dup_.Reset();
  HRESULT hr = gpu.output->DuplicateOutput(gpu.device.Get(), &dup_);
  if (FAILED(hr)) {
    std::fprintf(stderr, "DuplicateOutput falló: 0x%08lx\n", hr);
    return false;
  }
  DXGI_OUTDUPL_DESC desc{};
  dup_->GetDesc(&desc);
  // El codificador necesita dimensiones pares.
  width_ = desc.ModeDesc.Width & ~1u;
  height_ = desc.ModeDesc.Height & ~1u;
  return true;
}

DesktopCapture::Result DesktopCapture::Acquire(UINT timeout_ms, ID3D11Texture2D** texture) {
  Release();
  DXGI_OUTDUPL_FRAME_INFO info{};
  ComPtr<IDXGIResource> resource;
  // Se espera fuera de AcquireNextFrame: mientras espera retiene el cerrojo del dispositivo D3D11, que es el mismo
  // que usa el codificador, y con el escritorio quieto lo dejaba esperando hasta 16 ms en cada paso de cada frame
  // (40-60 ms por frame a 2944x1840 en una RTX 4060, y 30 fps). Se pregunta sin esperar y se duerme 1 ms entre
  // intentos (el host ya pide timeBeginPeriod(1)).
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  HRESULT hr;
  while ((hr = dup_->AcquireNextFrame(0, &info, &resource)) == DXGI_ERROR_WAIT_TIMEOUT &&
         std::chrono::steady_clock::now() < deadline) {
    Sleep(1);
  }
  if (hr == DXGI_ERROR_WAIT_TIMEOUT) return Result::kTimeout;
  if (hr == DXGI_ERROR_ACCESS_LOST) return Result::kLost;
  if (FAILED(hr)) {
    std::fprintf(stderr, "AcquireNextFrame falló: 0x%08lx\n", hr);
    return Result::kError;
  }
  held_ = true;

  // Sólo se movió el cursor: no hay imagen nueva que codificar.
  if (info.LastPresentTime.QuadPart == 0) {
    Release();
    return info.LastMouseUpdateTime.QuadPart != 0 ? Result::kCursorMoved : Result::kTimeout;
  }
  if (FAILED(resource.As(&frame_))) {
    Release();
    return Result::kError;
  }
  *texture = frame_.Get();
  return Result::kFrame;
}

void DesktopCapture::Release() {
  frame_.Reset();
  if (held_) {
    dup_->ReleaseFrame();
    held_ = false;
  }
}
