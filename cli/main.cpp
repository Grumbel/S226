// s226-cli: list Bluetooth controllers, print S226 heart rate, and read
// or change the watch's data and settings.

#include <getopt.h>
#include <signal.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "s226/step_rate.hpp"
#include "s226/usb.hpp"
#include "s226/watch.hpp"

#ifndef S226_VERSION
#define S226_VERSION "dev"
#endif

namespace {

namespace proto = s226::protocol;
using Bytes = proto::Bytes;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::atomic<bool> g_quit{false};

void onSignal(int) { g_quit = true; }

std::string timestamp() {
  std::time_t now = std::time(nullptr);
  char buf[16];
  std::strftime(buf, sizeof buf, "%H:%M:%S", std::localtime(&now));
  return buf;
}

std::string toHex(const Bytes& v) {
  std::string h;
  char b[4];
  for (uint8_t x : v) {
    std::snprintf(b, sizeof b, "%02x ", x);
    h += b;
  }
  if (!h.empty()) h.pop_back();
  return h;
}

std::optional<Bytes> parseHex(const std::string& text) {
  Bytes out;
  std::string digits;
  for (char c : text) {
    if (std::isxdigit(static_cast<unsigned char>(c))) {
      digits += c;
    } else if (c != ' ' && c != ':' && c != '-') {
      return std::nullopt;
    }
  }
  if (digits.empty() || digits.size() % 2) return std::nullopt;
  for (size_t i = 0; i < digits.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoi(digits.substr(i, 2), nullptr, 16)));
  }
  return out;
}

// "HH:MM-HH:MM/MIN"
std::optional<proto::SedentaryReminder> parseSedentary(const std::string& s) {
  proto::SedentaryReminder r;
  char tail = 0;
  if (std::sscanf(s.c_str(), "%d:%d-%d:%d/%d%c", &r.startHour, &r.startMinute, &r.endHour,
                  &r.endMinute, &r.intervalMinutes, &tail) != 5) {
    return std::nullopt;
  }
  if (r.startHour < 0 || r.startHour > 23 || r.endHour < 0 || r.endHour > 23 ||
      r.startMinute < 0 || r.startMinute > 59 || r.endMinute < 0 || r.endMinute > 59 ||
      r.intervalMinutes < 1 || r.intervalMinutes > 255) {
    return std::nullopt;
  }
  r.enabled = true;
  return r;
}

// "LOW-HIGH"
std::optional<proto::HeartRateAlarm> parseHeartRateAlarm(const std::string& s) {
  proto::HeartRateAlarm a;
  char tail = 0;
  if (std::sscanf(s.c_str(), "%d-%d%c", &a.low, &a.high, &tail) != 2 || a.low < 30 ||
      a.high > 250 || a.low >= a.high) {
    return std::nullopt;
  }
  a.enabled = true;
  return a;
}

// "HEIGHT,WEIGHT,AGE,m|f,STEPGOAL[,SLEEPMIN]"
std::optional<proto::PersonInfo> parsePerson(const std::string& s) {
  proto::PersonInfo p;
  char sex = 0;
  const int n = std::sscanf(s.c_str(), "%d,%d,%d,%c,%d,%d", &p.heightCm, &p.weightKg, &p.age,
                            &sex, &p.stepGoal, &p.sleepGoalMinutes);
  if (n < 5 || (sex != 'm' && sex != 'f') || p.heightCm < 50 || p.heightCm > 255 ||
      p.weightKg < 10 || p.weightKg > 255 || p.age < 1 || p.age > 255 || p.stepGoal < 0 ||
      p.stepGoal > 0xFFFF || p.sleepGoalMinutes < 0 || p.sleepGoalMinutes > 24 * 60) {
    return std::nullopt;
  }
  p.male = sex == 'm';
  return p;
}

std::string onOff(bool on) { return on ? "on" : "off"; }

std::string hhmm(int h, int m) {
  char buf[8];
  std::snprintf(buf, sizeof buf, "%02d:%02d", h, m);
  return buf;
}

std::string dateTime(const proto::DateTime& t) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", t.year, t.month, t.day,
                t.hour, t.minute, t.second);
  return buf;
}

// Notifications handed from the Watch worker thread to the main thread.
class Inbox {
public:
  void push(Bytes value) {
    {
      std::lock_guard lock(mutex_);
      queue_.push_back(std::move(value));
    }
    cv_.notify_one();
  }

  // Next notification, or nullopt at the deadline or on Ctrl+C.
  std::optional<Bytes> pop(Clock::time_point deadline) {
    std::unique_lock lock(mutex_);
    while (queue_.empty()) {
      if (g_quit || Clock::now() >= deadline) return std::nullopt;
      // Short waits so Ctrl+C is noticed.
      cv_.wait_until(lock, std::min(deadline, Clock::now() + 100ms));
    }
    Bytes v = std::move(queue_.front());
    queue_.pop_front();
    return v;
  }

  void clear() {
    std::lock_guard lock(mutex_);
    queue_.clear();
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Bytes> queue_;
};

// One-shot commands run in order after connecting. Each sends a command
// and waits for its reply, so a long transfer (history, workouts) is not
// cut short by the next command.
class Session {
public:
  Session(s226::Watch& watch, Inbox& inbox) : watch_(watch), inbox_(inbox) {}

  std::optional<proto::DeviceInfo> deviceInfo; // filled from the bind reply

  // Sends command and returns the first notification for which accept()
  // is true, or nullopt after the timeout.
  std::optional<Bytes> request(const Bytes& command, const std::function<bool(const Bytes&)>& accept,
                               std::chrono::milliseconds timeout = 3s) {
    inbox_.clear();
    watch_.send(command);
    return next(accept, timeout);
  }

  std::optional<Bytes> next(const std::function<bool(const Bytes&)>& accept,
                            std::chrono::milliseconds timeout = 3s) {
    const auto deadline = Clock::now() + timeout;
    while (auto v = inbox_.pop(deadline)) {
      if (accept(*v)) return v;
    }
    return std::nullopt;
  }

  template <typename Decoded>
  std::optional<Decoded> query(const Bytes& command,
                               std::optional<Decoded> (*decode)(std::span<const uint8_t>)) {
    std::optional<Decoded> result;
    request(command, [&](const Bytes& v) { return (result = decode(v)).has_value(); });
    return result;
  }

  s226::Watch& watch() { return watch_; }

private:
  s226::Watch& watch_;
  Inbox& inbox_;
};

using Action = std::function<bool(Session&)>;

bool fail(const char* what) {
  std::fprintf(stderr, "No answer from the watch to %s\n", what);
  return false;
}

bool showInfo(Session& s) {
  if (s.deviceInfo) {
    std::printf("Firmware:      %s (device number %d)\n", s.deviceInfo->firmware.c_str(),
                s.deviceInfo->deviceNumber);
  }
  auto b = s.query<proto::Battery>(proto::batteryRead(), proto::decodeBattery);
  if (!b) return fail("the battery query");
  if (b->percent >= 0) {
    std::printf("Battery:       %d%% (%d/4)\n", b->percent, b->level);
  } else {
    std::printf("Battery:       %d/4\n", b->level);
  }
  return true;
}

void printSedentary(const proto::SedentaryReminder& r) {
  std::printf("Sedentary:     %s, %s-%s, every %d min\n", onOff(r.enabled).c_str(),
              hhmm(r.startHour, r.startMinute).c_str(), hhmm(r.endHour, r.endMinute).c_str(),
              r.intervalMinutes);
}

void printHeartRateAlarm(const proto::HeartRateAlarm& a) {
  std::printf("HR alarm:      %s, below %d or above %d bpm\n", onOff(a.enabled).c_str(), a.low,
              a.high);
}

void printScreenOnTime(const proto::ScreenOnTime& t) {
  std::printf("Screen on:     %d s (%d-%d s)\n", t.seconds, t.minSeconds, t.maxSeconds);
}

void printBrightness(const proto::Brightness& b) {
  if (b.automatic) {
    std::printf("Brightness:    automatic, %d from %s to %s, else %d (max %d)\n", b.level,
                hhmm(b.startHour, b.startMinute).c_str(), hhmm(b.endHour, b.endMinute).c_str(),
                b.otherLevel, b.maxLevel);
  } else {
    std::printf("Brightness:    %d (max %d)\n", b.otherLevel, b.maxLevel);
  }
}

void printCountdown(const proto::Countdown& c) {
  std::printf("Countdown:     %d:%02d:%02d%s\n", c.seconds / 3600, c.seconds / 60 % 60,
              c.seconds % 60, c.showOnWatch ? "" : " (hidden on the watch)");
}

std::string alarmDays(const proto::Alarm& a) {
  if (a.days == 0) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", a.year, a.month, a.day);
    return std::string("once on ") + buf;
  }
  if (a.days == 0x7F) return "daily";
  std::string out;
  for (size_t i = 0; i < proto::kAlarmDays.size(); ++i) {
    if (!(a.days & (1 << i))) continue;
    if (!out.empty()) out += ",";
    out += proto::kAlarmDays[i];
  }
  return out;
}

void printAlarm(const proto::Alarm& a) {
  std::printf("Alarm %d:       %s %s%s\n", a.id, hhmm(a.hour, a.minute).c_str(),
              alarmDays(a).c_str(), a.enabled ? "" : " (off)");
}

// All stored alarms, or nullopt if the watch did not answer.
std::optional<std::vector<proto::Alarm>> readAlarms(Session& s) {
  auto isFrame = [](const Bytes& v) { return proto::decodeAlarmFrame(v).has_value(); };
  auto v = s.request(proto::alarmsRead(), isFrame);
  std::vector<proto::Alarm> alarms;
  while (v) {
    auto f = *proto::decodeAlarmFrame(*v);
    if (f.index == 0) return alarms; // end of list
    alarms.push_back(f.alarm);
    v = s.next(isFrame);
  }
  return std::nullopt;
}

bool showAlarms(Session& s) {
  auto alarms = readAlarms(s);
  if (!alarms) return fail("the alarm query");
  if (alarms->empty()) std::puts("Alarms:        none");
  for (const auto& a : *alarms) printAlarm(a);
  return true;
}

// "HH:MM[/daily|once|mon,tue,...]"
std::optional<proto::Alarm> parseAlarm(int id, const std::string& s) {
  proto::Alarm a;
  a.id = id;
  char tail = 0;
  const size_t slash = s.find('/');
  const std::string time = s.substr(0, slash);
  const std::string days = slash == std::string::npos ? "daily" : s.substr(slash + 1);
  if (std::sscanf(time.c_str(), "%d:%d%c", &a.hour, &a.minute, &tail) != 2 || a.hour < 0 ||
      a.hour > 23 || a.minute < 0 || a.minute > 59) {
    return std::nullopt;
  }
  if (days == "daily") {
    a.days = 0x7F;
  } else if (days == "once") {
    // Next occurrence: today if still ahead, else tomorrow.
    std::time_t t = std::time(nullptr);
    std::tm now{};
    localtime_r(&t, &now);
    if (a.hour * 60 + a.minute <= now.tm_hour * 60 + now.tm_min) t += 24 * 3600;
    localtime_r(&t, &now);
    a.days = 0;
    a.year = now.tm_year + 1900;
    a.month = now.tm_mon + 1;
    a.day = now.tm_mday;
  } else {
    a.days = 0;
    size_t pos = 0;
    while (pos <= days.size()) {
      const size_t end = std::min(days.find(',', pos), days.size());
      auto it = std::find(proto::kAlarmDays.begin(), proto::kAlarmDays.end(),
                          days.substr(pos, end - pos));
      if (it == proto::kAlarmDays.end()) return std::nullopt;
      a.days |= static_cast<uint8_t>(1 << (it - proto::kAlarmDays.begin()));
      pos = end + 1;
    }
  }
  return a;
}

Action setAlarm(proto::Alarm alarm) {
  return [alarm](Session& s) {
    std::optional<proto::AlarmFrame> ack;
    s.request(proto::alarmWrite(alarm), [&](const Bytes& v) {
      return (ack = proto::decodeAlarmFrame(v)).has_value();
    });
    if (!ack) return fail("the alarm setting");
    if (!ack->ok) {
      std::fprintf(stderr, "The watch refused alarm %d\n", alarm.id);
      return false;
    }
    printAlarm(ack->alarm);
    return true;
  };
}

Action deleteAlarm(int id) {
  return [id](Session& s) {
    auto alarms = readAlarms(s);
    if (!alarms) return fail("the alarm query");
    auto it = std::find_if(alarms->begin(), alarms->end(),
                           [&](const proto::Alarm& a) { return a.id == id; });
    if (it == alarms->end()) {
      std::fprintf(stderr, "There is no alarm %d\n", id);
      return false;
    }
    std::optional<proto::AlarmFrame> ack;
    s.request(proto::alarmDelete(*it), [&](const Bytes& v) {
      return (ack = proto::decodeAlarmFrame(v)).has_value();
    });
    if (!ack || !ack->ok) return fail("the alarm deletion");
    std::printf("Alarm %d deleted\n", id);
    return true;
  };
}

Action setBrightness(std::string value) {
  return [value](Session& s) {
    auto current = s.query<proto::Brightness>(proto::brightnessRead(), proto::decodeBrightness);
    if (!current) return fail("the brightness query");
    proto::Brightness b = proto::automaticBrightness();
    if (value != "auto") {
      const int level = std::atoi(value.c_str());
      if (level < 1 || level > current->maxLevel) {
        std::fprintf(stderr, "The watch accepts brightness 1-%d\n", current->maxLevel);
        return false;
      }
      b = proto::manualBrightness(level);
    }
    auto result = s.query<proto::Brightness>(proto::brightnessWrite(b), proto::decodeBrightness);
    if (!result) return fail("the brightness setting");
    printBrightness(*result);
    return true;
  };
}

Action setCountdown(int seconds) {
  return [seconds](Session& s) {
    auto result =
        s.query<proto::Countdown>(proto::countdownWrite(seconds), proto::decodeCountdown);
    if (!result) return fail("the countdown setting");
    printCountdown(*result);
    return true;
  };
}

Action setWatchFace(int style) {
  return [style](Session& s) {
    auto result = s.query<int>(proto::watchFaceWrite(style), proto::decodeWatchFace);
    if (!result) return fail("the watch face setting");
    std::printf("Watch face:    %d\n", *result);
    return true;
  };
}

void printFeatures(const proto::WatchFeatures& f) {
  std::string on, off;
  for (const auto& feature : proto::kWatchFeatures) {
    if (f[feature] == proto::WatchFeatures::Unsupported) continue;
    std::string& list = f[feature] == proto::WatchFeatures::On ? on : off;
    if (!list.empty()) list += ", ";
    list += feature.name;
  }
  std::printf("Features on:   %s\n", on.empty() ? "none" : on.c_str());
  std::printf("Features off:  %s\n", off.empty() ? "none" : off.c_str());
}

// NAME=on|off
std::optional<std::pair<proto::WatchFeature, bool>> parseFeature(const std::string& s) {
  const size_t eq = s.find('=');
  if (eq == std::string::npos) return std::nullopt;
  const std::string name = s.substr(0, eq), value = s.substr(eq + 1);
  if (value != "on" && value != "off") return std::nullopt;
  for (const auto& f : proto::kWatchFeatures) {
    if (f.name == name) return std::pair{f, value == "on"};
  }
  return std::nullopt;
}

Action setFeature(proto::WatchFeature feature, bool on) {
  return [feature, on](Session& s) {
    auto current =
        s.query<proto::WatchFeatures>(proto::watchFeaturesRead(), proto::decodeWatchFeatures);
    if (!current) return fail("the feature query");
    if ((*current)[feature] == proto::WatchFeatures::Unsupported) {
      std::fprintf(stderr, "The watch does not support %s\n",
                   std::string(feature.name).c_str());
      return false;
    }
    proto::WatchFeatures f = *current;
    f[feature] = on ? proto::WatchFeatures::On : proto::WatchFeatures::Off;
    auto result =
        s.query<proto::WatchFeatures>(proto::watchFeaturesWrite(f), proto::decodeWatchFeatures);
    if (!result) return fail("the feature setting");
    printFeatures(*result);
    return (*result)[feature] == f[feature];
  };
}

void printMessageSwitches(const proto::MessageSwitches& m) {
  std::string on, off;
  for (size_t i = 0; i < m.state.size(); ++i) {
    if (m.state[i] == proto::MessageSwitches::Unsupported) continue;
    std::string& list = m.state[i] == proto::MessageSwitches::On ? on : off;
    if (!list.empty()) list += ", ";
    list += proto::kMessageTypeNames[i];
  }
  std::printf("Messages on:   %s\n", on.empty() ? "none" : on.c_str());
  std::printf("Messages off:  %s\n", off.empty() ? "none" : off.c_str());
}

// Comma-separated type names, "all" or "none" -> set of indices.
std::optional<std::vector<bool>> parseMessageTypes(const std::string& s) {
  std::vector<bool> want(proto::kMessageTypeNames.size(), false);
  if (s == "all") return std::vector<bool>(want.size(), true);
  if (s == "none") return want;
  size_t pos = 0;
  while (pos <= s.size()) {
    const size_t end = std::min(s.find(',', pos), s.size());
    const std::string name = s.substr(pos, end - pos);
    auto it = std::find(proto::kMessageTypeNames.begin(), proto::kMessageTypeNames.end(), name);
    if (it == proto::kMessageTypeNames.end()) return std::nullopt;
    want[static_cast<size_t>(it - proto::kMessageTypeNames.begin())] = true;
    pos = end + 1;
  }
  return want;
}

Action setMessageTypes(std::vector<bool> want) {
  return [want](Session& s) {
    auto current = s.query<proto::MessageSwitches>(proto::messageSwitchesRead(),
                                                   proto::decodeMessageSwitches);
    if (!current) return fail("the message switch query");
    proto::MessageSwitches m = *current;
    for (size_t i = 0; i < m.state.size(); ++i) {
      // Call and SMS are always supported, even though they read as 2 when off.
      if (i >= 2 && m.state[i] == proto::MessageSwitches::Unsupported) {
        if (want[i]) {
          std::fprintf(stderr, "The watch does not support %s messages\n",
                       std::string(proto::kMessageTypeNames[i]).c_str());
        }
        continue;
      }
      m.state[i] = want[i] ? proto::MessageSwitches::On : proto::MessageSwitches::Off;
    }
    auto result = s.query<proto::MessageSwitches>(proto::messageSwitchesWrite(m),
                                                  proto::decodeMessageSwitches);
    if (!result) return fail("the message switch setting");
    printMessageSwitches(*result);
    return result->state == m.state;
  };
}

bool showSettings(Session& s) {
  auto sed = s.query<proto::SedentaryReminder>(proto::sedentaryRead(), proto::decodeSedentary);
  if (!sed) return fail("the sedentary query");
  printSedentary(*sed);
  auto hr = s.query<proto::HeartRateAlarm>(proto::heartRateAlarmRead(),
                                           proto::decodeHeartRateAlarm);
  if (!hr) return fail("the heart-rate alarm query");
  printHeartRateAlarm(*hr);
  auto scr = s.query<proto::ScreenOnTime>(proto::screenOnTimeRead(), proto::decodeScreenOnTime);
  if (!scr) return fail("the screen-on time query");
  printScreenOnTime(*scr);
  auto msg = s.query<proto::MessageSwitches>(proto::messageSwitchesRead(),
                                             proto::decodeMessageSwitches);
  if (!msg) return fail("the message switch query");
  printMessageSwitches(*msg);
  auto feat =
      s.query<proto::WatchFeatures>(proto::watchFeaturesRead(), proto::decodeWatchFeatures);
  if (!feat) return fail("the feature query");
  printFeatures(*feat);
  auto bright = s.query<proto::Brightness>(proto::brightnessRead(), proto::decodeBrightness);
  if (!bright) return fail("the brightness query");
  printBrightness(*bright);
  auto face = s.query<int>(proto::watchFaceRead(), proto::decodeWatchFace);
  if (!face) return fail("the watch face query");
  std::printf("Watch face:    %d\n", *face);
  auto countdown = s.query<proto::Countdown>(proto::countdownRead(), proto::decodeCountdown);
  if (!countdown) return fail("the countdown query");
  printCountdown(*countdown);
  return showAlarms(s);
}

// value: "on", "off" or "HH:MM-HH:MM/MIN"
Action setSedentary(std::string value) {
  return [value](Session& s) {
    proto::SedentaryReminder r;
    if (auto parsed = parseSedentary(value)) {
      r = *parsed;
    } else {
      auto current =
          s.query<proto::SedentaryReminder>(proto::sedentaryRead(), proto::decodeSedentary);
      if (!current) return fail("the sedentary query");
      r = *current;
      r.enabled = value == "on";
    }
    auto result =
        s.query<proto::SedentaryReminder>(proto::sedentaryWrite(r), proto::decodeSedentary);
    if (!result) return fail("the sedentary setting");
    printSedentary(*result);
    return true;
  };
}

// value: "on", "off" or "LOW-HIGH"
Action setHeartRateAlarm(std::string value) {
  return [value](Session& s) {
    proto::HeartRateAlarm a;
    if (auto parsed = parseHeartRateAlarm(value)) {
      a = *parsed;
    } else {
      auto current = s.query<proto::HeartRateAlarm>(proto::heartRateAlarmRead(),
                                                    proto::decodeHeartRateAlarm);
      if (!current) return fail("the heart-rate alarm query");
      a = *current;
      a.enabled = value == "on";
    }
    auto result = s.query<proto::HeartRateAlarm>(proto::heartRateAlarmWrite(a),
                                                 proto::decodeHeartRateAlarm);
    if (!result) return fail("the heart-rate alarm setting");
    printHeartRateAlarm(*result);
    return true;
  };
}

Action setScreenOnTime(int seconds) {
  return [seconds](Session& s) {
    // The watch ignores values outside its range without answering.
    auto range = s.query<proto::ScreenOnTime>(proto::screenOnTimeRead(), proto::decodeScreenOnTime);
    if (!range) return fail("the screen-on time query");
    if (seconds < range->minSeconds || seconds > range->maxSeconds) {
      std::fprintf(stderr, "The watch accepts a screen-on time of %d-%d s\n", range->minSeconds,
                   range->maxSeconds);
      return false;
    }
    auto result = s.query<proto::ScreenOnTime>(proto::screenOnTimeWrite(seconds),
                                               proto::decodeScreenOnTime);
    if (!result) return fail("the screen-on time setting");
    printScreenOnTime(*result);
    return true;
  };
}

Action setPerson(proto::PersonInfo p) {
  return [p](Session& s) {
    if (!s.request(proto::personInfoWrite(p),
                   [](const Bytes& v) { return proto::isPersonInfoAck(v); })) {
      return fail("the personal data");
    }
    std::printf("Personal data: %d cm, %d kg, age %d, %s, goal %d steps, sleep %d min\n",
                p.heightCm, p.weightKg, p.age, p.male ? "male" : "female", p.stepGoal,
                p.sleepGoalMinutes);
    return true;
  };
}

Action notify(std::string text, proto::MessageType type) {
  return [text, type](Session& s) {
    if (type == proto::MessageType::Sms) {
      s.watch().send(proto::smsAlert());
      std::this_thread::sleep_for(120ms);
    }
    for (const auto& packet : proto::messagePackets(text, type)) {
      s.watch().send(packet);
      std::this_thread::sleep_for(120ms);
    }
    return true;
  };
}

// Rings until the watch reports a key press or the time is up.
Action call(std::string name) {
  return [name](Session& s) {
    constexpr auto kRingTime = 20s;
    auto ack = s.request(proto::callAlert(), [](const Bytes& v) {
      return v.size() >= 3 && v[0] == 0xC1 && v[1] == 0x01;
    });
    if (!ack) return fail("the call alert");
    for (const auto& packet : proto::callerPackets(name)) {
      std::this_thread::sleep_for(120ms);
      s.watch().send(packet);
    }
    std::printf("Ringing: %s\n", name.c_str());
    std::fflush(stdout);
    // Acks of our commands are c1 xx 01; the watch reports a long press
    // (accept) as c1 02 00 and a short press (reject) as c1 03 00.
    auto answer = s.next(
        [](const Bytes& v) { return v.size() >= 3 && v[0] == 0xC1 && v[2] != 0x01; }, kRingTime);
    if (answer && (*answer)[1] == 0x02) {
      std::puts("Accepted on the watch");
    } else if (answer && (*answer)[1] == 0x03) {
      std::puts("Rejected on the watch");
    } else if (answer) {
      std::printf("Watch: %s\n", toHex(*answer).c_str());
    } else {
      std::puts("No answer");
    }
    s.watch().send(proto::callEnd());
    // A call alert right after the end is acked but not shown.
    std::this_thread::sleep_for(2s);
    return true;
  };
}

// 5-minute slots of one day as CSV.
Action history(int daysAgo) {
  return [daysAgo](Session& s) {
    std::time_t t = std::time(nullptr) - static_cast<std::time_t>(daysAgo) * 24 * 3600;
    std::tm day{};
    localtime_r(&t, &day);
    char date[16];
    std::strftime(date, sizeof date, "%Y-%m-%d", &day);

    auto isSlot = [&](const Bytes& v) {
      auto slot = proto::decodeHistorySlot(v);
      return slot && slot->daysAgo == daysAgo;
    };
    auto v = s.request(proto::historyRead(daysAgo), isSlot);
    if (!v) return fail("the history request");
    std::puts("time,steps,distance_m,kcal,activity,heart_rate");
    for (;;) {
      auto slot = *proto::decodeHistorySlot(*v);
      std::printf("%s %s,%d,%d,%.1f,%d,%d\n", date, hhmm(slot.hour, slot.minute).c_str(),
                  slot.steps, slot.distanceMeters, slot.calories / 10.0, slot.activity,
                  slot.heartRate);
      if (slot.index >= slot.count) return true;
      v = s.next(isSlot);
      if (!v) {
        std::fprintf(stderr, "History transfer stopped after slot %d of %d\n", slot.index,
                     slot.count);
        return false;
      }
    }
  };
}

// Sleep sessions for one day (0 = last night / today).
Action sleepDay(int daysAgo) {
  return [daysAgo](Session& s) {
    auto isFrame = [&](const Bytes& v) {
      auto f = proto::decodeSleepFrame(v);
      return f && f->dayIndex == daysAgo;
    };
    auto v = s.request(proto::sleepRead(daysAgo), isFrame);
    if (!v) return fail("the sleep request");
    std::vector<Bytes> frames;
    for (;;) {
      auto f = *proto::decodeSleepFrame(*v);
      frames.push_back(*v);
      if (f.packetIndex == 0) break;
      v = s.next(isFrame);
      if (!v) {
        std::fprintf(stderr, "Sleep transfer stopped after %zu frame(s)\n", frames.size());
        return false;
      }
    }
    auto day = proto::decodeSleepDay(frames);
    if (!day) {
      std::fprintf(stderr, "Could not decode sleep data\n");
      return false;
    }
    if (day->empty) {
      std::printf("No sleep data for day %d.\n", daysAgo);
      return true;
    }
    for (size_t i = 0; i < day->sessions.size(); ++i) {
      const auto& ss = day->sessions[i];
      auto fmt = [](const proto::SleepTime& t) {
        char buf[40];
        if (t.year > 0 && t.month > 0)
          std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:00", t.year, t.month, t.day,
                        t.hour);
        else
          std::snprintf(buf, sizeof buf, "%02d:00", t.hour);
        return std::string(buf);
      };
      std::printf("Sleep %zu: %s → %s, deep %d min, light %d min", i + 1, fmt(ss.sleepDown).c_str(),
                  fmt(ss.sleepUp).c_str(), ss.deepMinutes, ss.lightMinutes);
      if (ss.otherMinutes > 0) std::printf(", other %d min", ss.otherMinutes);
      if (ss.totalMinutes > 0) std::printf(", total %d min", ss.totalMinutes);
      if (ss.quality > 0) std::printf(", quality %d", ss.quality);
      if (ss.wakeCount > 0) std::printf(", wakes %d", ss.wakeCount);
      if (ss.v1) {
        std::printf(" [v1 scores: up=%d deep=%d eff=%d fall=%d time=%d]", ss.getUpScore,
                    ss.deepScore, ss.efficiencyScore, ss.fallAsleepScore, ss.sleepTimeScore);
      }
      std::printf("\n");
      if (!ss.stages.empty()) {
        const size_t show = std::min(ss.stages.size(), size_t(120));
        std::printf("  stages (%zu pts, %d min each): %s%s\n", ss.stages.size(),
                    ss.onePointDuration, ss.stages.substr(0, show).c_str(),
                    ss.stages.size() > show ? "…" : "");
      }
    }
    return true;
  };
}

bool workouts(Session& s) {
  int found = 0;
  for (int slot = 1; slot <= proto::kWorkoutSlots; ++slot) {
    auto isFrame = [&](const Bytes& v) {
      auto f = proto::decodeWorkoutFrame(v);
      return f && (f->slot == slot || f->count == 0);
    };
    auto v = s.request(proto::workoutRead(slot), isFrame);
    if (!v) return fail("the workout request");
    std::vector<Bytes> frames;
    for (;;) {
      auto f = *proto::decodeWorkoutFrame(*v);
      if (f.count == 0) break; // empty slot
      frames.push_back(*v);
      if (f.index >= f.count) break;
      v = s.next(isFrame);
      if (!v) {
        std::fprintf(stderr, "Workout transfer stopped after frame %d of %d\n", f.index,
                     f.count);
        return false;
      }
    }
    if (frames.empty()) continue;
    auto w = proto::decodeWorkout(frames);
    if (!w) {
      std::fprintf(stderr, "Could not decode workout %d\n", slot);
      continue;
    }
    ++found;
    int hrSum = 0, hrCount = 0, hrMax = 0;
    for (const auto& m : w->minutes) {
      if (m.heartRate == 0) continue;
      hrSum += m.heartRate;
      ++hrCount;
      hrMax = std::max(hrMax, m.heartRate);
    }
    std::printf("Workout %s to %s: %zu min, %d steps, %d m, %.1f kcal", dateTime(w->start).c_str(),
                dateTime(w->end).c_str(), w->minutes.size(), w->steps, w->distanceMeters,
                w->calories / 1000.0);
    if (hrCount) std::printf(", HR avg %d max %d", hrSum / hrCount, hrMax);
    std::puts("");
    std::puts("minute,heart_rate,steps,distance_m,kcal,activity");
    for (size_t i = 0; i < w->minutes.size(); ++i) {
      const auto& m = w->minutes[i];
      std::printf("%zu,%d,%d,%d,%.3f,%d\n", i + 1, m.heartRate, m.steps, m.distanceMeters,
                  m.calories / 1000.0, m.activity);
    }
  }
  if (!found) std::puts("No workouts stored on the watch.");
  return true;
}


// TITLE[/ARTIST[/ALBUM]] optional ;playing=0|1 ;vol=N
Action weatherStatus(std::optional<bool> open) {
  return [open](Session& s) {
    if (!open) {
      auto result = s.query<proto::WeatherStatus>(proto::weatherStatusRead(),
                                                  proto::decodeWeatherStatus);
      if (!result) return fail("the weather status");
      std::printf("Weather: %s (type %d)%s\n", result->open ? "on" : "off", result->type,
                  result->ok ? "" : " [not ok]");
      return true;
    }
    auto result = s.query<proto::WeatherStatus>(proto::weatherStatusWrite(*open, 0),
                                                proto::decodeWeatherStatus);
    if (!result) return fail("the weather status");
    std::printf("Weather: %s (type %d)%s\n", result->open ? "on" : "off", result->type,
                result->ok ? "" : " [not ok]");
    return result->open == *open;
  };
}

// "Name:Phone,Name2:Phone2" or empty string to clear.
Action contactsPush(std::string spec) {
  return [spec](Session& s) {
    std::vector<proto::Contact> list;
    if (!spec.empty()) {
      size_t pos = 0;
      uint8_t id = 1;
      while (pos < spec.size()) {
        size_t comma = spec.find(',', pos);
        if (comma == std::string::npos) comma = spec.size();
        std::string item = spec.substr(pos, comma - pos);
        pos = comma + 1;
        size_t colon = item.find(':');
        if (colon == std::string::npos) {
          std::fprintf(stderr, "Contact must be Name:Phone (got %s)\n", item.c_str());
          return false;
        }
        proto::Contact c;
        c.id = id++;
        c.name = item.substr(0, colon);
        c.phone = item.substr(colon + 1);
        list.push_back(std::move(c));
      }
    }
    auto packets = proto::contactWritePackets(list);
    for (const auto& p : packets) {
      s.watch().send(p);
      std::this_thread::sleep_for(120ms);
    }
    if (list.empty())
      std::printf("Contacts: cleared (%zu packet(s))\n", packets.size());
    else {
      std::printf("Contacts: pushed %zu entr%s (%zu packet(s))\n", list.size(),
                  list.size() == 1 ? "y" : "ies", packets.size());
      for (const auto& c : list)
        std::printf("  %u  %s  %s\n", c.id, c.name.c_str(), c.phone.c_str());
    }
    return true;
  };
}

Action contactDelete(int id) {
  return [id](Session& s) {
    s.watch().send(proto::contactDelete(static_cast<uint8_t>(id)));
    std::printf("Contact delete id %d sent\n", id);
    return true;
  };
}

Action music(std::string spec) {
  return [spec](Session& s) {
    proto::NowPlaying np;
    np.playing = true;
    np.volume = 50;
    std::string main = spec;
    // Trailing ;key=value options
    for (;;) {
      const size_t semi = main.rfind(';');
      if (semi == std::string::npos) break;
      const std::string opt = main.substr(semi + 1);
      main = main.substr(0, semi);
      if (opt.rfind("playing=", 0) == 0) {
        np.playing = (opt.substr(8) != "0" && opt.substr(8) != "false");
      } else if (opt.rfind("vol=", 0) == 0) {
        np.volume = std::atoi(opt.c_str() + 4);
      }
    }
    size_t p1 = main.find('/');
    if (p1 == std::string::npos) {
      np.title = main;
    } else {
      np.title = main.substr(0, p1);
      size_t p2 = main.find('/', p1 + 1);
      if (p2 == std::string::npos) {
        np.artist = main.substr(p1 + 1);
      } else {
        np.artist = main.substr(p1 + 1, p2 - p1 - 1);
        np.album = main.substr(p2 + 1);
      }
    }
    auto packets = proto::nowPlayingPackets(np);
    for (const auto& p : packets) s.watch().send(p);
    std::printf("Music: \"%s\" by %s (%s, vol %d) — %zu packet(s)\n",
                np.title.c_str(), np.artist.empty() ? "?" : np.artist.c_str(),
                np.playing ? "playing" : "paused", np.volume, packets.size());
    return true;
  };
}

Action sendRaw(Bytes command) {
  return [command](Session& s) {
    std::printf("send:  %s\n", toHex(command).c_str());
    auto sameHeader = [&](const Bytes& v) {
      if (v.empty() || v[0] != command[0]) return false;
      std::printf("reply: %s\n", toHex(v).c_str());
      return false; // keep collecting until the timeout
    };
    s.request(command, sameHeader, 1500ms);
    return true;
  };
}

int listControllers() {
  auto all = s226::listUsbControllers();
  if (all.empty()) {
    std::puts("No USB Bluetooth controllers found.");
    return 1;
  }
  for (size_t i = 0; i < all.size(); ++i) {
    const auto& c = all[i];
    std::printf("%zu: %s\n", i, c.displayName().c_str());
    std::printf("     --controller %s  (or %s, %s)  node %s  driver %s\n", c.vidPid().c_str(),
                c.portPath.c_str(), std::to_string(i).c_str(), c.devNode().c_str(),
                c.kernelDriver.empty() ? "-" : c.kernelDriver.c_str());
  }
  return 0;
}

void usage(const char* argv0) {
  std::printf(
      "Usage: %s [options]\n"
      "\n"
      "Without a command option, measures heart rate until interrupted.\n"
      "\n"
      "  -l, --list              list USB Bluetooth controllers and exit\n"
      "  -c, --controller SEL    controller: index, vid:pid, port path, hciN (default: auto)\n"
      "  -a, --address ADDR      only connect to this watch address\n"
      "      --bp                measure blood pressure instead of heart rate\n"
      "  -t, --duration SEC      stop after SEC seconds\n"
      "  -v, --verbose           show protocol log and raw notifications\n"
      "  -V, --version\n"
      "\n"
      "Commands (run in order after connecting, then exit):\n"
      "  -i, --info              firmware and battery\n"
      "  -s, --settings          show the watch settings\n"
      "      --history[=DAY]     5-minute activity slots as CSV; DAY 0 = today (default)\n"
      "      --sleep[=DAY]       sleep sessions for DAY (0 = last night / today)\n"
      "      --workouts          workouts recorded in sport mode\n"
      "      --notify TEXT       show a message on the watch\n"
      "      --call NAME         ring with an incoming call from NAME (20 s at most)\n"
      "      --notify-type TYPE  message type for the following --notify (default other;\n"
      "                          must be switched on with --messages)\n"
      "      --messages LIST     message types the watch shows, comma-separated:\n"
      "                          call, sms, whatsapp, gmail, other, ... (see --settings),\n"
      "                          or all, none\n"
      "      --feature NAME=on|off\n"
      "                          switch a watch feature (see --settings), e.g.\n"
      "                          stopwatch=off, auto-hr=on, 24h=off (12-hour clock)\n"
      "      --alarms            list the alarms\n"
      "      --alarm ID=HH:MM[/DAYS]\n"
      "                          set alarm ID (1-...); DAYS is daily (default), once,\n"
      "                          or e.g. mon,tue,wed,thu,fri\n"
      "      --alarm-delete ID   delete alarm ID\n"
      "      --brightness auto|LEVEL\n"
      "                          automatic (dimmed 22:00-08:00) or a fixed level\n"
      "      --countdown SEC     preset of the watch's countdown timer\n"
      "      --watch-face N      select watch face N\n"
      "      --sedentary on|off|HH:MM-HH:MM/MIN\n"
      "                          sedentary reminder, e.g. 08:00-18:00/60\n"
      "      --hr-alarm on|off|LOW-HIGH\n"
      "                          heart-rate alarm limits in bpm, e.g. 50-115\n"
      "      --screen-time SEC   how long the screen stays on\n"
      "      --person H,W,AGE,m|f,GOAL[,SLEEP]\n"
      "                          height cm, weight kg, age, sex, step goal,\n"
      "                          sleep goal in minutes (default 480)\n"
      "      --weather [on|off]  show or set weather status on the watch\n"
      "      --contacts [NAME:PHONE,...]\n                          push contacts (empty value clears the list)\n"
      "      --contact-delete ID delete contact by id\n"
      "      --music TITLE[/ARTIST[/ALBUM]][;playing=0|1][;vol=N]\n                          push now-playing metadata to the watch\n"
      "      --send HEX          send a raw command and print the replies\n",
      argv0);
}

} // namespace

int main(int argc, char** argv) {
  s226::WatchOptions opts;
  bool verbose = false;
  bool bloodPressure = false;
  double duration = 0;
  std::vector<Action> actions;
  proto::MessageType notifyType = proto::MessageType::Other;

  enum {
    OptBp = 1000,
    OptSend,
    OptHistory,
    OptSleep,
    OptWorkouts,
    OptSedentary,
    OptHrAlarm,
    OptScreenTime,
    OptPerson,
    OptNotify,
    OptMessages,
    OptNotifyType,
    OptCall,
    OptFeature,
    OptAlarms,
    OptAlarm,
    OptAlarmDelete,
    OptBrightness,
    OptCountdown,
    OptWatchFace,
    OptWeather,
    OptContacts,
    OptContactDelete,
    OptMusic,
  };
  const option longopts[] = {{"list", no_argument, nullptr, 'l'},
                             {"controller", required_argument, nullptr, 'c'},
                             {"address", required_argument, nullptr, 'a'},
                             {"bp", no_argument, nullptr, OptBp},
                             {"duration", required_argument, nullptr, 't'},
                             {"verbose", no_argument, nullptr, 'v'},
                             {"version", no_argument, nullptr, 'V'},
                             {"help", no_argument, nullptr, 'h'},
                             {"info", no_argument, nullptr, 'i'},
                             {"settings", no_argument, nullptr, 's'},
                             {"history", optional_argument, nullptr, OptHistory},
                             {"sleep", optional_argument, nullptr, OptSleep},
                             {"workouts", no_argument, nullptr, OptWorkouts},
                             {"sedentary", required_argument, nullptr, OptSedentary},
                             {"hr-alarm", required_argument, nullptr, OptHrAlarm},
                             {"screen-time", required_argument, nullptr, OptScreenTime},
                             {"person", required_argument, nullptr, OptPerson},
                             {"notify", required_argument, nullptr, OptNotify},
                             {"messages", required_argument, nullptr, OptMessages},
                             {"notify-type", required_argument, nullptr, OptNotifyType},
                             {"call", required_argument, nullptr, OptCall},
                             {"feature", required_argument, nullptr, OptFeature},
                             {"alarms", no_argument, nullptr, OptAlarms},
                             {"alarm", required_argument, nullptr, OptAlarm},
                             {"alarm-delete", required_argument, nullptr, OptAlarmDelete},
                             {"brightness", required_argument, nullptr, OptBrightness},
                             {"countdown", required_argument, nullptr, OptCountdown},
                             {"watch-face", required_argument, nullptr, OptWatchFace},
                             {"weather", optional_argument, nullptr, OptWeather},
                             {"contacts", optional_argument, nullptr, OptContacts},
                             {"contact-delete", required_argument, nullptr, OptContactDelete},
                             {"music", required_argument, nullptr, OptMusic},
                             {"send", required_argument, nullptr, OptSend},
                             {nullptr, 0, nullptr, 0}};
  auto bad = [&](const char* what, const char* arg) {
    std::fprintf(stderr, "Invalid %s: %s\n", what, arg);
    return 2;
  };
  int ch;
  while ((ch = getopt_long(argc, argv, "lc:a:t:vVhis", longopts, nullptr)) != -1) {
    switch (ch) {
    case 'l': return listControllers();
    case 'c': opts.controller = optarg; break;
    case 'a': opts.address = optarg; break;
    case OptBp: bloodPressure = true; break;
    case 't': duration = std::stod(optarg); break;
    case 'v': verbose = true; break;
    case 'V': std::puts("s226-cli " S226_VERSION); return 0;
    case 'h': usage(argv[0]); return 0;
    case 'i': actions.push_back(showInfo); break;
    case 's': actions.push_back(showSettings); break;
    case OptWorkouts: actions.push_back(workouts); break;
    case OptHistory: {
      int day = 0;
      if (optarg) {
        char tail = 0;
        if (std::sscanf(optarg, "%d%c", &day, &tail) != 1 || day < 0 || day > 7) {
          return bad("day (0-7)", optarg);
        }
      }
      actions.push_back(history(day));
      break;
    }
    case OptSleep: {
      int day = 0;
      if (optarg) {
        char tail = 0;
        if (std::sscanf(optarg, "%d%c", &day, &tail) != 1 || day < 0 || day > 7) {
          return bad("day (0-7)", optarg);
        }
      }
      actions.push_back(sleepDay(day));
      break;
    }
    case OptSedentary: {
      const std::string v = optarg;
      if (v != "on" && v != "off" && !parseSedentary(v)) return bad("sedentary setting", optarg);
      actions.push_back(setSedentary(v));
      break;
    }
    case OptHrAlarm: {
      const std::string v = optarg;
      if (v != "on" && v != "off" && !parseHeartRateAlarm(v)) {
        return bad("heart-rate alarm", optarg);
      }
      actions.push_back(setHeartRateAlarm(v));
      break;
    }
    case OptScreenTime: {
      int sec = 0;
      char tail = 0;
      if (std::sscanf(optarg, "%d%c", &sec, &tail) != 1 || sec < 1 || sec > 255) {
        return bad("screen time", optarg);
      }
      actions.push_back(setScreenOnTime(sec));
      break;
    }
    case OptPerson: {
      auto p = parsePerson(optarg);
      if (!p) return bad("personal data", optarg);
      actions.push_back(setPerson(*p));
      break;
    }
    case OptNotify: actions.push_back(notify(optarg, notifyType)); break;
    case OptCall: actions.push_back(call(optarg)); break;
    case OptAlarms: actions.push_back(showAlarms); break;
    case OptAlarm: {
      int id = 0, n = 0;
      if (std::sscanf(optarg, "%d=%n", &id, &n) != 1 || n == 0 || id < 1 || id > 255) {
        return bad("alarm", optarg);
      }
      auto a = parseAlarm(id, optarg + n);
      if (!a) return bad("alarm", optarg);
      actions.push_back(setAlarm(*a));
      break;
    }
    case OptAlarmDelete:
    case OptCountdown:
    case OptWatchFace: {
      int value = 0;
      char tail = 0;
      const int max = ch == OptCountdown ? 0xFFFFFF : 255;
      if (std::sscanf(optarg, "%d%c", &value, &tail) != 1 || value < 0 || value > max) {
        return bad(ch == OptCountdown ? "countdown" : ch == OptWatchFace ? "watch face" : "alarm",
                   optarg);
      }
      actions.push_back(ch == OptCountdown    ? setCountdown(value)
                        : ch == OptWatchFace ? setWatchFace(value)
                                             : deleteAlarm(value));
      break;
    }
    case OptBrightness: {
      const std::string v = optarg;
      if (v != "auto" && std::atoi(optarg) < 1) return bad("brightness", optarg);
      actions.push_back(setBrightness(v));
      break;
    }
    case OptFeature: {
      auto f = parseFeature(optarg);
      if (!f) return bad("feature setting", optarg);
      actions.push_back(setFeature(f->first, f->second));
      break;
    }
    case OptNotifyType: {
      auto it = std::find(proto::kMessageTypeNames.begin(), proto::kMessageTypeNames.end(),
                          std::string_view(optarg));
      if (it == proto::kMessageTypeNames.end() || it == proto::kMessageTypeNames.begin()) {
        return bad("message type", optarg);
      }
      notifyType = static_cast<proto::MessageType>(it - proto::kMessageTypeNames.begin());
      break;
    }
    case OptMessages: {
      auto want = parseMessageTypes(optarg);
      if (!want) return bad("message types", optarg);
      actions.push_back(setMessageTypes(*want));
      break;
    }
    case OptWeather: {
      if (!optarg) {
        actions.push_back(weatherStatus(std::nullopt));
      } else if (std::string(optarg) == "on") {
        actions.push_back(weatherStatus(true));
      } else if (std::string(optarg) == "off") {
        actions.push_back(weatherStatus(false));
      } else {
        return bad("weather (on|off)", optarg);
      }
      break;
    }
    case OptContacts:
      actions.push_back(contactsPush(optarg ? optarg : ""));
      break;
    case OptContactDelete: {
      int id = 0;
      char tail = 0;
      if (std::sscanf(optarg, "%d%c", &id, &tail) != 1 || id < 1 || id > 255)
        return bad("contact id (1-255)", optarg);
      actions.push_back(contactDelete(id));
      break;
    }
    case OptMusic: actions.push_back(music(optarg)); break;
    case OptSend: {
      auto b = parseHex(optarg);
      if (!b) return bad("hex bytes", optarg);
      actions.push_back(sendRaw(*b));
      break;
    }
    default: usage(argv[0]); return 2;
    }
  }

  // With commands, stdout carries only their output.
  FILE* status = actions.empty() ? stdout : stderr;

  std::atomic<bool> failed{false};
  std::atomic<bool> connected{false};
  std::mutex infoMutex;
  std::optional<proto::DeviceInfo> deviceInfo;
  Inbox inbox;
  s226::Watch* watchPtr = nullptr;
  s226::WatchEvents ev;
  ev.stateChanged = [&](s226::WatchState s, const std::string& detail) {
    std::fprintf(status, "[%s] %s%s%s\n", timestamp().c_str(), s226::toString(s),
                 detail.empty() ? "" : ": ", detail.c_str());
    std::fflush(status);
    if (s == s226::WatchState::Error) {
      failed = true;
      g_quit = true;
    }
    connected = s == s226::WatchState::Connected;
    if (s == s226::WatchState::Connected && bloodPressure) {
      watchPtr->startBloodPressure(); // only queues; safe from the callback
    }
  };
  ev.deviceInfo = [&](const proto::DeviceInfo& info) {
    std::lock_guard lock(infoMutex);
    deviceInfo = info;
  };
  ev.rawNotification = [&](const Bytes& v) {
    if (verbose) {
      std::fprintf(status, "[%s] notify: %s\n", timestamp().c_str(), toHex(v).c_str());
    }
    if (!actions.empty()) inbox.push(v);
  };
  ev.musicControl = [](proto::MusicAction a) {
    std::printf("[%s] music: %s\n", timestamp().c_str(), proto::toString(a));
    std::fflush(stdout);
  };
  if (actions.empty()) {
    ev.heartRate = [](const proto::HeartRateSample& s) {
      if (s.sessionEnded) return;
      if (s.bpm == 0) {
        std::printf("[%s] heart rate: measuring...\n", timestamp().c_str());
      } else {
        std::printf("[%s] heart rate: %d bpm\n", timestamp().c_str(), s.bpm);
      }
      std::fflush(stdout);
    };
    ev.bloodPressure = [](const proto::BloodPressureSample& s) {
      if (s.done) {
        std::printf("[%s] blood pressure: %d/%d mmHg\n", timestamp().c_str(), s.systolic,
                    s.diastolic);
      } else if (!s.stopped) {
        std::printf("[%s] blood pressure: measuring %d%%\n", timestamp().c_str(), s.percent);
      }
      std::fflush(stdout);
    };
    ev.activity = [last = std::optional<uint32_t>(), rate = s226::StepRate()](
                      const proto::ActivityTotals& a) mutable {
      rate.add(s226::StepRate::Clock::now(), a.steps);
      if (last == a.steps) return;
      last = a.steps;
      if (auto spm = rate.stepsPerMinute()) {
        std::printf("[%s] steps today: %u (%.0f spm)\n", timestamp().c_str(), a.steps, *spm);
      } else {
        std::printf("[%s] steps today: %u\n", timestamp().c_str(), a.steps);
      }
      std::fflush(stdout);
    };
  }
  ev.log = [&](const std::string& m) {
    if (verbose || m.rfind("Warning", 0) == 0 || m.rfind("Using", 0) == 0) {
      std::fprintf(status, "[%s] %s\n", timestamp().c_str(), m.c_str());
      std::fflush(status);
    }
  };

  struct sigaction sa{};
  sa.sa_handler = onSignal;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  opts.startHeartRate = !bloodPressure && actions.empty();
  opts.reconnect = actions.empty();
  s226::Watch watch(ev);
  watchPtr = &watch;
  watch.start(opts);

  const auto start = Clock::now();
  auto timeUp = [&] {
    return duration > 0 && Clock::now() - start > std::chrono::duration<double>(duration);
  };

  if (!actions.empty()) {
    while (!g_quit && watch.isRunning() && !connected && !timeUp()) {
      std::this_thread::sleep_for(100ms);
    }
    if (connected) {
      // Let the status dumps that follow the bind go by.
      std::this_thread::sleep_for(1s);
      Session session(watch, inbox);
      {
        std::lock_guard lock(infoMutex);
        session.deviceInfo = deviceInfo;
      }
      for (const auto& action : actions) {
        if (g_quit || !connected) break;
        if (!action(session)) failed = true;
        std::fflush(stdout);
      }
    } else if (!g_quit) {
      failed = true;
    }
    watch.stop();
    return failed ? 1 : 0;
  }

  while (!g_quit && watch.isRunning() && !timeUp()) {
    std::this_thread::sleep_for(100ms);
  }
  watch.stop();
  return failed ? 1 : 0;
}
