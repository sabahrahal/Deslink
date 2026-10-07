#include "converter.h"

#include <windows.h>

#include <cstdio>

bool Nv12Converter::Init(Gpu& gpu, UINT width, UINT height, size_t ring_size) {
  gpu_ = &gpu;
  {
    DXGI_OUTPUT_DESC od{};
    if (SUCCEEDED(gpu.output->GetDesc(&od))) {
      origin_x_ = od.DesktopCoordinates.left;
      origin_y_ = od.DesktopCoordinates.top;
    }
  }
  width_ = width;
  height_ = height;

  if (FAILED(gpu.device.As(&video_device_)) || FAILED(gpu.context.As(&video_context_))) {
    std::fprintf(stderr, "La GPU no expone el procesador de vídeo D3D11\n");
    return false;
  }

  D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
  cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  cd.InputWidth = cd.OutputWidth = width;
  cd.InputHeight = cd.OutputHeight = height;
  cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
  if (FAILED(video_device_->CreateVideoProcessorEnumerator(&cd, &enumerator_)) ||
      FAILED(video_device_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_))) {
    std::fprintf(stderr, "No se pudo crear el procesador de vídeo\n");
    return false;
  }

  // RGB completo de entrada, YCbCr BT.709 de rango limitado a la salida.
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs{};
  in_cs.RGB_Range = 0;
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs{};
  out_cs.YCbCr_Matrix = 1;
  out_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
  video_context_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &in_cs);
  video_context_->VideoProcessorSetOutputColorSpace(processor_.Get(), &out_cs);

  // La textura de Desktop Duplication no sirve como entrada directa del
  // procesador: se copia (en GPU) a una textura propia.
  D3D11_TEXTURE2D_DESC td{};
  td.Width = width;
  td.Height = height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  if (FAILED(gpu.device->CreateTexture2D(&td, nullptr, &clean_))) return false;
  // La textura que lee el procesador admite GDI para poder pintar el cursor encima.
  td.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
  if (FAILED(gpu.device->CreateTexture2D(&td, nullptr, &bgra_copy_)) || FAILED(bgra_copy_.As(&copy_surface_))) {
    std::fprintf(stderr, "aviso: no se puede dibujar el cursor en esta GPU; la tablet no lo mostrará\n");
    copy_surface_.Reset();
    bgra_copy_.Reset();
    td.MiscFlags = 0;
    if (FAILED(gpu.device->CreateTexture2D(&td, nullptr, &bgra_copy_))) return false;
  }
  td.MiscFlags = 0;

  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
  ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  if (FAILED(video_device_->CreateVideoProcessorInputView(bgra_copy_.Get(), enumerator_.Get(),
                                                          &ivd, &input_view_))) {
    return false;
  }

  td.Format = DXGI_FORMAT_NV12;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  for (size_t i = 0; i < ring_size; ++i) {
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(gpu.device->CreateTexture2D(&td, nullptr, &tex))) {
      std::fprintf(stderr, "No se pudo crear la textura NV12 %zu\n", i);
      return false;
    }
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorOutputView> view;
    if (FAILED(video_device_->CreateVideoProcessorOutputView(tex.Get(), enumerator_.Get(), &ovd,
                                                             &view))) {
      return false;
    }
    ring_.push_back(tex);
    output_views_.push_back(view);
  }
  return true;
}

ID3D11Texture2D* Nv12Converter::Convert(ID3D11Texture2D* bgra) {
  // La salida puede tener un píxel de más si su tamaño era impar: se copia sólo la parte par.
  const D3D11_BOX box{0, 0, 0, width_, height_, 1};
  gpu_->context->CopySubresourceRegion(clean_.Get(), 0, 0, 0, 0, bgra, 0, &box);
  have_clean_ = true;
  return ComposeAndConvert();
}

ID3D11Texture2D* Nv12Converter::Refresh() {
  if (!have_clean_ || !draw_cursor_ || !copy_surface_) return nullptr;
  return ComposeAndConvert();
}

void Nv12Converter::DrawCursor() {
  CURSORINFO ci{};
  ci.cbSize = sizeof(ci);
  if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;
  if (ci.hCursor != cursor_handle_) {
    ICONINFO ii{};
    if (!GetIconInfo(ci.hCursor, &ii)) return;
    hotspot_x_ = static_cast<int>(ii.xHotspot);
    hotspot_y_ = static_cast<int>(ii.yHotspot);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    cursor_handle_ = ci.hCursor;
  }
  HDC dc = nullptr;
  if (FAILED(copy_surface_->GetDC(FALSE, &dc))) return;
  DrawIconEx(dc, ci.ptScreenPos.x - origin_x_ - hotspot_x_, ci.ptScreenPos.y - origin_y_ - hotspot_y_,
             ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
  copy_surface_->ReleaseDC(nullptr);
}

ID3D11Texture2D* Nv12Converter::ComposeAndConvert() {
  gpu_->context->CopyResource(bgra_copy_.Get(), clean_.Get());
  if (draw_cursor_ && copy_surface_) DrawCursor();

  D3D11_VIDEO_PROCESSOR_STREAM stream{};
  stream.Enable = TRUE;
  stream.pInputSurface = input_view_.Get();

  const size_t slot = next_;
  HRESULT hr =
      video_context_->VideoProcessorBlt(processor_.Get(), output_views_[slot].Get(), 0, 1, &stream);
  if (FAILED(hr)) {
    std::fprintf(stderr, "VideoProcessorBlt falló: 0x%08lx\n", hr);
    return nullptr;
  }
  next_ = (next_ + 1) % ring_.size();
  return ring_[slot].Get();
}
