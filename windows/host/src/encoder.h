#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>

#include "gpu.h"

enum class VideoCodec { kH264, kHevc };

// Lo que el resto del host usa de un codificador: se le dan texturas NV12 de la GPU y entrega el flujo codificado
// por callback. Hay dos: NVENC directo (nvenc_encoder.h, solo con NVIDIA) y Media Foundation (cualquier GPU).
class VideoEncoder {
 public:
  using OutputCallback =
      std::function<void(const uint8_t* data, size_t size, bool keyframe, uint64_t timestamp_us)>;

  virtual ~VideoEncoder() = default;

  virtual bool Init(Gpu& gpu, VideoCodec codec, UINT width, UINT height, UINT fps, UINT bitrate_bps,
                    OutputCallback callback) = 0;

  // Deja el frame como el siguiente a codificar; si había otro sin consumir, se
  // sustituye (nunca se acumulan frames).
  virtual void Submit(ID3D11Texture2D* nv12, uint64_t timestamp_us) = 0;

  virtual void Stop() = 0;

  void RequestKeyframe() { keyframe_requested_ = true; }
  // Cambia la tasa en caliente; se aplica antes del siguiente frame.
  void SetBitrate(uint32_t bps) { bitrate_request_ = bps; }
  bool keyframe_pending() const { return keyframe_requested_; }
  // Diagnóstico: frames entregados al codificador y tiempo que esperaron en el buzón hasta que este los pidió.
  std::atomic<uint64_t> fed_frames{0};
  // Frames mandados y descartados porque el codificador aún no había pedido el anterior (no da abasto).
  std::atomic<uint64_t> submitted_frames{0};
  std::atomic<uint64_t> dropped_frames{0};
  std::atomic<uint64_t> mailbox_wait_us{0};
  // Tiempo que el codificador tarda de verdad en codificar: desde que recibe el frame hasta que sale.
  std::atomic<uint64_t> encode_us{0};
  std::atomic<uint64_t> encode_count{0};

 protected:
  std::atomic<uint32_t> bitrate_request_{0};
  std::atomic<bool> keyframe_requested_{true};  // el primer frame es clave
};

// Codificador por hardware de Media Foundation (en una RTX: NVENC) en modo de
// baja latencia: sin B-frames, sin GOP periódico, CBR, tamaño de cola mínimo.
class MfEncoder : public VideoEncoder {
 public:
  ~MfEncoder() override;

  bool Init(Gpu& gpu, VideoCodec codec, UINT width, UINT height, UINT fps, UINT bitrate_bps,
            OutputCallback callback) override;
  void Submit(ID3D11Texture2D* nv12, uint64_t timestamp_us) override;
  void Stop() override;

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
  void EventPump();
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

  // Dos hilos: EventPump espera los eventos del MFT (GetEvent bloquea) y los pasa a EventLoop, que da frames y
  // recoge salidas. Así una petición de entrada sin frame listo no tapa la salida ya codificada.
  std::thread thread_;
  std::thread pump_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<MediaEventType> events_queue_;
  int need_input_ = 0;      // entradas que el MFT pidió y aún no se le dieron
  bool pump_done_ = false;  // GetEvent falló o acabó el vaciado: no llegarán más eventos
  bool has_pending_ = false;
  PendingFrame pending_;
  std::deque<std::pair<uint64_t, uint64_t>> in_flight_;  // (timestamp del frame, cuándo entró al MFT)
  std::atomic<bool> stop_{false};
};
