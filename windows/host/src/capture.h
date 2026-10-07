#pragma once

#include "gpu.h"

// Captura de una salida con DXGI Desktop Duplication. La textura devuelta vive en
// la GPU y sólo es válida hasta la siguiente llamada a Release().
class DesktopCapture {
 public:
  enum class Result { kFrame, kCursorMoved, kTimeout, kError };

  bool Init(Gpu& gpu);
  Result Acquire(UINT timeout_ms, ID3D11Texture2D** texture);
  void Release();

  UINT width() const { return width_; }
  UINT height() const { return height_; }

 private:
  ComPtr<IDXGIOutputDuplication> dup_;
  ComPtr<ID3D11Texture2D> frame_;
  bool held_ = false;
  UINT width_ = 0;
  UINT height_ = 0;
};
