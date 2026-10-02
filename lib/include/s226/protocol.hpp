// S226 / Veepoo application protocol: packet builders and decoders.
//
// Pure functions, no I/O. See PROTOCOL.md for the captures these are
// derived from.

#pragma once

#include <array>
#include <cstdint>
#include <ctime>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace s226::protocol {

using Bytes = std::vector<uint8_t>;

// Vendor service f008: notifications (watch -> host) and commands
// (host -> watch, Write Command).
inline constexpr std::string_view kNotifyUuid = "f0080002-0451-4000-b000-000000000000";
inline constexpr std::string_view kWriteUuid = "f0080003-0451-4000-b000-000000000000";

// Manufacturer ID in the S226 advertisement (payload = address reversed).
inline constexpr uint16_t kManufacturerId = 0xF8F8;

// 128-bit UUID string -> little-endian bytes as used on the ATT wire.
std::array<uint8_t, 16> uuidToAtt(std::string_view uuid);

// 0xA1 bind / time sync. Must be sent right after enabling notifications.
Bytes bindPacket(const std::tm& localTime);
Bytes bindPacketNow();

Bytes keepalive();          // d8 00 (also returns today's activity totals)
Bytes heartRateStart();     // d0 01
Bytes heartRateStop();      // d0 00
Bytes bloodPressureStart(); // 90 01 00
Bytes bloodPressureStop();  // 90 00 00

struct HeartRateSample {
  int bpm = 0;               // 0 while the sensor settles
  bool sessionEnded = false; // watch finished the measurement on its own
};

// d0 <bpm> 00 00 00 <status> ...; "d0 01 00..." marks end of session.
std::optional<HeartRateSample> decodeHeartRate(std::span<const uint8_t> value);

struct BloodPressureSample {
  int percent = 0;
  int systolic = 0;
  int diastolic = 0;
  bool done = false;    // result valid
  bool stopped = false; // ack of 90 00 00
};

// 90 00 00 <pct> 00 01 (progress), 90 <sys> <dia> 64 (result), 90 01 00 (stopped)
std::optional<BloodPressureSample> decodeBloodPressure(std::span<const uint8_t> value);

struct ActivityTotals {
  uint32_t steps = 0;
  uint32_t distanceMeters = 0;
  uint32_t calories = 0; // raw; probably 0.1 kcal units (unverified)
};

// Reply to d8 00: d8 00 | steps u32le | distance u32le | calories u32le
std::optional<ActivityTotals> decodeActivity(std::span<const uint8_t> value);

} // namespace s226::protocol
