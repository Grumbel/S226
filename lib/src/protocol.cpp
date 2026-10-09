#include "s226/protocol.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace s226::protocol {

namespace {

uint8_t u8(int v) { return static_cast<uint8_t>(std::clamp(v, 0, 255)); }

int be16(std::span<const uint8_t> v, size_t i) { return v[i] << 8 | v[i + 1]; }
int le16(std::span<const uint8_t> v, size_t i) { return v[i] | v[i + 1] << 8; }
uint32_t le32(std::span<const uint8_t> v, size_t i) {
  return static_cast<uint32_t>(v[i] | v[i + 1] << 8 | v[i + 2] << 16) |
         static_cast<uint32_t>(v[i + 3]) << 24;
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

} // namespace

std::array<uint8_t, 16> uuidToAtt(std::string_view uuid) {
  std::array<uint8_t, 16> big{};
  size_t n = 0;
  for (size_t i = 0; i < uuid.size();) {
    if (uuid[i] == '-') {
      ++i;
      continue;
    }
    if (i + 1 >= uuid.size() || n >= big.size()) {
      throw std::invalid_argument("bad UUID");
    }
    int hi = hexValue(uuid[i]);
    int lo = hexValue(uuid[i + 1]);
    if (hi < 0 || lo < 0) throw std::invalid_argument("bad UUID");
    big[n++] = static_cast<uint8_t>(hi << 4 | lo);
    i += 2;
  }
  if (n != big.size()) throw std::invalid_argument("bad UUID");
  std::reverse(big.begin(), big.end());
  return big;
}

Bytes bindPacket(const std::tm& t) {
  const int year = t.tm_year + 1900;
  // a1 00 00 00 | year_be | mon day hour min sec | profile bytes from H-Band
  return {0xA1,
          0x00,
          0x00,
          0x00,
          static_cast<uint8_t>(year >> 8),
          static_cast<uint8_t>(year & 0xFF),
          static_cast<uint8_t>(t.tm_mon + 1),
          static_cast<uint8_t>(t.tm_mday),
          static_cast<uint8_t>(t.tm_hour),
          static_cast<uint8_t>(t.tm_min),
          static_cast<uint8_t>(t.tm_sec),
          0x01,
          0x01,
          0x04,
          0x00,
          0x00,
          0x00,
          0x00,
          0x00,
          0x00};
}

Bytes bindPacketNow() {
  std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  return bindPacket(local);
}

Bytes keepalive() { return {0xD8, 0x00}; }
Bytes heartRateStart() { return {0xD0, 0x01}; }
Bytes heartRateStop() { return {0xD0, 0x00}; }
Bytes bloodPressureStart() { return {0x90, 0x01, 0x00}; }
Bytes bloodPressureStop() { return {0x90, 0x00, 0x00}; }

std::optional<HeartRateSample> decodeHeartRate(std::span<const uint8_t> v) {
  if (v.size() < 2 || v[0] != 0xD0) return std::nullopt;
  HeartRateSample s;
  s.bpm = v[1];
  if (v[1] == 0x01 &&
      std::all_of(v.begin() + 2, v.end(), [](uint8_t b) { return b == 0; })) {
    s.bpm = 0;
    s.sessionEnded = true;
  }
  return s;
}

std::optional<BloodPressureSample> decodeBloodPressure(std::span<const uint8_t> v) {
  if (v.size() < 4 || v[0] != 0x90) return std::nullopt;
  BloodPressureSample s;
  if (v[1] == 0x01 && v[2] == 0x00 && v[3] == 0x00) {
    s.stopped = true;
    return s;
  }
  s.percent = v[3];
  if (v[3] >= 100 && v[1] != 0 && v[2] != 0) {
    s.systolic = v[1];
    s.diastolic = v[2];
    s.done = true;
  }
  return s;
}

std::optional<ActivityTotals> decodeActivity(std::span<const uint8_t> v) {
  if (v.size() < 14 || v[0] != 0xD8) return std::nullopt;
  return ActivityTotals{le32(v, 2), le32(v, 6), le32(v, 10)};
}

std::optional<DeviceInfo> decodeBindReply(std::span<const uint8_t> v) {
  if (v.size() < 9 || v[0] != 0xA1) return std::nullopt;
  char version[16];
  std::snprintf(version, sizeof version, "%02x.%02x.%02x", v[6], v[7], v[8]);
  return DeviceInfo{v[3], v[3] == 1 || v[3] == 6, be16(v, 4), version};
}

Bytes batteryRead() { return {0xA0, 0x00}; }

std::optional<Battery> decodeBattery(std::span<const uint8_t> v) {
  if (v.size() < 5 || v[0] != 0xA0) return std::nullopt;
  Battery b;
  if (v[2] & 0x80) b.percent = v[2] & 0x7F;
  b.level = v[4];
  return b;
}

Bytes smsAlert() { return {0xC1, 0x01, 0x01, 0x01}; }
Bytes callAlert() { return {0xC1, 0x01, 0x06, 0x00}; }
Bytes callEnd() { return {0xC1, 0x00, 0x00, 0x00}; }

Bytes messageSwitchesRead() { return {0xAD, 0x02}; }

Bytes messageSwitchesWrite(const MessageSwitches& m) {
  Bytes b{0xAD, 0x01};
  b.insert(b.end(), m.state.begin(), m.state.end());
  return b;
}

std::optional<MessageSwitches> decodeMessageSwitches(std::span<const uint8_t> v) {
  MessageSwitches m;
  if (v.size() < 2 + m.state.size() || v[0] != 0xAD) return std::nullopt;
  std::copy_n(v.begin() + 2, m.state.size(), m.state.begin());
  return m;
}

namespace {

std::vector<Bytes> contentPackets(std::string_view text, MessageType type, uint8_t flag) {
  constexpr size_t kChunk = 14;
  constexpr size_t kMaxPackets = 255;
  text = text.substr(0, kChunk * kMaxPackets);
  const size_t total = std::max<size_t>(1, (text.size() + kChunk - 1) / kChunk);
  std::vector<Bytes> packets;
  for (size_t i = 0; i < total; ++i) {
    const std::string_view chunk = text.substr(std::min(text.size(), i * kChunk), kChunk);
    Bytes p{0xC2,
            static_cast<uint8_t>(type),
            static_cast<uint8_t>(chunk.size()),
            static_cast<uint8_t>(total),
            static_cast<uint8_t>(i + 1),
            flag};
    p.insert(p.end(), chunk.begin(), chunk.end());
    p.resize(20, 0);
    packets.push_back(std::move(p));
  }
  return packets;
}

} // namespace

std::vector<Bytes> messagePackets(std::string_view text, MessageType type) {
  return contentPackets(text, type, 0x02);
}

std::vector<Bytes> callerPackets(std::string_view name) {
  return contentPackets(name, MessageType::Call, 0x01);
}

Bytes sedentaryRead() { return {0xE1, 0, 0, 0, 0, 0, 0x02}; }

Bytes sedentaryWrite(const SedentaryReminder& r) {
  return {0xE1,           u8(r.startHour),       u8(r.startMinute), u8(r.endHour),
          u8(r.endMinute), u8(r.intervalMinutes), u8(r.enabled ? 1 : 0)};
}

std::optional<SedentaryReminder> decodeSedentary(std::span<const uint8_t> v) {
  if (v.size() < 8 || v[0] != 0xE1 || v[1] != 0x01) return std::nullopt;
  return SedentaryReminder{v[7] != 0, v[2], v[3], v[4], v[5], v[6]};
}

Bytes heartRateAlarmRead() { return {0xAC, 0x00, 0x00, 0x02}; }

Bytes heartRateAlarmWrite(const HeartRateAlarm& a) {
  return {0xAC, u8(a.high), u8(a.low), u8(a.enabled ? 1 : 0)};
}

std::optional<HeartRateAlarm> decodeHeartRateAlarm(std::span<const uint8_t> v) {
  if (v.size() < 4 || v[0] != 0xAC) return std::nullopt;
  return HeartRateAlarm{v[3] != 0, v[1], v[2]};
}

namespace {

Bytes padded(Bytes b) {
  b.resize(20, 0);
  return b;
}

} // namespace

Bytes brightnessRead() { return padded({0xB1, 0x02}); }

Bytes brightnessWrite(const Brightness& b) {
  return padded({0xB1, 0x01, u8(b.startHour), u8(b.startMinute), u8(b.endHour),
                 u8(b.endMinute), u8(b.level), u8(b.otherLevel),
                 u8(b.automatic ? 1 : 2)});
}

std::optional<Brightness> decodeBrightness(std::span<const uint8_t> v) {
  if (v.size() < 11 || v[0] != 0xB1 || v[1] != 0x01) return std::nullopt;
  return Brightness{v[9] == 1, v[3], v[4], v[5], v[6], v[7], v[8], v[10]};
}

Brightness automaticBrightness() { return Brightness{true, 22, 0, 8, 0, 2, 4}; }

Brightness manualBrightness(int level) { return Brightness{false, 0, 0, 23, 59, level, level}; }

Bytes countdownRead() { return {0xB2, 0x02}; }

Bytes countdownWrite(int seconds, bool showOnWatch) {
  const int s = std::clamp(seconds, 0, 0xFFFFFF);
  return {0xB2,
          0x01,
          0x00,
          static_cast<uint8_t>(s),
          static_cast<uint8_t>(s >> 8),
          static_cast<uint8_t>(s >> 16),
          u8(showOnWatch ? 1 : 0)};
}

std::optional<Countdown> decodeCountdown(std::span<const uint8_t> v) {
  if (v.size() < 8 || v[0] != 0xB2 || v[2] != 0x01) return std::nullopt;
  return Countdown{v[4] | v[5] << 8 | v[6] << 16, v[7] != 0};
}

Bytes watchFaceRead() { return padded({0xC7, 0x02}); }
Bytes watchFaceWrite(int style) { return padded({0xC7, 0x01, u8(style)}); }

std::optional<int> decodeWatchFace(std::span<const uint8_t> v) {
  if (v.size() < 4 || v[0] != 0xC7 || v[2] != 0x01) return std::nullopt;
  return v[3];
}

Bytes alarmsRead() { return padded({0xB9, 0x02}); }

namespace {

Bytes alarmPacket(uint8_t op, const Alarm& a) {
  const int year = a.days ? 0 : a.year;
  return padded({0xB9, op, u8(a.id), u8(a.hour), u8(a.minute), u8(a.enabled ? 1 : 0), a.days,
                 0x00, static_cast<uint8_t>(year), static_cast<uint8_t>(year >> 8),
                 u8(a.days ? 0 : a.month), u8(a.days ? 0 : a.day)});
}

} // namespace

Bytes alarmWrite(const Alarm& a) { return alarmPacket(0x01, a); }
Bytes alarmDelete(const Alarm& a) { return alarmPacket(0x00, a); }

std::optional<AlarmFrame> decodeAlarmFrame(std::span<const uint8_t> v) {
  if (v.size() < 15 || v[0] != 0xB9) return std::nullopt;
  AlarmFrame f;
  f.ok = v[1] == 0x01;
  f.index = v[2];
  f.count = v[3];
  f.alarm.id = v[5];
  f.alarm.hour = v[6];
  f.alarm.minute = v[7];
  f.alarm.enabled = v[8] != 0;
  f.alarm.days = v[9];
  f.alarm.year = le16(v, 11);
  f.alarm.month = v[13];
  f.alarm.day = v[14];
  return f;
}

Bytes screenOnTimeRead() { return {0xB4, 0x02}; }
Bytes screenOnTimeWrite(int seconds) { return {0xB4, 0x01, u8(seconds)}; }

std::optional<ScreenOnTime> decodeScreenOnTime(std::span<const uint8_t> v) {
  if (v.size() < 6 || v[0] != 0xB4 || v[1] != 0x01) return std::nullopt;
  return ScreenOnTime{v[3], v[4], v[5]};
}

Bytes personInfoWrite(const PersonInfo& p) {
  const int goal = std::clamp(p.stepGoal, 0, 0xFFFF);
  const int sleep = std::clamp(p.sleepGoalMinutes, 0, 0xFFFF);
  return {0xA3,
          u8(p.heightCm),
          u8(p.weightKg),
          u8(p.age),
          u8(p.male ? 1 : 0),
          static_cast<uint8_t>(goal >> 8),
          static_cast<uint8_t>(goal),
          static_cast<uint8_t>(sleep >> 8),
          static_cast<uint8_t>(sleep)};
}

bool isPersonInfoAck(std::span<const uint8_t> v) {
  return v.size() >= 2 && v[0] == 0xA3 && v[1] == 0x01;
}

Bytes watchFeaturesRead() {
  Bytes b(20, 0);
  b[0] = 0xB8;
  b[1] = 0x02;
  return b;
}

Bytes watchFeaturesWrite(const WatchFeatures& f) {
  Bytes b(f.raw.begin(), f.raw.end());
  b[0] = 0xB8;
  b[1] = 0x01;
  b[19] = 0x00; // package 1
  return b;
}

std::optional<WatchFeatures> decodeWatchFeatures(std::span<const uint8_t> v) {
  WatchFeatures f;
  if (v.size() < f.raw.size() || v[0] != 0xB8 || v[19] != 0) return std::nullopt;
  std::copy_n(v.begin(), f.raw.size(), f.raw.begin());
  return f;
}

Bytes historyRead(int daysAgo, int firstSlot) {
  return {0xD1, static_cast<uint8_t>(firstSlot), static_cast<uint8_t>(firstSlot >> 8),
          u8(daysAgo)};
}

std::optional<HistorySlot> decodeHistorySlot(std::span<const uint8_t> v) {
  if (v.size() < 20 || v[0] != 0xD1) return std::nullopt;
  HistorySlot s;
  s.index = le16(v, 1);
  s.count = le16(v, 3);
  s.hour = v[5] & 0x1F;
  s.daysAgo = v[5] >> 5;
  s.calories = be16(v, 6);
  s.distanceMeters = be16(v, 8);
  s.steps = be16(v, 10);
  s.activity = be16(v, 12);
  s.heartRate = v[17];
  s.minute = v[19];
  return s;
}

Bytes workoutRead(int slot) { return {0xD4, u8(slot)}; }

std::optional<WorkoutFrame> decodeWorkoutFrame(std::span<const uint8_t> v) {
  if (v.size() < 6 || v[0] != 0xD4) return std::nullopt;
  return WorkoutFrame{le16(v, 1), le16(v, 3), v[5]};
}

std::optional<Workout> decodeWorkout(const std::vector<Bytes>& frames) {
  // Three header frames, then one frame per minute. The header payloads
  // form one record:
  //   00 | start: year u16le mon day h m s | end: same |
  //   steps u32le | distance u32le | calories u32le | activity u32le |
  //   minutes u16le | ...
  constexpr size_t kHeaderFrames = 3;
  constexpr size_t kPayload = 6;
  if (frames.size() < kHeaderFrames) return std::nullopt;
  Bytes header;
  for (size_t i = 0; i < kHeaderFrames; ++i) {
    const Bytes& f = frames[i];
    if (f.size() < 20 || f[0] != 0xD4) return std::nullopt;
    header.insert(header.end(), f.begin() + kPayload, f.end());
  }
  auto dateTime = [&](size_t i) {
    return DateTime{le16(header, i), header[i + 2], header[i + 3],
                    header[i + 4],   header[i + 5], header[i + 6]};
  };
  Workout w;
  w.start = dateTime(1);
  w.end = dateTime(8);
  w.steps = static_cast<int>(le32(header, 15));
  w.distanceMeters = static_cast<int>(le32(header, 19));
  w.calories = static_cast<int>(le32(header, 23));
  w.activity = static_cast<int>(le32(header, 27));
  const size_t minutes = static_cast<size_t>(le16(header, 31));
  for (size_t i = kHeaderFrames; i < frames.size() && w.minutes.size() < minutes; ++i) {
    const Bytes& f = frames[i];
    if (f.size() < kPayload + 9 || f[0] != 0xD4) return std::nullopt;
    std::span<const uint8_t> m(f.data() + kPayload, f.size() - kPayload);
    w.minutes.push_back({m[0], le16(m, 1), le16(m, 3), le16(m, 5), le16(m, 7)});
  }
  return w;
}

std::vector<Bytes> nowPlayingPackets(const NowPlaying& info) {
  // Inner payload: F0 <len> + TLVs (type, len, data…).
  Bytes inner;
  auto appendTlv = [&](uint8_t type, std::string_view data) {
    if (data.size() > 255) data = data.substr(0, 255);
    inner.push_back(type);
    inner.push_back(static_cast<uint8_t>(data.size()));
    inner.insert(inner.end(), data.begin(), data.end());
  };
  appendTlv(0xA0, info.album);
  appendTlv(0xA1, info.title);
  appendTlv(0xA2, info.artist);
  inner.push_back(0xA3);
  inner.push_back(0x01);
  inner.push_back(info.playing ? 1 : 0);
  inner.push_back(0xA4);
  inner.push_back(0x01);
  inner.push_back(u8(info.volume));

  Bytes body{0xF0, u8(static_cast<int>(std::min<size_t>(inner.size(), 255)))};
  // F0 length is one byte; if inner is longer, still send it (watch may
  // only use the declared prefix — keep payloads short in practice).
  body.insert(body.end(), inner.begin(), inner.end());

  constexpr size_t kChunk = 16;
  const size_t total = std::max<size_t>(1, (body.size() + kChunk - 1) / kChunk);
  std::vector<Bytes> packets;
  packets.reserve(total);
  for (size_t i = 0; i < total; ++i) {
    Bytes p{0x99, 0x01, u8(static_cast<int>(i + 1)), u8(static_cast<int>(total))};
    const size_t off = i * kChunk;
    const size_t n = std::min(kChunk, body.size() - std::min(body.size(), off));
    if (off < body.size()) p.insert(p.end(), body.begin() + static_cast<std::ptrdiff_t>(off),
                                    body.begin() + static_cast<std::ptrdiff_t>(off + n));
    p.resize(20, 0);
    packets.push_back(std::move(p));
  }
  return packets;
}

std::optional<MusicAction> decodeMusicControl(std::span<const uint8_t> value) {
  // 01 01 01 <action>
  if (value.size() < 4 || value[0] != 0x01 || value[1] != 0x01 || value[2] != 0x01)
    return std::nullopt;
  switch (value[3]) {
  case 1: return MusicAction::Next;
  case 2: return MusicAction::PlayPause;
  case 4: return MusicAction::Previous;
  default: return std::nullopt;
  }
}

const char* toString(MusicAction action) {
  switch (action) {
  case MusicAction::Next: return "next";
  case MusicAction::PlayPause: return "play-pause";
  case MusicAction::Previous: return "previous";
  }
  return "unknown";
}

// ---- Sleep (0xE0) -----------------------------------------------------

Bytes sleepRead(int daysAgo) { return {0xE0, u8(daysAgo)}; }

std::optional<SleepFrame> decodeSleepFrame(std::span<const uint8_t> v) {
  if (v.size() < 4 || v[0] != 0xE0) return std::nullopt;
  SleepFrame f;
  f.packetIndex = v[1];
  f.byte2 = v[2];
  f.dayIndex = v[3];
  if (v.size() > 4)
    f.payload.assign(v.begin() + 4, v.end());
  return f;
}

namespace {

SleepTime readTimeBean(std::span<const uint8_t> b, size_t off) {
  // Four single-byte fields: year (commonly year-2000), month, day, hour.
  SleepTime t;
  if (b.size() < off + 4) return t;
  t.year = b[off];
  t.month = b[off + 1];
  t.day = b[off + 2];
  t.hour = b[off + 3];
  // Year is a single byte; values 1..99 are treated as year-2000 offsets.
  if (t.year > 0 && t.year < 100) t.year += 2000;
  return t;
}

bool blobLooksEmpty(const Bytes& blob) {
  if (blob.empty()) return true;
  for (uint8_t b : blob)
    if (b != 0) return false;
  return true;
}

// Outer V1 items are 0xA1 + length (LE u16) + payload. Returns true if the
// blob starts with at least one such item.
bool isSleepV1(const Bytes& blob) {
  if (blob.size() < 3 || blob[0] != 0xA1) return false;
  const size_t len = static_cast<size_t>(le16(blob, 1));
  return 3 + len <= blob.size();
}

// Walk TLV triples inside an 0xA1 item payload: tag, length LE u16, data.
void parseV1Item(std::span<const uint8_t> item, SleepSession& s) {
  size_t i = 0;
  while (i + 3 <= item.size()) {
    const uint8_t tag = item[i];
    const size_t len = static_cast<size_t>(le16(item, i + 1));
    i += 3;
    if (i + len > item.size()) break;
    std::span<const uint8_t> body(item.data() + i, len);
    i += len;

    if (tag == 0xA3 && body.size() >= 35) {
      s.sleepDown = readTimeBean(body, 0);
      s.sleepUp = readTimeBean(body, 4);
      s.quality = body[15];
      s.wakeCount = body[16];
      s.getUpScore = body[9];
      s.deepScore = body[10];
      s.efficiencyScore = body[11];
      s.fallAsleepScore = body[12];
      s.sleepTimeScore = body[13];
      s.deepMinutes = le16(body, 19);
      s.lightMinutes = le16(body, 21);
      s.otherMinutes = le16(body, 23);
      s.totalMinutes = le16(body, 25);
      s.onePointDuration = le16(body, 33);
      if (s.onePointDuration <= 0) s.onePointDuration = 5;
      s.v1 = true;
    } else if (tag == 0xA5) {
      // 2-byte samples: stage = (sample & 0xE000) >> 13, clamped to 0..4.
      s.stages.clear();
      for (size_t o = 0; o + 1 < body.size(); o += 2) {
        const int sample = le16(body, o);
        int stage = (sample & 0xE000) >> 13;
        if (stage > 4) stage = 4;
        s.stages.push_back(static_cast<char>('0' + stage));
      }
    }
    // 0xA2 CRC, 0xA4/0xA7 insomnia, 0xA6 lengths: ignored for now.
  }
}

std::vector<SleepSession> parseSleepV1(const Bytes& blob) {
  std::vector<SleepSession> sessions;
  size_t i = 0;
  while (i + 3 <= blob.size() && blob[i] == 0xA1) {
    const size_t len = static_cast<size_t>(le16(blob, i + 1));
    i += 3;
    if (i + len > blob.size()) break;
    SleepSession s;
    parseV1Item(std::span<const uint8_t>(blob.data() + i, len), s);
    i += len;
    // Keep items that carried a base block (or at least some stage data).
    if (s.v1 || !s.stages.empty() || s.totalMinutes > 0 || s.deepMinutes > 0 ||
        s.lightMinutes > 0)
      sessions.push_back(std::move(s));
  }
  return sessions;
}

// Classic path: fixed field offsets from the APK's getSleepBean (1-based
// indices in the doc → 0-based here). One record is at least 56 bytes;
// multiple naps are sequential. Stage curve decoding is partial — we
// surface deep/light/quality/times and a simplified stage string.
std::vector<SleepSession> parseSleepClassic(const Bytes& blob) {
  std::vector<SleepSession> sessions;
  // Doc indices are 1-based; shift by -1. Minimum useful size covers
  // times + deep/light/quality (through index 11 → byte 10).
  constexpr size_t kMinRecord = 11;
  size_t off = 0;
  while (off + kMinRecord <= blob.size()) {
    // Skip runs of zeros between records.
    if (blob[off] == 0 && blob[off + 1] == 0) {
      ++off;
      continue;
    }
    SleepSession s;
    // Indices 1-4 / 5-8 → bytes 0-3 / 4-7 as TimeBean.
    s.sleepDown = readTimeBean(blob, off + 0);
    s.sleepUp = readTimeBean(blob, off + 4);
    s.deepMinutes = blob[off + 8] * 5;   // index 9
    s.lightMinutes = blob[off + 9] * 5;  // index 10
    s.quality = blob[off + 10];          // index 11
    s.totalMinutes = s.deepMinutes + s.lightMinutes;
    if (off + 54 < blob.size()) {
      s.wakeCount = blob[off + 54]; // index 55
      if (s.wakeCount > 20) s.wakeCount = 0; // sanity
    }
    // Stage curve source: bytes index 12..42 (31 bytes) → hex-digit bits.
    // Keep a compact 0/1 string from the high nibble of each byte for UI.
    if (off + 42 <= blob.size()) {
      const int bitLen =
          off + 42 < blob.size() ? std::min(int(blob[off + 42]), 248) : 0; // idx 43
      const int nHex = std::min(62, bitLen > 0 ? (bitLen + 3) / 4 : 62);
      for (int h = 0; h < nHex && off + 11 + 1 + h / 2 < blob.size(); ++h) {
        const uint8_t byte = blob[off + 11 + 1 + h / 2]; // indices 12..
        const int nibble = (h % 2 == 0) ? (byte >> 4) : (byte & 0x0F);
        // Expand nibble to 4 bits as '0'/'1' characters (MSB first).
        for (int b = 3; b >= 0 && static_cast<int>(s.stages.size()) < bitLen; --b)
          s.stages.push_back((nibble & (1 << b)) ? '1' : '0');
      }
    }
    s.v1 = false;
    // Accept the record if it has a plausible time or non-zero duration.
    const bool hasTime = s.sleepDown.month >= 1 && s.sleepDown.month <= 12 &&
                         s.sleepDown.day >= 1 && s.sleepDown.day <= 31;
    if (hasTime || s.totalMinutes > 0 || s.quality > 0)
      sessions.push_back(std::move(s));
    // Advance by a full classic record when possible (~67 bytes per doc).
    constexpr size_t kRecord = 67;
    if (off + kRecord <= blob.size())
      off += kRecord;
    else
      break;
  }
  return sessions;
}

} // namespace

std::optional<SleepDay> decodeSleepDay(const std::vector<Bytes>& frames) {
  if (frames.empty()) return std::nullopt;

  SleepDay day;
  Bytes blob;
  bool any = false;
  for (const Bytes& raw : frames) {
    auto f = decodeSleepFrame(raw);
    if (!f) continue;
    any = true;
    day.daysAgo = f->dayIndex;
    blob.insert(blob.end(), f->payload.begin(), f->payload.end());
  }
  if (!any) return std::nullopt;

  if (blobLooksEmpty(blob)) {
    day.empty = true;
    return day;
  }

  if (isSleepV1(blob))
    day.sessions = parseSleepV1(blob);
  else
    day.sessions = parseSleepClassic(blob);

  day.empty = day.sessions.empty();
  return day;
}

// ---- Weather status (0xC8) ------------------------------------------

Bytes weatherStatusRead() { return {0xC8, 0x02}; }

Bytes weatherStatusWrite(bool open, int type) {
  return {0xC8, 0x03, static_cast<uint8_t>(open ? 1 : 0), u8(type)};
}

std::optional<WeatherStatus> decodeWeatherStatus(std::span<const uint8_t> v) {
  // Reply: c8 <sub> <ok> <crc_lo> <crc_hi> <isOpen> <type> ...
  // Read uses sub 2; set echoes sub 3. Accept either when length is enough.
  if (v.size() < 7 || v[0] != 0xC8) return std::nullopt;
  if (v[1] != 0x02 && v[1] != 0x03) return std::nullopt;
  WeatherStatus s;
  s.ok = v[2] == 0x01;
  s.open = v[5] != 0;
  s.type = v[6];
  return s;
}

// ---- Contacts (0x72) ------------------------------------------------

namespace {

Bytes contactRecord(const Contact& c) {
  // 0xA0 + len LE (includes 3-byte header) + TLVs
  Bytes fields;
  // A1 id (int type1: type, 0x01, value)
  fields.push_back(0xA1);
  fields.push_back(0x01);
  fields.push_back(c.id);
  // A2 nickname UTF-8
  std::string name = c.name;
  if (name.size() > 20) name.resize(20);
  fields.push_back(0xA2);
  fields.push_back(u8(static_cast<int>(name.size())));
  fields.insert(fields.end(), name.begin(), name.end());
  // A3 telephone UTF-8
  std::string phone = c.phone;
  if (phone.size() > 22) phone.resize(22);
  fields.push_back(0xA3);
  fields.push_back(u8(static_cast<int>(phone.size())));
  fields.insert(fields.end(), phone.begin(), phone.end());

  const size_t recLen = 3 + fields.size(); // A0 + len16 + fields
  Bytes rec;
  rec.push_back(0xA0);
  rec.push_back(static_cast<uint8_t>(recLen & 0xFF));
  rec.push_back(static_cast<uint8_t>((recLen >> 8) & 0xFF));
  rec.insert(rec.end(), fields.begin(), fields.end());
  return rec;
}

} // namespace

std::vector<Bytes> contactWritePackets(const std::vector<Contact>& contacts) {
  Bytes body;
  for (const Contact& c : contacts) {
    Bytes rec = contactRecord(c);
    body.insert(body.end(), rec.begin(), rec.end());
  }
  // At least one packet even for an empty list (clear).
  constexpr size_t kChunk = 16;
  const size_t total = std::max<size_t>(1, (body.size() + kChunk - 1) / kChunk);
  std::vector<Bytes> packets;
  packets.reserve(total);
  for (size_t i = 0; i < total; ++i) {
    // Contacts: [2]=index, [3]=total (opposite of weather content framing)
    Bytes p{0x72, 0x01, u8(static_cast<int>(i + 1)), u8(static_cast<int>(total))};
    const size_t off = i * kChunk;
    if (off < body.size()) {
      const size_t n = std::min(kChunk, body.size() - off);
      p.insert(p.end(), body.begin() + static_cast<std::ptrdiff_t>(off),
               body.begin() + static_cast<std::ptrdiff_t>(off + n));
    }
    p.resize(20, 0);
    packets.push_back(std::move(p));
  }
  return packets;
}

Bytes contactDelete(uint8_t id) { return {0x72, 0x04, id}; }

Bytes contactMove(uint8_t fromId, uint8_t toId) { return {0x72, 0x03, fromId, toId}; }

} // namespace s226::protocol
