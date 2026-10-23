#include "amf_encoder.h"

#include <chrono>
#include <cstdio>

#include "../third_party/amf/components/VideoEncoderHEVC.h"
#include "../third_party/amf/components/VideoEncoderVCE.h"
#include "../third_party/amf/core/Factory.h"

struct AmfEncoder::Impl {
  amf::AMFFactory* factory = nullptr;  // del driver: no se libera
  amf::AMFContextPtr context;
  amf::AMFComponentPtr encoder;
};

namespace {

constexpr uint32_t kAmdVendorId = 0x1002;

uint64_t NowUs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// Los nombres de las propiedades de H.264 y HEVC son distintos («TargetBitrate» y «HevcTargetBitrate»…).
struct Names {
  const wchar_t *usage, *profile, *preset, *low_latency, *rc, *target, *peak, *vbv, *vbv_fullness, *filler, *hrd,
      *frame_size, *frame_rate, *query_timeout, *force_type, *output_type, *out_profile, *out_transfer, *out_primaries,
      *full_range;
};

constexpr Names kAvc = {AMF_VIDEO_ENCODER_USAGE,
                        AMF_VIDEO_ENCODER_PROFILE,
                        AMF_VIDEO_ENCODER_QUALITY_PRESET,
                        AMF_VIDEO_ENCODER_LOWLATENCY_MODE,
                        AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
                        AMF_VIDEO_ENCODER_TARGET_BITRATE,
                        AMF_VIDEO_ENCODER_PEAK_BITRATE,
                        AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE,
                        AMF_VIDEO_ENCODER_INITIAL_VBV_BUFFER_FULLNESS,
                        AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE,
                        AMF_VIDEO_ENCODER_ENFORCE_HRD,
                        AMF_VIDEO_ENCODER_FRAMESIZE,
                        AMF_VIDEO_ENCODER_FRAMERATE,
                        AMF_VIDEO_ENCODER_QUERY_TIMEOUT,
                        AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE,
                        AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE,
                        AMF_VIDEO_ENCODER_OUTPUT_COLOR_PROFILE,
                        AMF_VIDEO_ENCODER_OUTPUT_TRANSFER_CHARACTERISTIC,
                        AMF_VIDEO_ENCODER_OUTPUT_COLOR_PRIMARIES,
                        AMF_VIDEO_ENCODER_OUTPUT_FULL_RANGE_COLOR};

constexpr Names kHevc = {AMF_VIDEO_ENCODER_HEVC_USAGE,
                         AMF_VIDEO_ENCODER_HEVC_PROFILE,
                         AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET,
                         AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE,
                         AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
                         AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE,
                         AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE,
                         AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE,
                         AMF_VIDEO_ENCODER_HEVC_INITIAL_VBV_BUFFER_FULLNESS,
                         AMF_VIDEO_ENCODER_HEVC_FILLER_DATA_ENABLE,
                         AMF_VIDEO_ENCODER_HEVC_ENFORCE_HRD,
                         AMF_VIDEO_ENCODER_HEVC_FRAMESIZE,
                         AMF_VIDEO_ENCODER_HEVC_FRAMERATE,
                         AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT,
                         AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE,
                         AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE,
                         AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PROFILE,
                         AMF_VIDEO_ENCODER_HEVC_OUTPUT_TRANSFER_CHARACTERISTIC,
                         AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PRIMARIES,
                         nullptr};  // en HEVC el rango es un enum y ya es limitado de serie

template <typename T>
bool Set(amf::AMFComponent* c, const wchar_t* name, const T& value) {
  return c->SetProperty(name, value) == AMF_OK;
}

bool SetInt(amf::AMFComponent* c, const wchar_t* name, int64_t value) {
  return Set(c, name, static_cast<amf_int64>(value));
}

// VBV de un frame, como en NvencEncoder: ningún frame pesa más de lo que el enlace pasa en 1/fps s.
void SetRate(amf::AMFComponent* c, const Names& n, uint32_t bps, uint32_t fps) {
  SetInt(c, n.target, bps);
  SetInt(c, n.peak, bps);
  SetInt(c, n.vbv, bps / (fps ? fps : 60));
}

}  // namespace

bool AmfEncoder::IsAmd(Gpu& gpu) {
  DXGI_ADAPTER_DESC1 ad{};
  return gpu.adapter && SUCCEEDED(gpu.adapter->GetDesc1(&ad)) && ad.VendorId == kAmdVendorId;
}

AmfEncoder::~AmfEncoder() {
  Stop();
  if (impl_) {
    if (impl_->encoder) impl_->encoder->Terminate();
    if (impl_->context) impl_->context->Terminate();
    delete impl_;  // suelta los punteros de AMF antes de descargar su biblioteca
    impl_ = nullptr;
  }
  if (dll_) FreeLibrary(dll_);
}

bool AmfEncoder::Init(Gpu& gpu, VideoCodec codec, UINT width, UINT height, UINT fps, UINT bitrate_bps,
                      OutputCallback callback) {
  callback_ = std::move(callback);
  width_ = width;
  height_ = height;
  fps_ = fps ? fps : 60;
  hevc_ = codec == VideoCodec::kHevc;
  const Names& n = hevc_ ? kHevc : kAvc;

  if (!IsAmd(gpu)) return false;
  dll_ = LoadLibraryW(AMF_DLL_NAME);
  if (!dll_) {
    std::fprintf(stderr, "  AMF directo: no está %ls (¿driver de AMD sin instalar?)\n", AMF_DLL_NAME);
    return false;
  }
  auto query = reinterpret_cast<AMFQueryVersion_Fn>(GetProcAddress(dll_, AMF_QUERY_VERSION_FUNCTION_NAME));
  auto init = reinterpret_cast<AMFInit_Fn>(GetProcAddress(dll_, AMF_INIT_FUNCTION_NAME));
  amf_uint64 version = 0;
  if (!query || !init || query(&version) != AMF_OK) {
    std::fprintf(stderr, "  AMF directo: la biblioteca del driver no responde\n");
    return false;
  }
  // «Latencia ultrabaja» y la espera dentro de QueryOutput llegaron con AMF 1.4.9 (drivers de 2018).
  if (version < AMF_MAKE_FULL_VERSION(1, 4, 9, 0)) {
    std::fprintf(stderr, "  AMF directo: el driver de AMD es antiguo (AMF %llu.%llu): actualízalo\n",
                 static_cast<unsigned long long>(AMF_GET_MAJOR_VERSION(version)),
                 static_cast<unsigned long long>(AMF_GET_MINOR_VERSION(version)));
    return false;
  }
  impl_ = new Impl();
  if (init(AMF_FULL_VERSION, &impl_->factory) != AMF_OK || !impl_->factory) {
    std::fprintf(stderr, "  AMF directo: no se pudo cargar la API\n");
    return false;
  }
  if (impl_->factory->CreateContext(&impl_->context) != AMF_OK ||
      impl_->context->InitDX11(gpu.device.Get()) != AMF_OK) {
    std::fprintf(stderr, "  AMF directo: no se pudo usar la GPU con Direct3D 11\n");
    return false;
  }
  if (impl_->factory->CreateComponent(impl_->context, hevc_ ? AMFVideoEncoder_HEVC : AMFVideoEncoderVCE_AVC,
                                      &impl_->encoder) != AMF_OK) {
    std::fprintf(stderr, "  AMF directo: esta GPU no ofrece %s\n", hevc_ ? "HEVC" : "H.264");
    return false;
  }
  amf::AMFComponent* e = impl_->encoder;
  // El uso va primero: rehace todos los demás ajustes a sus valores para ese uso.
  if (!SetInt(e, n.usage, hevc_ ? AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY : AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY)) {
    std::fprintf(stderr, "  AMF directo: no acepta el uso de latencia ultrabaja\n");
    return false;
  }
  SetInt(e, n.profile, hevc_ ? AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN : AMF_VIDEO_ENCODER_PROFILE_HIGH);
  // Como en NvencEncoder: por encima de 1080p60 el preset más rápido; por debajo, el equilibrado.
  const uint64_t pixel_rate = static_cast<uint64_t>(width_) * height_ * fps_;
  const bool fastest = pixel_rate > 1920ull * 1080 * 60;
  SetInt(e, n.preset,
         hevc_ ? (fastest ? AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED : AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED)
               : (fastest ? AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED : AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED));
  Set(e, n.low_latency, true);
  Set(e, n.frame_size, ::AMFConstructSize(static_cast<amf_int32>(width_), static_cast<amf_int32>(height_)));
  Set(e, n.frame_rate, ::AMFConstructRate(fps_, 1));
  SetInt(e, n.rc, hevc_ ? AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR : AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR);
  SetRate(e, n, bitrate_bps, fps_);
  SetInt(e, n.vbv_fullness, 64);  // 100 %
  Set(e, n.filler, false);        // sin relleno: con el escritorio quieto los frames son diminutos
  Set(e, n.hrd, true);
  // Sin frames clave periódicos (0 = solo el primero); si el driver no acepta 0, uno cada 1000 frames.
  if (hevc_) {
    if (!SetInt(e, AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, 0)) SetInt(e, AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, 1000);
    SetInt(e, AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, 0);
    SetInt(e, AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE, AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE_IDR_ALIGNED);
  } else {
    if (!SetInt(e, AMF_VIDEO_ENCODER_IDR_PERIOD, 0)) SetInt(e, AMF_VIDEO_ENCODER_IDR_PERIOD, 1000);
    SetInt(e, AMF_VIDEO_ENCODER_B_PIC_PATTERN, 0);
  }
  // El conversor produce BT.709 de rango limitado (converter.cpp): se declara en la VUI, como en los otros dos.
  SetInt(e, n.out_profile, AMF_VIDEO_CONVERTER_COLOR_PROFILE_709);
  SetInt(e, n.out_transfer, AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709);
  SetInt(e, n.out_primaries, AMF_COLOR_PRIMARIES_BT709);
  if (n.full_range) Set(e, n.full_range, false);
  // QueryOutput espera dentro hasta que el frame sale, sin dar vueltas.
  SetInt(e, n.query_timeout, 50);

  if (e->Init(amf::AMF_SURFACE_NV12, static_cast<amf_int32>(width_), static_cast<amf_int32>(height_)) != AMF_OK) {
    std::fprintf(stderr, "  AMF directo: no aceptó la configuración\n");
    return false;
  }

  DXGI_ADAPTER_DESC1 ad{};
  gpu.adapter->GetDesc1(&ad);
  std::wprintf(L"Codificador: AMF directo (%s), %s, latencia ultrabaja\n", ad.Description,
               fastest ? L"velocidad" : L"equilibrado");
  thread_ = std::thread([this] { Loop(); });
  return true;
}

void AmfEncoder::Submit(ID3D11Texture2D* nv12, uint64_t timestamp_us) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++submitted_frames;
    if (has_pending_) ++dropped_frames;
    pending_ = nv12;
    pending_ts_ = timestamp_us;
    has_pending_ = true;
  }
  cv_.notify_one();
}

void AmfEncoder::Stop() {
  if (!thread_.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  thread_.join();
}

void AmfEncoder::Loop() {
  while (true) {
    ComPtr<ID3D11Texture2D> texture;
    uint64_t ts = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stop_ || has_pending_; });
      if (stop_) break;
      texture = std::move(pending_);
      ts = pending_ts_;
      has_pending_ = false;
    }
    const uint64_t now = NowUs();
    if (now > ts) mailbox_wait_us += now - ts;
    ++fed_frames;
    if (!Encode(texture.Get(), ts) && (++errors_ <= 5 || errors_ % 600 == 0)) {
      std::fprintf(stderr, "aviso: AMF no pudo codificar un frame (%llu fallos)\n",
                   static_cast<unsigned long long>(errors_));
    }
  }
}

void AmfEncoder::ApplyBitrate(uint32_t bps) { SetRate(impl_->encoder, hevc_ ? kHevc : kAvc, bps, fps_); }

bool AmfEncoder::Encode(ID3D11Texture2D* texture, uint64_t timestamp_us) {
  if (const uint32_t bps = bitrate_request_.exchange(0)) ApplyBitrate(bps);
  const Names& n = hevc_ ? kHevc : kAvc;
  amf::AMFComponent* e = impl_->encoder;

  amf::AMFSurfacePtr surface;
  if (impl_->context->CreateSurfaceFromDX11Native(texture, &surface, nullptr) != AMF_OK || !surface) return false;
  surface->SetPts(static_cast<amf_pts>(timestamp_us) * 10);  // AMF cuenta en unidades de 100 ns
  surface->SetDuration(10'000'000 / fps_);
  const bool idr = keyframe_requested_.exchange(false);
  if (idr) {
    surface->SetProperty(n.force_type, static_cast<amf_int64>(hevc_ ? AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR
                                                                     : AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR));
    if (hevc_) {
      surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, true);  // VPS/SPS/PPS, como espera la tablet
    } else {
      surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
      surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
    }
  }

  const uint64_t start = NowUs();
  if (e->SubmitInput(surface) != AMF_OK) {
    if (idr) keyframe_requested_ = true;  // que lo intente el siguiente
    return false;
  }
  // Un frame en vuelo: se espera su salida aquí (QueryOutput ya espera hasta 50 ms; con drivers que no la
  // admiten devuelve al momento y se reintenta cediendo el hilo).
  amf::AMFDataPtr data;
  while (true) {
    const AMF_RESULT r = e->QueryOutput(&data);
    if (data) break;
    if ((r != AMF_OK && r != AMF_REPEAT) || NowUs() - start > 200'000) {
      if (idr) keyframe_requested_ = true;
      return false;
    }
    if (NowUs() - start > 2'000) Sleep(0);
  }
  amf::AMFBufferPtr buffer(data);
  if (!buffer) return false;
  encode_us += NowUs() - start;
  ++encode_count;
  amf_int64 type = -1;
  data->GetProperty(n.output_type, &type);
  // Los valores de IDR e I coinciden en H.264 y HEVC (0 y 1).
  const bool keyframe = type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR || type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_I;
  callback_(static_cast<const uint8_t*>(buffer->GetNative()), buffer->GetSize(), keyframe,
            static_cast<uint64_t>(data->GetPts() / 10));
  return true;
}
