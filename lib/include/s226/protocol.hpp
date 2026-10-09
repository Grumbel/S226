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
#include <string>
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

// ---- Device information -------------------------------------------

struct DeviceInfo {
  int status = 0;         // 1 password OK, 6 password OK and time set; else failed
  bool bound = false;     // status 1 or 6
  int deviceNumber = 0;   // H-Band shows it before the version, e.g. 851
  std::string firmware;   // e.g. "01.31.06"
};

// Reply to the bind: a1 00 00 <status> <number u16 BE> <version x3> ...
std::optional<DeviceInfo> decodeBindReply(std::span<const uint8_t> value);

Bytes batteryRead(); // a0 00

struct Battery {
  int percent = -1; // -1 if the watch only reports the level
  int level = 0;    // 0..4 bars
};

// a0 00 <0x80 | percent> 00 <level>
std::optional<Battery> decodeBattery(std::span<const uint8_t> value);

// ---- Messages -------------------------------------------------------

// Message source shown by the watch. Only types switched on in the
// watch's 0xAD table are displayed; by default only Other on the S226.
// Index into kMessageTypeNames.
enum class MessageType : uint8_t {
  Call = 0,
  Sms = 1,
  WeChat = 2,
  WhatsApp = 9,
  Other = 17,
};

Bytes smsAlert();  // c1 01 01 01, sent before an SMS text
Bytes callAlert(); // c1 01 06 00: incoming call, followed by callerPackets()
Bytes callEnd();   // c1 00 00 00: stop ringing

// Message types in the order of the 0xAD switch table.
inline constexpr std::array<std::string_view, 18> kMessageTypeNames = {
    "call",     "sms",       "wechat",   "qq",        "weibo",    "facebook",
    "twitter",  "flickr",    "linkedin", "whatsapp",  "line",     "instagram",
    "snapchat", "skype",     "gmail",    "dingtalk",  "wechatwork", "other"};

// Which message types the watch displays. Call and SMS are always
// supported; for the others 0 means unsupported.
struct MessageSwitches {
  enum : uint8_t { Unsupported = 0, On = 1, Off = 2 };
  std::array<uint8_t, kMessageTypeNames.size()> state{};
};

Bytes messageSwitchesRead();                              // ad 02
Bytes messageSwitchesWrite(const MessageSwitches& value); // ad 01 <state x18>
// ad <op> <state x18>; also sent unasked right after the bind
std::optional<MessageSwitches> decodeMessageSwitches(std::span<const uint8_t> value);

// c2 <type> <len> <total> <index> <flag 2 = body> <14 bytes UTF-8>, one
// 20-byte packet per 14 bytes of text. H-Band sends them ~120 ms apart.
std::vector<Bytes> messagePackets(std::string_view text, MessageType type = MessageType::Other);
// Caller name or number for an incoming call (type Call, flag 1).
std::vector<Bytes> callerPackets(std::string_view name);

// ---- Settings ---------------------------------------------------------
//
// Settings commands share a layout: the operation byte is 02 to read and
// 01 (or the on/off flag) to write, and the watch answers with the stored
// values either way.

struct SedentaryReminder {
  bool enabled = false;
  int startHour = 8, startMinute = 0;
  int endHour = 18, endMinute = 0;
  int intervalMinutes = 60;
};

Bytes sedentaryRead();                                // e1 00 00 00 00 00 02
Bytes sedentaryWrite(const SedentaryReminder& value); // e1 sh sm eh em interval on/off
// e1 01 sh sm eh em interval enabled op
std::optional<SedentaryReminder> decodeSedentary(std::span<const uint8_t> value);

struct HeartRateAlarm {
  bool enabled = false;
  int high = 115; // bpm
  int low = 50;
};

Bytes heartRateAlarmRead();                             // ac 00 00 02
Bytes heartRateAlarmWrite(const HeartRateAlarm& value); // ac high low on/off
// ac high low enabled op 01
std::optional<HeartRateAlarm> decodeHeartRateAlarm(std::span<const uint8_t> value);

struct Brightness {
  // Automatic: `level` from start to end (night), `otherLevel` otherwise.
  // Manual: H-Band sends 00:00-23:59 with both levels equal.
  bool automatic = false;
  int startHour = 0, startMinute = 0;
  int endHour = 23, endMinute = 59;
  int level = 1;
  int otherLevel = 1;
  int maxLevel = 0; // reply only
};

Bytes brightnessRead();                         // b1 02 00 ... (20 bytes)
Bytes brightnessWrite(const Brightness& value); // b1 01 sh sm eh em level other mode
// b1 01 op sh sm eh em level other mode max; mode 1 automatic, 2 manual
std::optional<Brightness> decodeBrightness(std::span<const uint8_t> value);
// What H-Band calls automatic: 22:00-08:00 at level 2, else 4.
Brightness automaticBrightness();
Brightness manualBrightness(int level);

struct Countdown {
  int seconds = 0;
  bool showOnWatch = false; // the watch's countdown screen
};

Bytes countdownRead();                                      // b2 02
Bytes countdownWrite(int seconds, bool showOnWatch = true); // b2 01 00 <seconds u24le> <ui>
// b2 op 01 id <seconds u24le> ui ...
std::optional<Countdown> decodeCountdown(std::span<const uint8_t> value);

Bytes watchFaceRead();          // c7 02 00 ... (20 bytes)
Bytes watchFaceWrite(int style); // c7 01 <style>
// c7 op ok style ...; nullopt also if the watch refused the style
std::optional<int> decodeWatchFace(std::span<const uint8_t> value);

// Alarms (0xB9). days is a weekday bit mask (bit 0 = kAlarmDays[0]);
// 0 means once, on the given date.
inline constexpr std::array<std::string_view, 7> kAlarmDays = {"mon", "tue", "wed", "thu",
                                                                "fri", "sat", "sun"};

struct Alarm {
  int id = 1;
  int hour = 0, minute = 0;
  bool enabled = true;
  uint8_t days = 0x7F;
  int year = 0, month = 0, day = 0; // only if days == 0
};

Bytes alarmsRead();                      // b9 02 00 ... (20 bytes)
Bytes alarmWrite(const Alarm& alarm);    // b9 01 id h m on days 00 <year u16le> mon day
Bytes alarmDelete(const Alarm& alarm);   // b9 00 id ... (same fields)

struct AlarmFrame {
  bool ok = false;
  int index = 0; // 1..count while listing; 0 ends a list and acks writes
  int count = 0; // alarms stored
  Alarm alarm;   // valid if index > 0, or in the ack of a write / delete
};

// b9 ok index count op id h m on days scene <year u16le> mon day ... crc
std::optional<AlarmFrame> decodeAlarmFrame(std::span<const uint8_t> value);

struct ScreenOnTime {
  int seconds = 0;
  int minSeconds = 0; // range the watch accepts
  int maxSeconds = 0;
};

Bytes screenOnTimeRead();                  // b4 02
Bytes screenOnTimeWrite(int seconds);      // b4 01 <seconds>
// b4 01 op seconds min max ...
std::optional<ScreenOnTime> decodeScreenOnTime(std::span<const uint8_t> value);

struct PersonInfo {
  int heightCm = 170;
  int weightKg = 70;
  int age = 30;
  bool male = true; // H-Band's sex byte, 1 = male (from the APK, unverified)
  int stepGoal = 8000;
  int sleepGoalMinutes = 480;
};

// a3 height weight age sex goal_u16be sleep_u16be. Write only; the watch
// answers a3 01.
Bytes personInfoWrite(const PersonInfo& value);
bool isPersonInfoAck(std::span<const uint8_t> value);

// Feature toggles (0xB8 package 1): b8 <op> <state per feature> ... 00.
// Each state is 0 unsupported, 1 on, 2 off. For "metric", off means
// imperial units; for "24h", off means 12-hour clock. The second package
// (byte 19 = 1: button lock, message wakes screen) is not supported by
// the S226, which answers with package 1.
struct WatchFeature {
  std::string_view name;
  size_t offset;
};

inline constexpr std::array<WatchFeature, 15> kWatchFeatures = {{
    {"metric", 2},
    {"24h", 3},
    {"auto-hr", 4},           // heart rate every 5 minutes
    {"auto-bp", 5},           // blood pressure every 5 minutes
    {"goal-remind", 6},
    {"voice", 7},
    {"find-phone", 8},
    {"stopwatch", 9},
    {"spo2-remind", 10},
    {"wear-detect", 11},
    {"auto-hrv", 12},
    {"auto-call", 13},
    {"disconnect-remind", 14},
    {"ppg", 16},
    {"music", 18},
}};

struct WatchFeatures {
  enum : uint8_t { Unsupported = 0, On = 1, Off = 2 };
  std::array<uint8_t, 20> raw{};

  uint8_t& operator[](const WatchFeature& f) { return raw[f.offset]; }
  uint8_t operator[](const WatchFeature& f) const { return raw[f.offset]; }
};

Bytes watchFeaturesRead();                            // b8 02 00 ... (20 bytes)
Bytes watchFeaturesWrite(const WatchFeatures& value); // b8 01 <states> ... 00
std::optional<WatchFeatures> decodeWatchFeatures(std::span<const uint8_t> value);

// ---- Music (now-playing push + watch media keys) ---------------------
//
// Phone → watch: 0x99 multi-packet now-playing metadata (title/artist/…).
// Watch → phone: 0x01 auto-callback with next / play-pause / previous.

struct NowPlaying {
  std::string title;
  std::string artist;
  std::string album;
  bool playing = false;
  int volume = 50; // 0..100, displayed on the watch if it shows a level
};

// One or more 20-byte ATT writes (0x99 …). Send in order, ~120 ms apart
// if the link is lossy; back-to-back is fine on a quiet connection.
std::vector<Bytes> nowPlayingPackets(const NowPlaying& info);

enum class MusicAction : uint8_t {
  Next = 1,
  PlayPause = 2,
  Previous = 4,
};

// 01 01 01 <action> … — nullopt if not a music-control notification.
std::optional<MusicAction> decodeMusicControl(std::span<const uint8_t> value);

const char* toString(MusicAction action);

// ---- Stored data ------------------------------------------------------

// Daily history in 5-minute slots. daysAgo 0 = today; the watch sends
// slots firstSlot..count (1-based), one notification each. Any other
// command aborts the transfer.
Bytes historyRead(int daysAgo, int firstSlot = 1); // d1 <first u16le> <day>

struct HistorySlot {
  int index = 0; // 1-based
  int count = 0; // slots available for that day
  int daysAgo = 0;
  int hour = 0;
  int minute = 0;
  int steps = 0;
  int distanceMeters = 0;
  int calories = 0;      // raw, see ActivityTotals
  int activity = 0;      // "sport value", meaning unverified
  int heartRate = 0;     // average bpm, 0 if not measured
};

std::optional<HistorySlot> decodeHistorySlot(std::span<const uint8_t> value);

// Workouts (sport mode) are kept in slots 1..kWorkoutSlots.
inline constexpr int kWorkoutSlots = 3;
Bytes workoutRead(int slot); // d4 <slot>

struct WorkoutFrame {
  int index = 0; // 1-based; 0 with count 0 for an empty slot
  int count = 0;
  int slot = 0;
};

// d4 <index u16le> <count u16le> <slot> <14 payload bytes>
std::optional<WorkoutFrame> decodeWorkoutFrame(std::span<const uint8_t> value);

struct DateTime {
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
};

struct WorkoutMinute {
  int heartRate = 0;
  int activity = 0;
  int steps = 0;
  int calories = 0; // 1/1000 kcal
  int distanceMeters = 0;
};

struct Workout {
  DateTime start;
  DateTime end;
  int steps = 0;
  int distanceMeters = 0;
  int calories = 0; // 1/1000 kcal
  int activity = 0;
  std::vector<WorkoutMinute> minutes;
};

// Assembles all frames of one slot (in order, as received).
std::optional<Workout> decodeWorkout(const std::vector<Bytes>& frames);

// ---- Sleep (0xE0) -----------------------------------------------------
// Multi-packet day dump. daysAgo 0 = last night / today; 1 = yesterday, …
// Write e0 <day>, collect notifies until packet index is 0 (end of day).
// Payload bytes [4..19] of each frame are concatenated into a day blob.

Bytes sleepRead(int daysAgo); // e0 <daysAgo>

struct SleepFrame {
  int packetIndex = 0; // 0 = last packet of this day
  int byte2 = 0;       // used by classic item parse; opaque
  int dayIndex = 0;    // 0 = today, 1 = yesterday, …
  Bytes payload;       // 16 bytes (may be shorter if the ATT value is)
};

// e0 <index> <byte2> <day> <16 payload bytes>
std::optional<SleepFrame> decodeSleepFrame(std::span<const uint8_t> value);

// TimeBean as used by the sleep base block: year (often year-2000),
// month, day, hour — four single bytes.
struct SleepTime {
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
};

struct SleepSession {
  SleepTime sleepDown; // fall-asleep
  SleepTime sleepUp;   // wake
  int deepMinutes = 0;
  int lightMinutes = 0;
  int otherMinutes = 0;  // V1 "other" duration
  int totalMinutes = 0;
  int quality = 0;
  int wakeCount = 0;
  // V1 score fields (0 if classic / unknown)
  int getUpScore = 0;
  int deepScore = 0;
  int efficiencyScore = 0;
  int fallAsleepScore = 0;
  int sleepTimeScore = 0;
  int onePointDuration = 5; // minutes represented by one stage sample
  // Stage curve: digits '0'..'4' (V1) or '0'/'1'/'2' (classic, 2=awake).
  std::string stages;
  bool v1 = false; // true if decoded from the protocol-type-3 TLV path
};

struct SleepDay {
  int daysAgo = 0;
  std::vector<SleepSession> sessions;
  bool empty = true; // no sessions, or only zeroed end-of-day frame(s)
};

// Concatenate frame payloads and parse classic or V1. Returns nullopt only
// when the frame list is empty or contains no valid 0xE0 frames.
std::optional<SleepDay> decodeSleepDay(const std::vector<Bytes>& frames);

} // namespace s226::protocol
