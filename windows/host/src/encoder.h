#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>

#include "gpu.h"

enum class VideoCodec { kH264, kHevc };

// Codificador por hardware de Media Foundation (en una RTX: NVENC) en modo de
// baja latencia: sin B-frames, sin GOP periódico, CBR, tamaño de cola mínimo.
// Recibe texturas NV12 de la GPU y entrega el flujo codificado por callback.
class MfEncoder {
 public:
  using OutputCallback =
      std::function<void(const uint8_t* data, size_t size, bool keyframe, uint64_t timestamp_us)>;

  ~MfEncoder();

  bool Init(Gpu& gpu, VideoCodec codec, UINT width, UINT height, UINT fps, UINT bitrate_bps,
            OutputCallback callback);

  // Deja el frame como el siguiente a codificar; si había otro sin consumir, se
  // sustituye (nunca se acumulan frames).
  void Submit(ID3D11Texture2D* nv12, uint64_t timestamp_us);

  void RequestKeyframe() { keyframe_requested_ = true; }
  bool keyframe_pending() const { return keyframe_requested_; }
  // Diagnóstico: frames entregados al MFT y tiempo que esperaron en el buzón hasta que el MFT los pidió.
  std::atomic<uint64_t> fed_frames{0};
  std::atomic<uint64_t> mailbox_wait_us{0};

  void Stop();

 private:
  struct PendingFrame {
    ComPtr<ID3D11Texture2D> texture;
    uint64_t timestamp_us = 0;
  };

  bool SetupTransform(IMFActivate* activate, UINT bitrate_bps);
  void ResetTransform();
  bool ConfigureTypes(VideoCodec codec);
  void ApplyLowLatencySettings(UINT bitrate_bps);
  void EventLoop();
  bool FeedInput(const PendingFrame& frame);
  void DrainOutput();
  void CaptureSequenceHeader();

  ComPtr<IMFTransform> mft_;
  ComPtr<IMFMediaEventGenerator> events_;
  ComPtr<ICodecAPI> codec_api_;
  ComPtr<IMFDXGIDeviceManager> device_manager_;

  OutputCallback callback_;
  std::vector<uint8_t> sequence_header_;
  VideoCodec codec_ = VideoCodec::kHevc;
  UINT width_ = 0, height_ = 0, fps_ = 60;

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool has_pending_ = false;
  PendingFrame pending_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> keyframe_requested_{true};  // el primer frame es clave
};
