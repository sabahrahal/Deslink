#pragma once

#include <vector>

#include "gpu.h"

// Convierte BGRA a NV12 en la GPU con el procesador de vídeo de D3D11. Las
// texturas NV12 salen de un anillo para que el codificador pueda seguir leyendo
// una mientras se escribe la siguiente.
class Nv12Converter {
 public:
  bool Init(Gpu& gpu, UINT width, UINT height, size_t ring_size = 6);
  // Devuelve la textura NV12 resultante (propiedad del conversor) o nullptr.
  ID3D11Texture2D* Convert(ID3D11Texture2D* bgra);

 private:
  Gpu* gpu_ = nullptr;
  ComPtr<ID3D11VideoDevice> video_device_;
  ComPtr<ID3D11VideoContext> video_context_;
  ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
  ComPtr<ID3D11VideoProcessor> processor_;
  ComPtr<ID3D11Texture2D> bgra_copy_;
  ComPtr<ID3D11VideoProcessorInputView> input_view_;
  std::vector<ComPtr<ID3D11Texture2D>> ring_;
  std::vector<ComPtr<ID3D11VideoProcessorOutputView>> output_views_;
  size_t next_ = 0;
  UINT width_ = 0;
  UINT height_ = 0;
};
