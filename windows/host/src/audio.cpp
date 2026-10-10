#include "audio.h"

#include <windows.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <opus.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "dlp/packet.h"

namespace {

uint64_t NowUs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

template <typename T>
struct ComPtr {
  T* p = nullptr;
  ~ComPtr() {
    if (p) p->Release();
  }
  T** operator&() { return &p; }
  T* operator->() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

struct CoMem {
  void* p = nullptr;
  ~CoMem() { CoTaskMemFree(p); }
};

// Id de la salida predeterminada ("" si no hay ninguna).
std::wstring DefaultRenderId(IMMDeviceEnumerator* enumerator) {
  ComPtr<IMMDevice> device;
  if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return {};
  LPWSTR id = nullptr;
  if (FAILED(device->GetId(&id))) return {};
  std::wstring out(id);
  CoTaskMemFree(id);
  return out;
}

// Mientras el sonido va a la tablet se silencian los altavoces del PC, para que no suene en los dos sitios. Qué
// salida silenció DeskLink queda apuntado en el registro: si el host se cierra de golpe, la siguiente vez la
// vuelve a activar.
constexpr wchar_t kMuteKey[] = L"Software\\DeskLink";
constexpr wchar_t kMuteValue[] = L"SonidoSilenciado";

void RememberMuted(const std::wstring& id) {
  RegSetKeyValueW(HKEY_CURRENT_USER, kMuteKey, kMuteValue, REG_SZ, id.c_str(),
                  static_cast<DWORD>((id.size() + 1) * sizeof(wchar_t)));
}

void ForgetMuted() { RegDeleteKeyValueW(HKEY_CURRENT_USER, kMuteKey, kMuteValue); }

bool SetEndpointMute(IMMDevice* device, BOOL mute, BOOL* was = nullptr) {
  ComPtr<IAudioEndpointVolume> volume;
  if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&volume)))) {
    return false;
  }
  if (was && FAILED(volume->GetMute(was))) return false;
  return SUCCEEDED(volume->SetMute(mute, nullptr));
}

// La salida que DeskLink dejó silenciada en una sesión que no terminó bien.
void RestoreLeftoverMute(IMMDeviceEnumerator* enumerator) {
  wchar_t id[512];
  DWORD size = sizeof(id);
  if (RegGetValueW(HKEY_CURRENT_USER, kMuteKey, kMuteValue, RRF_RT_REG_SZ, nullptr, id, &size) != ERROR_SUCCESS) return;
  ComPtr<IMMDevice> device;
  if (SUCCEEDED(enumerator->GetDevice(id, &device))) SetEndpointMute(device.p, FALSE);
  ForgetMuted();
}

enum class SampleFormat { kFloat32, kInt16, kInt24, kInt32, kUnsupported };

SampleFormat FormatOf(const WAVEFORMATEX* wf) {
  bool is_float = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
  bool is_pcm = wf->wFormatTag == WAVE_FORMAT_PCM;
  if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
    // KSDATAFORMAT_SUBTYPE_PCM e IEEE_FLOAT solo se distinguen en el primer campo (1 y 3).
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
    is_float = ext->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
    is_pcm = ext->SubFormat.Data1 == WAVE_FORMAT_PCM;
  }
  if (is_float && wf->wBitsPerSample == 32) return SampleFormat::kFloat32;
  if (is_pcm && wf->wBitsPerSample == 16) return SampleFormat::kInt16;
  if (is_pcm && wf->wBitsPerSample == 24) return SampleFormat::kInt24;
  if (is_pcm && wf->wBitsPerSample == 32) return SampleFormat::kInt32;
  return SampleFormat::kUnsupported;
}

void ToFloat(const BYTE* data, size_t samples, SampleFormat format, std::vector<float>* out) {
  out->resize(samples);
  float* o = out->data();
  switch (format) {
    case SampleFormat::kFloat32:
      std::memcpy(o, data, samples * sizeof(float));
      break;
    case SampleFormat::kInt16:
      for (size_t i = 0; i < samples; ++i) o[i] = reinterpret_cast<const int16_t*>(data)[i] / 32768.0f;
      break;
    case SampleFormat::kInt24:
      for (size_t i = 0; i < samples; ++i) {
        const BYTE* s = data + 3 * i;
        const int32_t v = static_cast<int32_t>((s[0] << 8) | (s[1] << 16) | (s[2] << 24)) >> 8;
        o[i] = v / 8388608.0f;
      }
      break;
    case SampleFormat::kInt32:
      for (size_t i = 0; i < samples; ++i) o[i] = reinterpret_cast<const int32_t*>(data)[i] / 2147483648.0f;
      break;
    case SampleFormat::kUnsupported:
      break;
  }
}

}  // namespace

void StereoResampler::Reset(uint32_t rate, uint32_t channels) {
  rate_ = rate ? rate : 48000;
  channels_ = channels ? channels : 2;
  pos_ = 0;
  last_[0] = last_[1] = 0;
}

void StereoResampler::Process(const float* in, size_t frames, std::vector<float>* out) {
  // Mezcla a estéreo. Orden de canales de Windows: FL FR FC LFE BL BR SL SR; el central va a los dos lados,
  // el LFE se descarta y los traseros/laterales van a su lado.
  thread_local std::vector<float> stereo;
  stereo.resize(frames * 2);
  const uint32_t ch = channels_;
  for (size_t i = 0; i < frames; ++i) {
    const float* s = in + i * ch;
    float l, r;
    if (ch == 1) {
      l = r = s[0];
    } else if (ch == 2) {
      l = s[0];
      r = s[1];
    } else {
      l = s[0];
      r = s[1];
      float side = 1;
      if (ch >= 3) {
        l += 0.707f * s[2];
        r += 0.707f * s[2];
        side += 0.707f;
      }
      for (uint32_t c = 4; c + 1 < ch; c += 2) {
        l += 0.707f * s[c];
        r += 0.707f * s[c + 1];
      }
      if (ch >= 6) side += 0.707f;
      l /= side;
      r /= side;
    }
    stereo[2 * i] = l;
    stereo[2 * i + 1] = r;
  }
  if (rate_ == dlp::kAudioSampleRate) {
    out->insert(out->end(), stereo.begin(), stereo.end());
    return;
  }
  // S[0] = last_, S[k] = stereo[k - 1]: se interpola entre S[floor(pos)] y S[floor(pos) + 1].
  const double step = static_cast<double>(rate_) / dlp::kAudioSampleRate;
  const double n = static_cast<double>(frames);
  auto sample = [&](size_t k, int c) { return k == 0 ? last_[c] : stereo[2 * (k - 1) + c]; };
  while (pos_ < n) {
    const size_t k = static_cast<size_t>(pos_);
    const float t = static_cast<float>(pos_ - k);
    for (int c = 0; c < 2; ++c) out->push_back(sample(k, c) + (sample(k + 1, c) - sample(k, c)) * t);
    pos_ += step;
  }
  pos_ -= n;
  if (frames > 0) {
    last_[0] = stereo[2 * (frames - 1)];
    last_[1] = stereo[2 * (frames - 1) + 1];
  }
}

AudioCapture::~AudioCapture() { Stop(); }

bool AudioCapture::Start(FrameHandler on_frame, uint32_t bitrate_bps) {
  if (running()) return true;
  on_frame_ = std::move(on_frame);
  bitrate_bps_ = bitrate_bps;
  stop_ = false;
  thread_ = std::thread([this] { Loop(); });
  return true;
}

void AudioCapture::Stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
}

void AudioCapture::Loop() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  int err = 0;
  // RESTRICTED_LOWDELAY: solo CELT, 2,5 ms de retardo del códec (en vez de 6,5 ms). Va bien con música y juegos.
  OpusEncoder* enc = opus_encoder_create(dlp::kAudioSampleRate, dlp::kAudioChannels,
                                         OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
  if (!enc) {
    std::fprintf(stderr, "Sonido: no se pudo crear el codificador Opus (%d).\n", err);
  } else {
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(static_cast<opus_int32>(bitrate_bps_)));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));  // de sobra para 10 ms, y apenas usa CPU
    bool warned = false;
    while (!stop_) {
      if (CaptureDefaultDevice(enc)) {
        warned = false;
        continue;
      }
      if (!warned) std::printf("Sonido: no hay salida de audio que capturar; lo reintento cada segundo.\n");
      warned = true;
      for (int i = 0; i < 10 && !stop_; ++i) Sleep(100);
    }
    opus_encoder_destroy(enc);
  }
  CoUninitialize();
}

bool AudioCapture::CaptureDefaultDevice(void* encoder) {
  auto* enc = static_cast<OpusEncoder*>(encoder);
  ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&enumerator)))) {
    return false;
  }
  RestoreLeftoverMute(enumerator.p);
  ComPtr<IMMDevice> device;
  if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return false;
  const std::wstring id = DefaultRenderId(enumerator.p);
  ComPtr<IAudioClient> client;
  if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)))) {
    return false;
  }
  CoMem mix;
  if (FAILED(client->GetMixFormat(reinterpret_cast<WAVEFORMATEX**>(&mix.p)))) return false;
  const auto* wf = static_cast<const WAVEFORMATEX*>(mix.p);
  const SampleFormat format = FormatOf(wf);
  if (format == SampleFormat::kUnsupported || wf->nChannels == 0) {
    std::fprintf(stderr, "Sonido: formato de la salida no soportado (%u bits, etiqueta %u).\n", wf->wBitsPerSample,
                 wf->wFormatTag);
    return false;
  }
  // 100 ms de búfer en Windows: solo es la capacidad; se vacía cada 5 ms, así que no añade retardo.
  constexpr REFERENCE_TIME kBuffer = 1'000'000;
  if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, kBuffer, 0, wf, nullptr))) {
    return false;
  }
  ComPtr<IAudioCaptureClient> capture;
  if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture)))) return false;
  if (FAILED(client->Start())) return false;
  std::printf("Sonido: capturando la salida predeterminada (%lu Hz, %u canales) para la tablet.\n",
              wf->nSamplesPerSec, wf->nChannels);
  // Solo si estaba sonando: si el usuario ya la tenía silenciada, al terminar se queda como estaba.
  BOOL was_muted = TRUE;
  bool muted_by_us = false;
  if (SetEndpointMute(device.p, TRUE, &was_muted) && !was_muted) {
    muted_by_us = true;
    RememberMuted(id);
    std::printf("Sonido: silencio los altavoces del PC mientras suena en la tablet.\n");
  }

  StereoResampler resampler;
  resampler.Reset(wf->nSamplesPerSec, wf->nChannels);
  std::vector<float> in, pcm;
  constexpr size_t kFrame = dlp::kAudioFrameSamples;
  unsigned char packet[1276];  // máximo de un paquete Opus
  uint64_t last_check_us = NowUs();
  bool reopen = false;
  while (!stop_ && !reopen) {
    Sleep(5);
    UINT32 next = 0;
    HRESULT hr;
    while (SUCCEEDED(hr = capture->GetNextPacketSize(&next)) && next > 0) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      if (FAILED(hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
      const size_t samples = static_cast<size_t>(frames) * wf->nChannels;
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        in.assign(samples, 0.0f);
      } else {
        ToFloat(data, samples, format, &in);
      }
      capture->ReleaseBuffer(frames);
      resampler.Process(in.data(), frames, &pcm);
    }
    if (FAILED(hr)) {
      // AUDCLNT_E_DEVICE_INVALIDATED: se desconectó la salida (auriculares USB, HDMI...). Se busca la nueva
      // (tras una pausa: Windows tarda un poco en elegir otra salida predeterminada).
      reopen = true;
      Sleep(200);
      break;
    }
    size_t offset = 0;
    while (pcm.size() - offset >= kFrame * 2) {
      const opus_int32 n = opus_encode_float(enc, pcm.data() + offset, static_cast<int>(kFrame), packet,
                                             static_cast<opus_int32>(sizeof(packet)));
      offset += kFrame * 2;
      if (n > 0) {
        ++frames_;
        // Por debajo de ~-60 dB no se oye: cuenta como silencio.
        for (size_t i = 0; i < kFrame * 2; ++i) {
          if (std::fabs(pcm[offset - kFrame * 2 + i]) > 0.001f) {
            ++loud_frames_;
            break;
          }
        }
        on_frame_(packet, static_cast<size_t>(n), NowUs());
      }
    }
    pcm.erase(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(offset));
    // Si el usuario elige otros altavoces o auriculares en Windows, se pasa a capturar esos.
    const uint64_t now = NowUs();
    if (now - last_check_us >= 1'000'000) {
      last_check_us = now;
      if (DefaultRenderId(enumerator.p) != id) {
        std::printf("Sonido: cambió la salida de audio predeterminada.\n");
        reopen = true;
      }
    }
  }
  client->Stop();
  if (muted_by_us) {
    SetEndpointMute(device.p, FALSE);
    ForgetMuted();
    std::printf("Sonido: vuelven a sonar los altavoces del PC.\n");
  }
  return true;
}
