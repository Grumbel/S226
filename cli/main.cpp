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
  return true;
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

Action notify(std::string text) {
  return [text](Session& s) {
    for (const auto& packet : proto::messagePackets(text)) {
      s.watch().send(packet);
      std::this_thread::sleep_for(120ms);
    }
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
      "      --workouts          workouts recorded in sport mode\n"
      "      --notify TEXT       show a message on the watch\n"
      "      --sedentary on|off|HH:MM-HH:MM/MIN\n"
      "                          sedentary reminder, e.g. 08:00-18:00/60\n"
      "      --hr-alarm on|off|LOW-HIGH\n"
      "                          heart-rate alarm limits in bpm, e.g. 50-115\n"
      "      --screen-time SEC   how long the screen stays on\n"
      "      --person H,W,AGE,m|f,GOAL[,SLEEP]\n"
      "                          height cm, weight kg, age, sex, step goal,\n"
      "                          sleep goal in minutes (default 480)\n"
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

  enum {
    OptBp = 1000,
    OptSend,
    OptHistory,
    OptWorkouts,
    OptSedentary,
    OptHrAlarm,
    OptScreenTime,
    OptPerson,
    OptNotify,
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
                             {"workouts", no_argument, nullptr, OptWorkouts},
                             {"sedentary", required_argument, nullptr, OptSedentary},
                             {"hr-alarm", required_argument, nullptr, OptHrAlarm},
                             {"screen-time", required_argument, nullptr, OptScreenTime},
                             {"person", required_argument, nullptr, OptPerson},
                             {"notify", required_argument, nullptr, OptNotify},
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
    case OptNotify: actions.push_back(notify(optarg)); break;
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
