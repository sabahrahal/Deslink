#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "encoder.h"

// AMD AMF sin pasar por Media Foundation, como NvencEncoder con NVIDIA: uso «latencia ultrabaja», preset más rápido,
// CBR con VBV de un frame y un frame en vuelo (se espera su salida antes de dar el siguiente). Carga amfrt64.dll del
// driver en marcha: si no hay AMD o el driver es viejo, Init falla y el host usa MfEncoder.
class AmfEncoder : public VideoEncoder {
 public:
  ~AmfEncoder() override;

  bool Init(Gpu& gpu, VideoCodec codec, UINT width, UINT height, UINT fps, UINT bitrate_bps,
            OutputCallback callback) override;
  void Submit(ID3D11Texture2D* nv12, uint64_t timestamp_us) override;
  void Stop() override;
  bool retains_frames() const override { return false; }  // cada frame sale antes de dar el siguiente

  static bool IsAmd(Gpu& gpu);

 private:
  struct Impl;  // los punteros de AMF (cabeceras de AMD solo en el .cpp)

  void Loop();
  bool Encode(ID3D11Texture2D* texture, uint64_t timestamp_us);
  void ApplyBitrate(uint32_t bps);

  HMODULE dll_ = nullptr;
  Impl* impl_ = nullptr;
  bool hevc_ = false;
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
