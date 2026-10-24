#include "dlp/session.h"

#include <algorithm>
#include <cmath>

namespace dlp {

namespace {

void Put16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

uint16_t Get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// Escalones de fps del gobernador, de más a menos.
constexpr uint32_t kFpsSteps[] = {120, 90, 60, 45, 30};

uint32_t StepDown(uint32_t fps) {
  for (uint32_t s : kFpsSteps) {
    if (s < fps) return s;
  }
  return fps;
}

uint32_t StepUp(uint32_t fps, uint32_t target) {
  uint32_t up = target;
  for (uint32_t s : kFpsSteps) {
    if (s > fps && s < up) up = s;
  }
  return std::min(up, target);
}

}  // namespace

std::vector<uint8_t> EncodeSessionConfig(const SessionConfig& c) {
  std::vector<uint8_t> out(kSessionConfigSize);
  out[0] = static_cast<uint8_t>(c.profile);
  out[1] = c.flags;
  out[2] = static_cast<uint8_t>(c.input);
  out[3] = static_cast<uint8_t>(c.reduced);
  Put16(out.data() + 4, c.fps);
  Put16(out.data() + 6, c.bitrate_x100kbps);
  Put16(out.data() + 8, c.max_bitrate_x100kbps);
  return out;
}

std::optional<SessionConfig> DecodeSessionConfig(const uint8_t* p, size_t size) {
  if (size < kSessionConfigSize) return std::nullopt;
  SessionConfig c;
  c.profile = p[0] <= static_cast<uint8_t>(Profile::kMinimum) ? static_cast<Profile>(p[0]) : Profile::kBalanced;
  c.flags = p[1];
  c.input = p[2] <= static_cast<uint8_t>(SessionInput::kViewOnly) ? static_cast<SessionInput>(p[2]) : SessionInput::kTouch;
  c.reduced = p[3] <= static_cast<uint8_t>(Reduced::kTablet) ? static_cast<Reduced>(p[3]) : Reduced::kNone;
  c.fps = Get16(p + 4);
  c.bitrate_x100kbps = Get16(p + 6);
  c.max_bitrate_x100kbps = Get16(p + 8);
  return c;
}

uint32_t AutoFps(uint16_t tablet_refresh_hz, uint32_t user_fps) {
  if (user_fps > 0) return std::clamp<uint32_t>(user_fps, 10, 240);
  if (tablet_refresh_hz >= 119) return 120;
  if (tablet_refresh_hz >= 89) return 90;
  return 60;
}

uint32_t AutoBitrateMbps(uint32_t width, uint32_t height, uint32_t fps, Codec codec) {
  const double bpp = codec == Codec::kHevc ? 0.08 : 0.12;
  const double mbps = static_cast<double>(width) * height * std::max<uint32_t>(fps, 1) * bpp / 1e6;
  return std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(mbps)), 20, 100);
}

Codec ResolveCodec(uint8_t user_codec, uint8_t tablet_codec) {
  const bool tablet_hevc = tablet_codec != 1;  // 0 (no lo dice), 2 o 3 (HEVC por hardware)
  if (user_codec == static_cast<uint8_t>(Codec::kH264)) return Codec::kH264;
  if (user_codec == static_cast<uint8_t>(Codec::kHevc)) return tablet_hevc ? Codec::kHevc : Codec::kH264;
  // Automático: HEVC salvo que la tablet diga que su H.264 decodifica más deprisa (3).
  return tablet_hevc && tablet_codec != 3 ? Codec::kHevc : Codec::kH264;
}

Limits CombineLimits(Profile host_profile, uint32_t target_fps, uint32_t target_bitrate_bps, bool battery_adapt,
                     const std::optional<QualityRequest>& tablet) {
  const QualityRequest p = ProfileDefaults(host_profile);
  Limits l;
  l.fps = p.max_fps ? std::min<uint32_t>(target_fps, p.max_fps) : target_fps;
  l.bitrate_bps = p.max_bitrate_bps ? std::min(target_bitrate_bps, p.max_bitrate_bps) : target_bitrate_bps;
  l.scale_percent = p.scale_percent ? p.scale_percent : 100;
  if (battery_adapt && tablet && static_cast<uint8_t>(tablet->profile) > static_cast<uint8_t>(host_profile)) {
    const uint32_t chosen = l.bitrate_bps;
    const uint32_t chosen_fps = l.fps;  // con el tope de la calidad del PC: eso no lo baja la batería
    if (tablet->max_fps) l.fps = std::min<uint32_t>(l.fps, tablet->max_fps);
    if (tablet->max_bitrate_bps) {
      l.bitrate_bps = std::max(std::min(chosen, tablet->max_bitrate_bps), static_cast<uint32_t>(chosen * 6ull / 10));
    }
    l.battery_reduced = l.fps < chosen_fps || l.bitrate_bps < chosen;
  }
  return l;
}

void RateController::Reset(uint32_t max_bps, bool usb, uint64_t now_us) {
  max_ = std::max<uint32_t>(max_bps, 1'000'000);
  usb_ = usb;
  current_ = usb ? max_ : std::min(max_, kStartBps);
  current_ = std::max(current_, min_bps());
  ceiling_ = max_;
  link_cap_ = UINT32_MAX;
  link_idle_since_us_ = 0;
  last_decrease_us_ = 0;
  last_increase_us_ = now_us;
  last_relax_us_ = now_us;
}

bool RateController::Decrease(uint64_t now_us) {
  if (usb_) return false;
  if (last_decrease_us_ && now_us - last_decrease_us_ < 1'000'000) return false;  // un recorte por segundo
  last_decrease_us_ = now_us;
  last_relax_us_ = now_us;
  ceiling_ = std::max<uint32_t>(static_cast<uint32_t>(current_ * 0.95), min_bps());
  const uint32_t target = std::max<uint32_t>(static_cast<uint32_t>(current_ * 0.85), min_bps());
  if (target == current_) return false;
  current_ = target;
  return true;
}

bool RateController::OnReport(uint32_t received, uint32_t lost, uint64_t now_us) {
  const uint32_t total = received + lost;
  // Más del 2 % perdido: el enlace no da para esta tasa. Una pérdida suelta se arregla reenviando (NACK).
  if (lost > 0 && static_cast<uint64_t>(lost) * 100 > static_cast<uint64_t>(total) * 2) return Decrease(now_us);
  if (lost > 0) return false;
  bool changed = false;
  if (now_us - last_relax_us_ >= 3'000'000 && ceiling_ < max_) {
    ceiling_ = std::min<uint32_t>({ceiling_ + 5'000'000, max_, std::max(link_cap_, min_bps())});
    last_relax_us_ = now_us;
  }
  const bool clean_for_a_while = !last_decrease_us_ || now_us - last_decrease_us_ >= 3'000'000;
  if (clean_for_a_while && now_us - last_increase_us_ >= 1'000'000 && current_ < ceiling_) {
    last_increase_us_ = now_us;
    current_ = std::min<uint32_t>(static_cast<uint32_t>(current_ * 1.1) + 1, ceiling_);
    changed = true;
  }
  return changed;
}

bool RateController::OnKeyframeRequest(uint64_t now_us) {
  if (last_decrease_us_ && now_us - last_decrease_us_ < 2'000'000) return false;
  return Decrease(now_us);
}

bool RateController::OnLinkQuality(uint32_t sent_packets, uint32_t resent_packets, bool backlog, uint64_t now_us) {
  if (usb_) return false;
  const bool many_resends = sent_packets >= 100 && static_cast<uint64_t>(resent_packets) * 100 >
                                                       static_cast<uint64_t>(sent_packets) * 3;
  if (!backlog && !many_resends) return false;
  return Decrease(now_us);
}

bool RateController::SetMax(uint32_t max_bps, uint64_t now_us) {
  max_bps = std::max<uint32_t>(max_bps, 1'000'000);
  if (max_bps == max_) return false;
  const bool up = max_bps > max_;
  const bool clean = !last_decrease_us_ || now_us - last_decrease_us_ >= 3'000'000;
  max_ = max_bps;
  ceiling_ = up ? max_ : std::min(ceiling_, max_);
  current_ = (up && (usb_ || clean)) ? max_ : std::clamp(current_, min_bps(), max_);
  current_ = std::min(current_, std::max(link_cap_, min_bps()));
  ceiling_ = std::min(ceiling_, std::max(link_cap_, min_bps()));
  return true;
}

bool RateController::OnLinkLoad(uint32_t link_bps, double busy_fraction, uint64_t now_us) {
  if (busy_fraction > 0.8 && link_bps > 0) {
    link_idle_since_us_ = 0;
    if (last_decrease_us_ && now_us - last_decrease_us_ < 1'000'000) return false;
    const uint32_t target = std::clamp<uint32_t>(static_cast<uint32_t>(link_bps * 0.8), min_bps(), current_);
    link_cap_ = target;
    ceiling_ = target;
    last_decrease_us_ = now_us;
    last_relax_us_ = now_us;
    if (target == current_) return false;
    current_ = target;
    return true;
  }
  if (busy_fraction < 0.5 && link_cap_ != UINT32_MAX) {
    if (!link_idle_since_us_) link_idle_since_us_ = now_us;
    if (now_us - link_idle_since_us_ >= 30'000'000) {
      link_idle_since_us_ = now_us;
      link_cap_ = link_cap_ >= max_ ? UINT32_MAX : static_cast<uint32_t>(link_cap_ * 1.1);
    }
  } else {
    link_idle_since_us_ = 0;
  }
  return false;
}

void FpsGovernor::Reset(uint32_t target_fps) {
  target_ = std::max<uint32_t>(target_fps, 1);
  fps_ = target_;
  slow_seconds_ = clean_seconds_ = 0;
  wait_seconds_ = 10;
  since_up_ = -1;
}

bool FpsGovernor::Update(uint32_t submitted, uint32_t dropped, uint32_t tablet_decode_us) {
  if (submitted < 10) {  // escritorio casi quieto: no dice nada de la carga
    slow_seconds_ = 0;
    return false;
  }
  if (since_up_ >= 0 && ++since_up_ > 60) {  // la subida aguantó: la próxima prueba vuelve a ser rápida
    since_up_ = -1;
    wait_seconds_ = 10;
  }
  const bool tablet_slow = tablet_decode_us && static_cast<uint64_t>(tablet_decode_us) * fps_ > 900'000;
  if (dropped * 10 > submitted || tablet_slow) {
    clean_seconds_ = 0;
    if (++slow_seconds_ < 3) return false;
    slow_seconds_ = 0;
    const uint32_t down = StepDown(fps_);
    if (down == fps_) return false;
    if (since_up_ >= 0 && since_up_ <= 15) wait_seconds_ = std::min(wait_seconds_ * 2, 160);
    since_up_ = -1;
    fps_ = down;
    return true;
  }
  slow_seconds_ = 0;
  const uint32_t up = StepUp(fps_, target_);
  const bool tablet_room = !tablet_decode_us || static_cast<uint64_t>(tablet_decode_us) * up < 850'000;
  if (dropped > 0 || fps_ >= target_ || !tablet_room) {
    clean_seconds_ = 0;
    return false;
  }
  if (++clean_seconds_ < wait_seconds_) return false;
  clean_seconds_ = 0;
  fps_ = up;
  since_up_ = 0;
  return true;
}

}  // namespace dlp
