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

std::vector<Bytes> messagePackets(std::string_view text, MessageType type) {
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
            0x02};
    p.insert(p.end(), chunk.begin(), chunk.end());
    p.resize(20, 0);
    packets.push_back(std::move(p));
  }
  return packets;
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

} // namespace s226::protocol
