#include "capture.h"

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
  HRESULT hr = dup_->AcquireNextFrame(timeout_ms, &info, &resource);
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
