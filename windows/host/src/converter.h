#pragma once

#include <dxgi.h>
#include <vector>

#include "gpu.h"

// Convierte BGRA a NV12 en la GPU con el procesador de vídeo de D3D11. Las
// texturas NV12 salen de un anillo para que el codificador pueda seguir leyendo
// una mientras se escribe la siguiente. La salida puede ser más pequeña que la captura (perfiles de
// batería): el mismo procesador escala, sin pasada extra.
class Nv12Converter {
 public:
  bool Init(Gpu& gpu, UINT width, UINT height, UINT out_width, UINT out_height, size_t ring_size = 6);
  // Guarda una copia de la imagen del escritorio (solo una copia en la GPU, sin convertir): hay que
  // hacerlo antes de soltar el frame de Desktop Duplication.
  void Store(ID3D11Texture2D* bgra);
  // Pinta el cursor sobre la última imagen guardada y la convierte a NV12. Es la parte cara: se llama
  // solo cuando el frame va de verdad al codificador. Devuelve la textura (propiedad del conversor) o nullptr.
  ID3D11Texture2D* Compose();
  bool has_image() const { return have_clean_; }
  // Dibuja el cursor del sistema sobre la imagen (Desktop Duplication no lo incluye).
  void SetDrawCursor(bool on) { draw_cursor_ = on; }
  // Posición del monitor en el escritorio virtual (cambia si el usuario lo mueve en Configuración).
  void SetOrigin(int x, int y) {
    origin_x_ = x;
    origin_y_ = y;
  }

 private:
  void DrawCursor();

  Gpu* gpu_ = nullptr;
  ComPtr<ID3D11VideoDevice> video_device_;
  ComPtr<ID3D11VideoContext> video_context_;
  ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
  ComPtr<ID3D11VideoProcessor> processor_;
  ComPtr<ID3D11Texture2D> clean_;      // última imagen del escritorio, sin cursor
  ComPtr<ID3D11Texture2D> bgra_copy_;  // clean_ + cursor: entrada del procesador de vídeo
  ComPtr<IDXGISurface1> copy_surface_;
  bool draw_cursor_ = true;
  bool have_clean_ = false;
  int origin_x_ = 0, origin_y_ = 0;
  HCURSOR cursor_handle_ = nullptr;
  int hotspot_x_ = 0, hotspot_y_ = 0;
  ComPtr<ID3D11VideoProcessorInputView> input_view_;
  std::vector<ComPtr<ID3D11Texture2D>> ring_;
  std::vector<ComPtr<ID3D11VideoProcessorOutputView>> output_views_;
  size_t next_ = 0;
  UINT width_ = 0;
  UINT height_ = 0;
  UINT out_width_ = 0;
  UINT out_height_ = 0;
};
