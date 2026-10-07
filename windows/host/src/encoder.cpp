#include "encoder.h"

#include <chrono>

#include <codecapi.h>
#include <oleauto.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>

namespace {

void SetBool(ICodecAPI* api, const char* name, const GUID& key, bool value) {
  VARIANT v;
  VariantInit(&v);
  v.vt = VT_BOOL;
  v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
  if (FAILED(api->SetValue(&key, &v))) std::fprintf(stderr, "aviso: ajuste de códec no soportado (%s)\n", name);
}

void SetU32(ICodecAPI* api, const char* name, const GUID& key, ULONG value) {
  VARIANT v;
  VariantInit(&v);
  v.vt = VT_UI4;
  v.ulVal = value;
  if (FAILED(api->SetValue(&key, &v))) std::fprintf(stderr, "aviso: ajuste de códec no soportado (%s)\n", name);
}

// El flujo HEVC debe empezar con el VPS (NAL tipo 32) en los frames clave.
bool StartsWithHevcVps(const uint8_t* d, size_t n) {
  size_t i = 0;
  if (n >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 1) i = 4;
  else if (n >= 3 && d[0] == 0 && d[1] == 0 && d[2] == 1) i = 3;
  else return false;
  return i < n && ((d[i] >> 1) & 0x3F) == 32;
}

// El conversor produce BT.709 de rango limitado (converter.cpp). Se declara en el tipo para que el flujo
// lo lleve en la VUI: sin ella cada decodificador supone lo que quiere y algunos pintan los colores
// lavados o con otro tono.
void SetBt709(IMFMediaType* type) {
  type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
  type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
  type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
  type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
}

void ClearColor(IMFMediaType* type) {
  type->DeleteItem(MF_MT_VIDEO_PRIMARIES);
  type->DeleteItem(MF_MT_TRANSFER_FUNCTION);
  type->DeleteItem(MF_MT_YUV_MATRIX);
  type->DeleteItem(MF_MT_VIDEO_NOMINAL_RANGE);
}

}  // namespace

MfEncoder::~MfEncoder() { Stop(); }

bool MfEncoder::Init(Gpu& gpu, VideoCodec codec, UINT width, UINT height, UINT fps,
                     UINT bitrate_bps, OutputCallback callback) {
  callback_ = std::move(callback);
  codec_ = codec;
  width_ = width;
  height_ = height;
  fps_ = fps;

  UINT reset_token = 0;
  if (FAILED(MFCreateDXGIDeviceManager(&reset_token, &device_manager_)) ||
      FAILED(device_manager_->ResetDevice(gpu.device.Get(), reset_token))) {
    std::fprintf(stderr, "No se pudo crear el administrador de dispositivo DXGI\n");
    return false;
  }

  MFT_REGISTER_TYPE_INFO out_info{MFMediaType_Video,
                                  codec == VideoCodec::kHevc ? MFVideoFormat_HEVC
                                                             : MFVideoFormat_H264};
  IMFActivate** activates = nullptr;
  UINT32 count = 0;

  // Sólo los codificadores de la GPU que captura: un PC puede tener también el de otra
  // GPU (por ejemplo la AMD integrada en el procesador) y ese no puede leer las texturas
  // de esta.
  HRESULT hr = E_FAIL;
  ComPtr<IMFAttributes> enum_attrs;
  if (SUCCEEDED(MFCreateAttributes(&enum_attrs, 1))) {
    DXGI_ADAPTER_DESC1 ad{};
    gpu.adapter->GetDesc1(&ad);
    const UINT64 luid = (static_cast<UINT64>(static_cast<UINT32>(ad.AdapterLuid.HighPart)) << 32) |
                        ad.AdapterLuid.LowPart;
    enum_attrs->SetUINT64(MFT_ENUM_ADAPTER_LUID, luid);
    hr = MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                  nullptr, &out_info, enum_attrs.Get(), &activates, &count);
  }
  if (FAILED(hr) || count == 0) {
    if (activates) CoTaskMemFree(activates);
    activates = nullptr;
    count = 0;
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                   nullptr, &out_info, &activates, &count);
  }
  if (FAILED(hr) || count == 0) {
    std::fprintf(stderr, "No hay codificador por hardware para este códec\n");
    return false;
  }

  // Se prueba cada candidato hasta que uno se configure bien.
  bool ok = false;
  for (UINT32 i = 0; i < count && !ok; ++i) {
    WCHAR* name = nullptr;
    UINT32 len = 0;
    if (SUCCEEDED(activates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len))) {
      std::wprintf(L"Codificador: %s\n", name);
      CoTaskMemFree(name);
    }
    ok = SetupTransform(activates[i], bitrate_bps);
    if (!ok) ResetTransform();
  }
  for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
  CoTaskMemFree(activates);
  if (!ok) {
    std::fprintf(stderr, "Ningún codificador por hardware funcionó con esta GPU\n");
    return false;
  }

  thread_ = std::thread([this] { EventLoop(); });
  return true;
}

void MfEncoder::ResetTransform() {
  mft_.Reset();
  events_.Reset();
  codec_api_.Reset();
  sequence_header_.clear();
}

bool MfEncoder::SetupTransform(IMFActivate* activate, UINT bitrate_bps) {
  HRESULT hr = activate->ActivateObject(IID_PPV_ARGS(&mft_));
  if (FAILED(hr)) {
    std::fprintf(stderr, "  no se pudo activar el codificador: 0x%08lx\n", hr);
    return false;
  }

  ComPtr<IMFAttributes> attrs;
  if (FAILED(mft_->GetAttributes(&attrs))) {
    std::fprintf(stderr, "  el codificador no expone atributos\n");
    return false;
  }
  UINT32 is_async = 0;
  attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);
  if (!is_async) {
    std::fprintf(stderr, "  el codificador no es asíncrono: no soportado en la fase 1\n");
    return false;
  }
  attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
  attrs->SetUINT32(MF_LOW_LATENCY, TRUE);

  hr = mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                            reinterpret_cast<ULONG_PTR>(device_manager_.Get()));
  if (FAILED(hr)) {
    std::fprintf(stderr, "  el codificador no acepta texturas D3D11: 0x%08lx\n", hr);
    return false;
  }

  if (FAILED(mft_.As(&codec_api_)) || FAILED(mft_.As(&events_))) {
    std::fprintf(stderr, "  el codificador no ofrece ICodecAPI o eventos\n");
    return false;
  }
  ApplyLowLatencySettings(bitrate_bps);
  if (!ConfigureTypes(codec_)) return false;
  CaptureSequenceHeader();

  mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  return true;
}

void MfEncoder::ApplyLowLatencySettings(UINT bitrate_bps) {
  SetBool(codec_api_.Get(), "CODECAPI_AVLowLatencyMode", CODECAPI_AVLowLatencyMode, true);
  SetBool(codec_api_.Get(), "CODECAPI_AVEncCommonRealTime", CODECAPI_AVEncCommonRealTime, true);
  SetU32(codec_api_.Get(), "CODECAPI_AVEncCommonRateControlMode", CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
  SetU32(codec_api_.Get(), "CODECAPI_AVEncCommonMeanBitRate", CODECAPI_AVEncCommonMeanBitRate, bitrate_bps);
  SetU32(codec_api_.Get(), "CODECAPI_AVEncMPVDefaultBPictureCount", CODECAPI_AVEncMPVDefaultBPictureCount, 0);
  SetU32(codec_api_.Get(), "CODECAPI_AVEncMPVGOPSize", CODECAPI_AVEncMPVGOPSize, 0x7FFFFFFF);  // sin keyframes periódicos
}

bool MfEncoder::ConfigureTypes(VideoCodec codec) {
  ComPtr<IMFMediaType> out;
  MFCreateMediaType(&out);
  out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  out->SetGUID(MF_MT_SUBTYPE, codec == VideoCodec::kHevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
  out->SetUINT32(MF_MT_AVG_BITRATE, 50'000'000);  // el valor real lo fija CODECAPI_AVEncCommonMeanBitRate
  MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, width_, height_);
  MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, fps_, 1);
  MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  out->SetUINT32(MF_MT_MPEG2_PROFILE,
                 codec == VideoCodec::kHevc ? eAVEncH265VProfile_Main_420_8 : eAVEncH264VProfile_High);
  SetBt709(out.Get());
  HRESULT hr = mft_->SetOutputType(0, out.Get(), 0);
  if (FAILED(hr)) {
    // Algún codificador puede rechazar los atributos de color: mejor sin ellos que sin vídeo.
    ClearColor(out.Get());
    hr = mft_->SetOutputType(0, out.Get(), 0);
  }
  if (FAILED(hr)) {
    std::fprintf(stderr, "SetOutputType falló: 0x%08lx\n", hr);
    return false;
  }

  ComPtr<IMFMediaType> in;
  MFCreateMediaType(&in);
  in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, width_, height_);
  MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fps_, 1);
  MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  SetBt709(in.Get());
  hr = mft_->SetInputType(0, in.Get(), 0);
  if (FAILED(hr)) {
    ClearColor(in.Get());
    hr = mft_->SetInputType(0, in.Get(), 0);
  }
  if (FAILED(hr)) {
    std::fprintf(stderr, "SetInputType falló: 0x%08lx\n", hr);
    return false;
  }
  return true;
}

// Guarda las cabeceras de secuencia (VPS/SPS/PPS) por si el codificador no las
// incluye dentro de los frames clave.
void MfEncoder::CaptureSequenceHeader() {
  ComPtr<IMFMediaType> type;
  if (FAILED(mft_->GetOutputCurrentType(0, &type))) return;
  UINT32 size = 0;
  if (FAILED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) || size == 0) return;
  sequence_header_.resize(size);
  type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sequence_header_.data(), size, nullptr);
}

void MfEncoder::Submit(ID3D11Texture2D* nv12, uint64_t timestamp_us) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.texture = nv12;
    pending_.timestamp_us = timestamp_us;
    has_pending_ = true;
  }
  cv_.notify_one();
}

void MfEncoder::Stop() {
  if (!thread_.joinable()) return;
  stop_ = true;
  cv_.notify_all();
  if (mft_) mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);  // despierta GetEvent
  thread_.join();
}

void MfEncoder::EventLoop() {
  while (!stop_) {
    ComPtr<IMFMediaEvent> event;
    if (FAILED(events_->GetEvent(0, &event))) break;
    MediaEventType type = MEUnknown;
    event->GetType(&type);

    if (type == METransformNeedInput) {
      PendingFrame frame;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return has_pending_ || stop_; });
        if (stop_) break;
        frame = std::move(pending_);
        has_pending_ = false;
      }
      if (!FeedInput(frame)) break;
    } else if (type == METransformHaveOutput) {
      DrainOutput();
    } else if (type == METransformDrainComplete) {
      break;
    }
  }
}

bool MfEncoder::FeedInput(const PendingFrame& frame) {
  {
    using namespace std::chrono;
    const uint64_t now = static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
    if (now > frame.timestamp_us) mailbox_wait_us += now - frame.timestamp_us;
    ++fed_frames;
  }
  if (const uint32_t bps = bitrate_request_.exchange(0)) {
    SetU32(codec_api_.Get(), "CODECAPI_AVEncCommonMeanBitRate", CODECAPI_AVEncCommonMeanBitRate, bps);
  }
  if (keyframe_requested_.exchange(false)) {
    SetU32(codec_api_.Get(), "CODECAPI_AVEncVideoForceKeyFrame", CODECAPI_AVEncVideoForceKeyFrame, 1);
  }

  ComPtr<IMFMediaBuffer> buffer;
  HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), frame.texture.Get(), 0, FALSE,
                                         &buffer);
  if (FAILED(hr)) {
    std::fprintf(stderr, "MFCreateDXGISurfaceBuffer falló: 0x%08lx\n", hr);
    return false;
  }
  ComPtr<IMF2DBuffer> buffer2d;
  DWORD length = 0;
  if (SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->GetContiguousLength(&length))) {
    buffer->SetCurrentLength(length);
  }

  ComPtr<IMFSample> sample;
  MFCreateSample(&sample);
  sample->AddBuffer(buffer.Get());
  sample->SetSampleTime(static_cast<LONGLONG>(frame.timestamp_us) * 10);  // unidades de 100 ns
  sample->SetSampleDuration(10'000'000LL / fps_);

  hr = mft_->ProcessInput(0, sample.Get(), 0);
  if (FAILED(hr)) {
    std::fprintf(stderr, "ProcessInput falló: 0x%08lx\n", hr);
    return false;
  }
  return true;
}

void MfEncoder::DrainOutput() {
  MFT_OUTPUT_STREAM_INFO info{};
  mft_->GetOutputStreamInfo(0, &info);

  MFT_OUTPUT_DATA_BUFFER out{};
  ComPtr<IMFSample> own_sample;
  const bool provides = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                         MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
  if (!provides) {
    ComPtr<IMFMediaBuffer> buf;
    MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 1 << 20), &buf);
    MFCreateSample(&own_sample);
    own_sample->AddBuffer(buf.Get());
    out.pSample = own_sample.Get();
  }

  DWORD status = 0;
  HRESULT hr = mft_->ProcessOutput(0, 1, &out, &status);
  if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
    ComPtr<IMFMediaType> type;
    if (SUCCEEDED(mft_->GetOutputAvailableType(0, 0, &type))) mft_->SetOutputType(0, type.Get(), 0);
    CaptureSequenceHeader();
    return;
  }
  if (out.pEvents) out.pEvents->Release();
  if (FAILED(hr)) return;

  ComPtr<IMFSample> sample;
  if (provides) sample.Attach(out.pSample);  // el codificador nos cede una referencia
  else sample = own_sample;
  if (!sample) return;

  ComPtr<IMFMediaBuffer> contiguous;
  if (FAILED(sample->ConvertToContiguousBuffer(&contiguous))) return;
  BYTE* data = nullptr;
  DWORD max_len = 0, cur_len = 0;
  if (FAILED(contiguous->Lock(&data, &max_len, &cur_len))) return;

  UINT32 clean = 0;
  sample->GetUINT32(MFSampleExtension_CleanPoint, &clean);
  LONGLONG time100ns = 0;
  sample->GetSampleTime(&time100ns);
  const uint64_t ts_us = static_cast<uint64_t>(time100ns / 10);

  const bool keyframe = clean != 0;
  if (keyframe && codec_ == VideoCodec::kHevc && !sequence_header_.empty() &&
      !StartsWithHevcVps(data, cur_len)) {
    std::vector<uint8_t> merged(sequence_header_);
    merged.insert(merged.end(), data, data + cur_len);
    callback_(merged.data(), merged.size(), true, ts_us);
  } else {
    callback_(data, cur_len, keyframe, ts_us);
  }
  contiguous->Unlock();
}
