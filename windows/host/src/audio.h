#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

// Sonido del PC para la tablet: captura lo que suena en la salida de audio predeterminada de Windows
// (WASAPI en modo loopback), lo pasa a 48 kHz estéreo y lo codifica en Opus en trozos de 10 ms.
// Si el usuario cambia de altavoces o auriculares, sigue a la nueva salida predeterminada.
class AudioCapture {
 public:
  // Un trozo Opus de 10 ms. Se llama desde el hilo de captura.
  using FrameHandler = std::function<void(const uint8_t* data, size_t size, uint64_t timestamp_us)>;

  ~AudioCapture();

  bool Start(FrameHandler on_frame, uint32_t bitrate_bps);
  void Stop();
  bool running() const { return thread_.joinable(); }
  uint64_t frames() const { return frames_; }
  // Trozos con algo audible (no silencio): para ver en el log si de verdad suena algo en el PC.
  uint64_t loud_frames() const { return loud_frames_; }

 private:
  void Loop();
  // Captura de la salida predeterminada hasta que haya que parar o cambie la salida. false si no se pudo abrir.
  bool CaptureDefaultDevice(void* encoder);

  FrameHandler on_frame_;
  uint32_t bitrate_bps_ = 0;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> frames_{0};
  std::atomic<uint64_t> loud_frames_{0};
};

// Pasa sonido intercalado de `channels` canales a `rate` Hz a estéreo de 48 kHz (interpolación lineal, que
// basta para 44,1 kHz; la mayoría de PCs ya mezclan a 48 kHz y entonces solo copia). Mantiene el estado entre
// llamadas para no dejar saltos entre bloques.
class StereoResampler {
 public:
  void Reset(uint32_t rate, uint32_t channels);
  // Añade `frames` muestras (float, -1..1) al final de `out` (estéreo intercalado, 48 kHz).
  void Process(const float* in, size_t frames, std::vector<float>* out);

 private:
  uint32_t rate_ = 48000;
  uint32_t channels_ = 2;
  double pos_ = 0;   // posición de la siguiente muestra de salida, en muestras de entrada, respecto a `last_`
  float last_[2] = {0, 0};
};
