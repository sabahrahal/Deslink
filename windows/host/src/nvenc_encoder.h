#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "encoder.h"

// NVENC sin pasar por Media Foundation: el MFT de NVIDIA añade su cola de eventos y sus hilos, y no deja pedir el
// ajuste de latencia ultrabaja ni un VBV de un frame. Aquí cada frame se codifica en cuanto llega (modo síncrono,
// uno en vuelo), con el preset más rápido y la sintonía de latencia ultrabaja. Carga nvEncodeAPI64.dll del driver
// en marcha: si no hay NVIDIA o el driver es viejo, Init falla y el host usa MfEncoder.
class NvencEncoder : public VideoEncoder {
 public:
  ~NvencEncoder() override;

  bool Init(Gpu& gpu, VideoCodec codec, UINT width, UINT height, UINT fps, UINT bitrate_bps,
            OutputCallback callback) override;
  void Submit(ID3D11Texture2D* nv12, uint64_t timestamp_us) override;
  void Stop() override;
  bool retains_frames() const override { return false; }  // síncrono: cada frame sale antes del siguiente

  // Se puede probar antes de crear nada: la GPU que captura es de NVIDIA.
  static bool IsNvidia(Gpu& gpu);

 private:
  struct Registered {
    ComPtr<ID3D11Texture2D> texture;
    void* handle = nullptr;
  };

  void Loop();
  bool Encode(ID3D11Texture2D* texture, uint64_t timestamp_us);
  void* RegisterTexture(ID3D11Texture2D* texture);
  void ApplyBitrate(uint32_t bps);
  void Release();

  HMODULE dll_ = nullptr;
  struct Api;
  Api* api_ = nullptr;  // la tabla de funciones de NVENC (NV_ENCODE_API_FUNCTION_LIST)
  struct Params;
  Params* params_ = nullptr;  // NV_ENC_INITIALIZE_PARAMS + NV_ENC_CONFIG, para reconfigurar la tasa
  void* session_ = nullptr;
  void* bitstream_ = nullptr;
  std::vector<Registered> registered_;  // las texturas del anillo del conversor, registradas una sola vez

  OutputCallback callback_;
  UINT width_ = 0, height_ = 0, fps_ = 60;
  uint64_t errors_ = 0;

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool has_pending_ = false;
  ComPtr<ID3D11Texture2D> pending_;
  uint64_t pending_ts_ = 0;
  bool stop_ = false;
};
