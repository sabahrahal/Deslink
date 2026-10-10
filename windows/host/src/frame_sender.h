#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// Envía los frames codificados desde su propio hilo. Antes se enviaban desde el hilo del codificador y un envío lento
// (el túnel de adb por cable, o el reparto con --pace por WiFi) lo frenaba: el codificador no recibía el frame
// siguiente hasta acabar de enviar el anterior y la fluidez bajaba a 30 fps aunque la gráfica diera para más.
// Si el envío se atrasa más de kMaxQueued frames, se tiran los que esperan y se pide un frame clave (un P-frame sin el
// anterior saldría roto); hasta que llega, el resto también se tira.
class FrameSender {
 public:
  struct Frame {
    std::vector<uint8_t> data;
    bool keyframe = false;
    uint64_t timestamp_us = 0;
  };
  using SendFn = std::function<void(const Frame&)>;

  static constexpr size_t kMaxQueued = 4;

  ~FrameSender() { Stop(); }

  void Start(SendFn send) {
    send_ = std::move(send);
    stop_ = false;
    thread_ = std::thread([this] { Loop(); });
  }

  // Desde el hilo del codificador. Devuelve true si hay que pedir un frame clave.
  bool Push(const uint8_t* data, size_t size, bool keyframe, uint64_t timestamp_us) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (waiting_keyframe_ && !keyframe) {
        ++dropped;
        return false;
      }
      waiting_keyframe_ = false;
      if (queue_.size() >= kMaxQueued) {
        dropped += queue_.size() + (keyframe ? 0 : 1);
        queue_.clear();
        if (!keyframe) {
          waiting_keyframe_ = true;
          return true;
        }
      }
      queue_.push_back(Frame{std::vector<uint8_t>(data, data + size), keyframe, timestamp_us});
    }
    cv_.notify_one();
    return false;
  }

  // Tira lo pendiente (el codificador se va a rehacer con otro tamaño: sus frames ya no sirven).
  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    waiting_keyframe_ = false;
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

  // Tiempo que el hilo pasó enviando y bytes enviados: lo que aguanta el enlace (ver RateController::OnLinkLoad).
  std::atomic<uint64_t> busy_us{0};
  std::atomic<uint64_t> sent_bytes{0};
  std::atomic<uint64_t> sent_frames{0};
  std::atomic<uint64_t> dropped{0};

 private:
  void Loop() {
    while (true) {
      Frame frame;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (stop_) return;
        frame = std::move(queue_.front());
        queue_.pop_front();
      }
      const auto start = std::chrono::steady_clock::now();
      send_(frame);
      busy_us += static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
      sent_bytes += frame.data.size();
      ++sent_frames;
    }
  }

  SendFn send_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Frame> queue_;
  bool waiting_keyframe_ = false;
  bool stop_ = false;
};
