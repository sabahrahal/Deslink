#include "nvenc_encoder.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#include "../third_party/nvenc/nvEncodeAPI.h"

struct NvencEncoder::Api : NV_ENCODE_API_FUNCTION_LIST {};
struct NvencEncoder::Params {
  NV_ENC_INITIALIZE_PARAMS init;
  NV_ENC_CONFIG config;
};

namespace {

constexpr uint32_t kNvidiaVendorId = 0x10DE;

uint64_t NowUs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// VBV de un frame: ningún frame pesa más de lo que el enlace pasa en 1/fps s, así que tampoco tarda más en llegar.
// Los frames clave salen algo más borrosos y se afinan en los siguientes.
void SetRate(NV_ENC_CONFIG* c, uint32_t bps, uint32_t fps) {
  c->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
  c->rcParams.averageBitRate = bps;
  c->rcParams.maxBitRate = bps;
  c->rcParams.vbvBufferSize = bps / (fps ? fps : 60);
  c->rcParams.vbvInitialDelay = c->rcParams.vbvBufferSize;
}

// El conversor produce BT.709 de rango limitado (converter.cpp): se declara en la VUI, como hace MfEncoder.
void SetBt709(NV_ENC_CONFIG_H264_VUI_PARAMETERS* vui) {
  vui->videoSignalTypePresentFlag = 1;
  vui->videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
  vui->videoFullRangeFlag = 0;
  vui->colourDescriptionPresentFlag = 1;
  vui->colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
  vui->transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
  vui->colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
}

}  // namespace

bool NvencEncoder::IsNvidia(Gpu& gpu) {
  DXGI_ADAPTER_DESC1 ad{};
  return gpu.adapter && SUCCEEDED(gpu.adapter->GetDesc1(&ad)) && ad.VendorId == kNvidiaVendorId;
}

NvencEncoder::~NvencEncoder() {
  Stop();
  Release();
}

bool NvencEncoder::Init(Gpu& gpu, VideoCodec codec, UINT width, UINT height, UINT fps, UINT bitrate_bps,
                        OutputCallback callback) {
  callback_ = std::move(callback);
  width_ = width;
  height_ = height;
  fps_ = fps ? fps : 60;

  if (!IsNvidia(gpu)) return false;
  dll_ = LoadLibraryW(L"nvEncodeAPI64.dll");
  if (!dll_) {
    std::fprintf(stderr, "  NVENC directo: no está nvEncodeAPI64.dll (¿driver de NVIDIA sin instalar?)\n");
    return false;
  }
  using GetMaxVersion = NVENCSTATUS(NVENCAPI*)(uint32_t*);
  using CreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
  auto get_max = reinterpret_cast<GetMaxVersion>(GetProcAddress(dll_, "NvEncodeAPIGetMaxSupportedVersion"));
  auto create = reinterpret_cast<CreateInstance>(GetProcAddress(dll_, "NvEncodeAPICreateInstance"));
  uint32_t max_version = 0;
  if (!get_max || !create || get_max(&max_version) != NV_ENC_SUCCESS) {
    std::fprintf(stderr, "  NVENC directo: la biblioteca del driver no responde\n");
    return false;
  }
  const uint32_t needed = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
  if (max_version < needed) {
    std::fprintf(stderr, "  NVENC directo: el driver de NVIDIA es antiguo (API %u.%u, hace falta %u.%u): actualízalo\n",
                 max_version >> 4, max_version & 0xF, NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
    return false;
  }
  api_ = new Api();
  std::memset(static_cast<NV_ENCODE_API_FUNCTION_LIST*>(api_), 0, sizeof(NV_ENCODE_API_FUNCTION_LIST));
  api_->version = NV_ENCODE_API_FUNCTION_LIST_VER;
  if (create(api_) != NV_ENC_SUCCESS || !api_->nvEncOpenEncodeSessionEx) {
    std::fprintf(stderr, "  NVENC directo: no se pudo cargar la API\n");
    return false;
  }

  NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
  open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
  open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
  open.device = gpu.device.Get();
  open.apiVersion = NVENCAPI_VERSION;
  NVENCSTATUS st = api_->nvEncOpenEncodeSessionEx(&open, &session_);
  if (st != NV_ENC_SUCCESS) {
    // Las GeForce limitan las sesiones abiertas a la vez (otro programa grabando, por ejemplo).
    std::fprintf(stderr, "  NVENC directo: no se pudo abrir la sesión (%d)\n", static_cast<int>(st));
    session_ = nullptr;
    return false;
  }

  const GUID codec_guid = codec == VideoCodec::kHevc ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
  // Como en MfEncoder: por encima de 1080p60 el preset más rápido (a 2944x1840 a 60 fps cualquier otro va justo);
  // por debajo hay holgura para uno algo mejor.
  const uint64_t pixel_rate = static_cast<uint64_t>(width_) * height_ * fps_;
  const GUID preset = pixel_rate > 1920ull * 1080 * 60 ? NV_ENC_PRESET_P1_GUID : NV_ENC_PRESET_P3_GUID;
  NV_ENC_PRESET_CONFIG preset_config{};
  preset_config.version = NV_ENC_PRESET_CONFIG_VER;
  preset_config.presetCfg.version = NV_ENC_CONFIG_VER;
  st = api_->nvEncGetEncodePresetConfigEx(session_, codec_guid, preset, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY,
                                          &preset_config);
  if (st != NV_ENC_SUCCESS) {
    std::fprintf(stderr, "  NVENC directo: esta GPU no ofrece %s (%d)\n", codec == VideoCodec::kHevc ? "HEVC" : "H.264",
                 static_cast<int>(st));
    return false;
  }

  params_ = new Params();
  NV_ENC_CONFIG& c = params_->config;
  c = preset_config.presetCfg;
  c.version = NV_ENC_CONFIG_VER;
  c.gopLength = NVENC_INFINITE_GOPLENGTH;  // sin frames clave periódicos: solo cuando la tablet los pide
  c.frameIntervalP = 1;                    // sin B-frames
  SetRate(&c, bitrate_bps, fps_);
  if (codec == VideoCodec::kHevc) {
    c.profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
    c.encodeCodecConfig.hevcConfig.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    c.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;  // VPS/SPS/PPS en cada frame clave, como espera la tablet
    SetBt709(&c.encodeCodecConfig.hevcConfig.hevcVUIParameters);
  } else {
    c.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
    c.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    c.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    SetBt709(&c.encodeCodecConfig.h264Config.h264VUIParameters);
  }

  NV_ENC_INITIALIZE_PARAMS& p = params_->init;
  p.version = NV_ENC_INITIALIZE_PARAMS_VER;
  p.encodeGUID = codec_guid;
  p.presetGUID = preset;
  p.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
  p.encodeWidth = width_;
  p.encodeHeight = height_;
  p.darWidth = width_;
  p.darHeight = height_;
  p.frameRateNum = fps_;
  p.frameRateDen = 1;
  p.enablePTD = 1;
  p.enableEncodeAsync = 0;  // síncrono: un frame en vuelo y nvEncLockBitstream espera a que acabe
  p.encodeConfig = &c;
  st = api_->nvEncInitializeEncoder(session_, &p);
  if (st != NV_ENC_SUCCESS) {
    std::fprintf(stderr, "  NVENC directo: no aceptó la configuración (%d)\n", static_cast<int>(st));
    return false;
  }

  NV_ENC_CREATE_BITSTREAM_BUFFER bs{};
  bs.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
  st = api_->nvEncCreateBitstreamBuffer(session_, &bs);
  if (st != NV_ENC_SUCCESS) {
    std::fprintf(stderr, "  NVENC directo: no se pudo crear el búfer de salida (%d)\n", static_cast<int>(st));
    return false;
  }
  bitstream_ = bs.bitstreamBuffer;

  DXGI_ADAPTER_DESC1 ad{};
  gpu.adapter->GetDesc1(&ad);
  std::wprintf(L"Codificador: NVENC directo (%s), %s, latencia ultrabaja\n", ad.Description,
               pixel_rate > 1920ull * 1080 * 60 ? L"P1" : L"P3");
  thread_ = std::thread([this] { Loop(); });
  return true;
}

void NvencEncoder::Submit(ID3D11Texture2D* nv12, uint64_t timestamp_us) {
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

void NvencEncoder::Stop() {
  if (!thread_.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  thread_.join();
}

void NvencEncoder::Release() {
  if (api_ && session_) {
    for (auto& r : registered_) api_->nvEncUnregisterResource(session_, r.handle);
    if (bitstream_) api_->nvEncDestroyBitstreamBuffer(session_, bitstream_);
    api_->nvEncDestroyEncoder(session_);
  }
  registered_.clear();
  bitstream_ = nullptr;
  session_ = nullptr;
  delete params_;
  params_ = nullptr;
  delete api_;
  api_ = nullptr;
  if (dll_) FreeLibrary(dll_);
  dll_ = nullptr;
}

void NvencEncoder::Loop() {
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
      std::fprintf(stderr, "aviso: NVENC no pudo codificar un frame (%llu fallos)\n",
                   static_cast<unsigned long long>(errors_));
    }
  }
}

void* NvencEncoder::RegisterTexture(ID3D11Texture2D* texture) {
  for (auto& r : registered_) {
    if (r.texture.Get() == texture) return r.handle;
  }
  D3D11_TEXTURE2D_DESC desc{};
  texture->GetDesc(&desc);
  NV_ENC_REGISTER_RESOURCE reg{};
  reg.version = NV_ENC_REGISTER_RESOURCE_VER;
  reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
  reg.width = desc.Width;
  reg.height = desc.Height;
  reg.pitch = 0;
  reg.resourceToRegister = texture;
  reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
  reg.bufferUsage = NV_ENC_INPUT_IMAGE;
  if (api_->nvEncRegisterResource(session_, &reg) != NV_ENC_SUCCESS) return nullptr;
  // El anillo del conversor tiene unas pocas texturas fijas; si crece (no debería), se suelta la más vieja.
  if (registered_.size() >= 64) {
    api_->nvEncUnregisterResource(session_, registered_.front().handle);
    registered_.erase(registered_.begin());
  }
  registered_.push_back({texture, reg.registeredResource});
  return reg.registeredResource;
}

void NvencEncoder::ApplyBitrate(uint32_t bps) {
  SetRate(&params_->config, bps, fps_);
  NV_ENC_RECONFIGURE_PARAMS r{};
  r.version = NV_ENC_RECONFIGURE_PARAMS_VER;
  r.reInitEncodeParams = params_->init;
  r.reInitEncodeParams.encodeConfig = &params_->config;
  if (api_->nvEncReconfigureEncoder(session_, &r) != NV_ENC_SUCCESS) {
    std::fprintf(stderr, "aviso: NVENC no aceptó la tasa nueva (%.1f Mbps)\n", bps / 1e6);
  }
}

bool NvencEncoder::Encode(ID3D11Texture2D* texture, uint64_t timestamp_us) {
  if (const uint32_t bps = bitrate_request_.exchange(0)) ApplyBitrate(bps);
  void* reg = RegisterTexture(texture);
  if (!reg) return false;

  NV_ENC_MAP_INPUT_RESOURCE map{};
  map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
  map.registeredResource = reg;
  if (api_->nvEncMapInputResource(session_, &map) != NV_ENC_SUCCESS) return false;

  const uint64_t start = NowUs();
  NV_ENC_PIC_PARAMS pic{};
  pic.version = NV_ENC_PIC_PARAMS_VER;
  pic.inputWidth = width_;
  pic.inputHeight = height_;
  pic.inputBuffer = map.mappedResource;
  pic.bufferFmt = map.mappedBufferFmt;
  pic.outputBitstream = bitstream_;
  pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
  pic.inputTimeStamp = timestamp_us;
  pic.inputDuration = 1'000'000 / fps_;
  if (keyframe_requested_.exchange(false)) pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
  NVENCSTATUS st = api_->nvEncEncodePicture(session_, &pic);
  bool ok = st == NV_ENC_SUCCESS;
  if (ok) {
    NV_ENC_LOCK_BITSTREAM lock{};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = bitstream_;
    lock.doNotWait = 0;
    ok = api_->nvEncLockBitstream(session_, &lock) == NV_ENC_SUCCESS;
    if (ok) {
      const uint64_t done = NowUs();
      encode_us += done - start;
      ++encode_count;
      const bool keyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
      callback_(static_cast<const uint8_t*>(lock.bitstreamBufferPtr), lock.bitstreamSizeInBytes, keyframe,
                lock.outputTimeStamp);
      api_->nvEncUnlockBitstream(session_, bitstream_);
    }
  }
  if (!ok && (pic.encodePicFlags & NV_ENC_PIC_FLAG_FORCEIDR)) keyframe_requested_ = true;  // que lo intente el siguiente
  api_->nvEncUnmapInputResource(session_, map.mappedResource);
  return ok;
}
