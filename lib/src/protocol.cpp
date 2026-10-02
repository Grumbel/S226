#include "s226/protocol.hpp"

#include <algorithm>
#include <stdexcept>

namespace s226::protocol {

namespace {

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
  auto u32 = [&](size_t i) {
    return static_cast<uint32_t>(v[i] | v[i + 1] << 8 | v[i + 2] << 16 |
                                 static_cast<uint32_t>(v[i + 3]) << 24);
  };
  return ActivityTotals{u32(2), u32(6), u32(10)};
}

} // namespace s226::protocol
