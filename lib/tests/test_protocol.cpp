// Decoder checks against frames from the H-Band captures and the watch.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "s226/protocol.hpp"
#include "s226/step_rate.hpp"

using namespace s226::protocol;

static int failures = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                       \
    }                                                                   \
  } while (0)

static Bytes frame(std::initializer_list<uint8_t> head) {
  Bytes b(head);
  b.resize(20, 0);
  return b;
}

int main() {
  // Heart rate
  auto hr = decodeHeartRate(frame({0xD0, 0x80}));
  CHECK(hr && hr->bpm == 128 && !hr->sessionEnded);
  hr = decodeHeartRate(frame({0xD0, 0x00, 0x00, 0x00, 0x00, 0x02}));
  CHECK(hr && hr->bpm == 0 && !hr->sessionEnded);
  hr = decodeHeartRate(frame({0xD0, 0x01}));
  CHECK(hr && hr->sessionEnded && hr->bpm == 0);
  CHECK(!decodeHeartRate(frame({0x90, 0x80})));

  // Blood pressure
  auto bp = decodeBloodPressure(frame({0x90, 0x8F, 0x5F, 0x64, 0x00, 0x01}));
  CHECK(bp && bp->done && bp->systolic == 143 && bp->diastolic == 95);
  bp = decodeBloodPressure(frame({0x90, 0x00, 0x00, 0x2E, 0x00, 0x01}));
  CHECK(bp && !bp->done && bp->percent == 46);
  bp = decodeBloodPressure(frame({0x90, 0x01}));
  CHECK(bp && bp->stopped);

  // Activity totals, 2026-08-24 16:24 (matches the 0xD1 history sums)
  auto act = decodeActivity(frame({0xD8, 0x00, 0xEC, 0x1E, 0x00, 0x00, 0xE3, 0x1C, 0x00, 0x00,
                                   0xE4, 0x12}));
  CHECK(act && act->steps == 7916 && act->distanceMeters == 7395 && act->calories == 4836);
  CHECK(!decodeActivity(frame({0xD0, 0x80})));

  // Step rate: 1 s polls, watch bumps its counter by 10 every 5 s = 120 spm
  {
    using namespace std::chrono;
    s226::StepRate rate;
    const auto t0 = s226::StepRate::Clock::time_point{};
    auto total = [](int sec) { return static_cast<uint32_t>(1000 + 10 * (sec / 5)); };
    rate.add(t0, total(0));
    for (int sec = 1; sec <= 7; ++sec) rate.add(t0 + seconds(sec), total(sec));
    CHECK(!rate.stepsPerMinute()); // only one counter change so far
    for (int sec = 8; sec <= 40; ++sec) rate.add(t0 + seconds(sec), total(sec));
    CHECK(rate.stepsPerMinute() && *rate.stepsPerMinute() > 119.9 &&
          *rate.stepsPerMinute() < 120.1);
    // Counter stops: 0 once it has been quiet for 2.5 update intervals.
    for (int sec = 41; sec <= 47; ++sec) rate.add(t0 + seconds(sec), total(40));
    CHECK(rate.stepsPerMinute() && *rate.stepsPerMinute() > 100);
    for (int sec = 48; sec <= 55; ++sec) rate.add(t0 + seconds(sec), total(40));
    CHECK(rate.stepsPerMinute() && *rate.stepsPerMinute() == 0.0);
    // Counter reset (midnight) starts over.
    rate.add(t0 + 60s, 3);
    CHECK(!rate.stepsPerMinute());
  }

  // Bind packet: capture sent 2026-08-25 17:50:43
  std::tm t{};
  t.tm_year = 2026 - 1900;
  t.tm_mon = 7;
  t.tm_mday = 25;
  t.tm_hour = 17;
  t.tm_min = 50;
  t.tm_sec = 43;
  Bytes expected{0xa1, 0x00, 0x00, 0x00, 0x07, 0xea, 0x08, 0x19, 0x11, 0x32,
                 0x2b, 0x01, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  CHECK(bindPacket(t) == expected);

  // Frames from the S226 itself (2026-10-08), MAC replaced by the example.
  auto info = decodeBindReply(Bytes{0xa1, 0x00, 0x00, 0x06, 0x03, 0x53, 0x01, 0x31, 0x06, 0x00,
                                    0x00, 0x01, 0xab, 0x89, 0x67, 0x45, 0x23, 0xc1, 0x00, 0x01});
  CHECK(info && info->bound && info->status == 6 && info->deviceNumber == 851 &&
        info->firmware == "01.31.06");

  auto battery = decodeBattery(frame({0xa0, 0x00, 0xcd, 0x00, 0x03}));
  CHECK(battery && battery->percent == 77 && battery->level == 3);

  CHECK(sedentaryRead() == Bytes({0xe1, 0, 0, 0, 0, 0, 0x02}));
  SedentaryReminder sed{true, 8, 0, 18, 0, 60};
  CHECK(sedentaryWrite(sed) == Bytes({0xe1, 0x08, 0x00, 0x12, 0x00, 0x3c, 0x01}));
  auto sedReply = decodeSedentary(frame({0xe1, 0x01, 0x08, 0x00, 0x12, 0x00, 0x3c, 0x01, 0x02}));
  CHECK(sedReply && sedReply->enabled && sedReply->startHour == 8 && sedReply->endHour == 18 &&
        sedReply->intervalMinutes == 60);
  sedReply = decodeSedentary(frame({0xe1, 0x01, 0x08, 0x00, 0x12, 0x00, 0x3c, 0x00, 0x00}));
  CHECK(sedReply && !sedReply->enabled);

  CHECK(heartRateAlarmWrite({true, 115, 52}) == Bytes({0xac, 0x73, 0x34, 0x01}));
  auto alarm = decodeHeartRateAlarm(frame({0xac, 0x73, 0x34, 0x01, 0x02, 0x01}));
  CHECK(alarm && alarm->enabled && alarm->high == 115 && alarm->low == 52);

  CHECK(screenOnTimeWrite(10) == Bytes({0xb4, 0x01, 0x0a}));
  auto screen = decodeScreenOnTime(frame({0xb4, 0x01, 0x02, 0x0a, 0x05, 0x1e, 0x03}));
  CHECK(screen && screen->seconds == 10 && screen->minSeconds == 5 && screen->maxSeconds == 30);

  // H-Band's person info write from the phone capture (2026-08-24)
  PersonInfo person{175, 60, 34, true, 9000, 480};
  CHECK(personInfoWrite(person) == Bytes({0xa3, 0xaf, 0x3c, 0x22, 0x01, 0x23, 0x28, 0x01, 0xe0}));
  CHECK(isPersonInfoAck(frame({0xa3, 0x01})));

  // "Hello world!" as shown by the watch, and a two-packet message
  auto msg = messagePackets("Hello world!");
  CHECK(msg.size() == 1 && msg[0] == Bytes({0xc2, 0x11, 0x0c, 0x01, 0x01, 0x02, 'H', 'e', 'l', 'l',
                                            'o', ' ', 'w', 'o', 'r', 'l', 'd', '!', 0x00, 0x00}));
  msg = messagePackets("0123456789abcdefg");
  CHECK(msg.size() == 2 && msg[0][2] == 14 && msg[0][3] == 2 && msg[0][4] == 1 &&
        msg[1][2] == 3 && msg[1][4] == 2 && msg[1][6] == 'e' && msg[1].size() == 20);

  msg = callerPackets("Alice");
  CHECK(msg.size() == 1 && msg[0] == Bytes({0xc2, 0x00, 0x05, 0x01, 0x01, 0x01, 'A', 'l', 'i', 'c',
                                            'e', 0, 0, 0, 0, 0, 0, 0, 0, 0}));
  msg = messagePackets("Hello SMS", MessageType::Sms);
  CHECK(msg.size() == 1 && msg[0][1] == 0x01 && msg[0][2] == 9);

  // 0xAD switch table as pushed after the bind
  auto sw = decodeMessageSwitches(Bytes{0xad, 0x02, 0x02, 0x02, 0x02, 0x02, 0x00, 0x02, 0x01, 0x00,
                                        0x02, 0x02, 0x02, 0x01, 0x01, 0x02, 0x02, 0x00, 0x00, 0x01});
  CHECK(sw && sw->state[0] == MessageSwitches::Off && sw->state[1] == MessageSwitches::Off &&
        sw->state[4] == MessageSwitches::Unsupported && sw->state[17] == MessageSwitches::On);
  sw->state[0] = sw->state[1] = MessageSwitches::On;
  auto swWrite = messageSwitchesWrite(*sw);
  CHECK(swWrite.size() == 20 && swWrite[0] == 0xad && swWrite[1] == 0x01 && swWrite[2] == 0x01 &&
        swWrite[3] == 0x01 && swWrite[19] == 0x01);

  // 0xB8 features as pushed after the bind
  auto feat = decodeWatchFeatures(Bytes{0xb8, 0x02, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x02, 0x01,
                                        0x00, 0x01, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00});
  CHECK(feat && (*feat)[kWatchFeatures[0]] == WatchFeatures::On &&
        (*feat)[kWatchFeatures[7]] == WatchFeatures::On &&       // stopwatch
        (*feat)[kWatchFeatures[6]] == WatchFeatures::Off &&      // find-phone
        (*feat)[kWatchFeatures[14]] == WatchFeatures::Unsupported); // music
  (*feat)[kWatchFeatures[7]] = WatchFeatures::Off;
  auto featWrite = watchFeaturesWrite(*feat);
  CHECK(featWrite.size() == 20 && featWrite[1] == 0x01 && featWrite[9] == 0x02 &&
        featWrite[2] == 0x01 && featWrite[19] == 0x00);

  // Brightness: automatic as read from the watch, manual as written
  auto bright = decodeBrightness(frame({0xb1, 0x01, 0x02, 0x16, 0x00, 0x08, 0x00, 0x02, 0x04,
                                        0x01, 0x0c}));
  CHECK(bright && bright->automatic && bright->startHour == 22 && bright->endHour == 8 &&
        bright->level == 2 && bright->otherLevel == 4 && bright->maxLevel == 12);
  CHECK(brightnessWrite(manualBrightness(12)) ==
        frame({0xb1, 0x01, 0x00, 0x00, 0x17, 0x3b, 0x0c, 0x0c, 0x02}));
  CHECK(brightnessWrite(automaticBrightness()) ==
        frame({0xb1, 0x01, 0x16, 0x00, 0x08, 0x00, 0x02, 0x04, 0x01}));

  // Countdown 90 s
  CHECK(countdownWrite(90) == Bytes({0xb2, 0x01, 0x00, 0x5a, 0x00, 0x00, 0x01}));
  auto cd = decodeCountdown(frame({0xb2, 0x02, 0x01, 0x00, 0x11, 0x0e, 0x00, 0x01}));
  CHECK(cd && cd->seconds == 3601 && cd->showOnWatch);

  // Watch face
  CHECK(watchFaceWrite(1) == frame({0xc7, 0x01, 0x01}));
  CHECK(decodeWatchFace(frame({0xc7, 0x01, 0x01, 0x01})) == 1);
  CHECK(decodeWatchFace(frame({0xc7, 0x02, 0x01, 0x00})) == 0);

  // Alarms
  Alarm once{5, 23, 45, true, 0, 2026, 10, 8};
  CHECK(alarmWrite(once) == frame({0xb9, 0x01, 0x05, 0x17, 0x2d, 0x01, 0x00, 0x00, 0xea, 0x07,
                                    0x0a, 0x08}));
  auto af = decodeAlarmFrame(Bytes{0xb9, 0x01, 0x01, 0x04, 0x02, 0x01, 0x00, 0x01, 0x01, 0x10,
                                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x3b});
  CHECK(af && af->ok && af->index == 1 && af->count == 4 && af->alarm.id == 1 &&
        af->alarm.hour == 0 && af->alarm.minute == 1 && af->alarm.days == 0x10);
  af = decodeAlarmFrame(Bytes{0xb9, 0x01, 0x00, 0x05, 0x01, 0x05, 0x17, 0x2d, 0x01, 0x00,
                              0x00, 0xea, 0x07, 0x0a, 0x08, 0x00, 0x00, 0x00, 0x2f, 0xa4});
  CHECK(af && af->ok && af->index == 0 && af->alarm.days == 0 && af->alarm.year == 2026 &&
        af->alarm.month == 10 && af->alarm.day == 8);
  af = decodeAlarmFrame(Bytes{0xb9, 0x00, 0x00, 0x04, 0x01, 0x05, 0x17, 0x2d, 0x01, 0x00,
                              0x00, 0x07, 0xea, 0x0a, 0x08, 0x00, 0x00, 0x00, 0x52, 0x3b});
  CHECK(af && !af->ok);

  CHECK(historyRead(1) == Bytes({0xd1, 0x01, 0x00, 0x01}));
  auto slot = decodeHistorySlot(Bytes{0xd1, 0x0d, 0x00, 0x20, 0x01, 0x21, 0x00, 0x00, 0x00, 0x00,
                                      0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x49, 0x01, 0x05});
  CHECK(slot && slot->index == 13 && slot->count == 288 && slot->daysAgo == 1 &&
        slot->hour == 1 && slot->minute == 5 && slot->heartRate == 73);
  slot = decodeHistorySlot(Bytes{0xd1, 0xc0, 0x00, 0xc2, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00,
                                 0x00, 0x00, 0x00, 0x68, 0x01, 0x00, 0x00, 0x48, 0x04, 0x0a});
  CHECK(slot && slot->index == 192 && slot->daysAgo == 0 && slot->hour == 16 &&
        slot->minute == 10 && slot->activity == 0x68 && slot->heartRate == 72);

  // Workout slot 1: 5 minutes, header totals all zero.
  std::vector<Bytes> workout{
      {0xd4, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0xea, 0x07, 0x0a,
       0x08, 0x16, 0x22, 0x28, 0xea, 0x07, 0x0a, 0x08, 0x16, 0x27},
      {0xd4, 0x02, 0x00, 0x08, 0x00, 0x01, 0x1b, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44},
      {0xd4, 0x03, 0x00, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00, 0x05,
       0x00, 0x00, 0x00, 0x00, 0x3a, 0x77, 0x00, 0x00, 0xff, 0xff},
      frame({0xd4, 0x04, 0x00, 0x08, 0x00, 0x01, 0x68, 0x10}),
      frame({0xd4, 0x05, 0x00, 0x08, 0x00, 0x01, 0x6d, 0x0c}),
      frame({0xd4, 0x06, 0x00, 0x08, 0x00, 0x01, 0x72, 0x0a}),
      frame({0xd4, 0x07, 0x00, 0x08, 0x00, 0x01, 0x7c, 0x0b}),
      frame({0xd4, 0x08, 0x00, 0x08, 0x00, 0x01, 0x75, 0x13}),
  };
  auto wf = decodeWorkoutFrame(workout[7]);
  CHECK(wf && wf->index == 8 && wf->count == 8 && wf->slot == 1);
  wf = decodeWorkoutFrame(frame({0xd4, 0x00, 0x00, 0x00, 0x00, 0x03}));
  CHECK(wf && wf->count == 0 && wf->slot == 3);
  auto w = decodeWorkout(workout);
  CHECK(w && w->start.year == 2026 && w->start.month == 10 && w->start.day == 8 &&
        w->start.hour == 22 && w->start.minute == 34 && w->start.second == 40);
  CHECK(w && w->end.minute == 39 && w->end.second == 27);
  CHECK(w && w->minutes.size() == 5 && w->minutes[0].heartRate == 104 &&
        w->minutes[0].activity == 16 && w->minutes[4].heartRate == 117);

  // Workout totals (2026-10-06 session): steps, distance, calories, activity
  std::vector<Bytes> totals{
      {0xd4, 0x01, 0x00, 0x03, 0x00, 0x02, 0x00, 0xea, 0x07, 0x0a,
       0x06, 0x08, 0x31, 0x33, 0xea, 0x07, 0x0a, 0x06, 0x0b, 0x2e},
      {0xd4, 0x02, 0x00, 0x03, 0x00, 0x02, 0x30, 0x2d, 0x18, 0x00,
       0x00, 0x29, 0x16, 0x00, 0x00, 0x61, 0xa9, 0x05, 0x00, 0xf4},
      {0xd4, 0x03, 0x00, 0x03, 0x00, 0x02, 0x1b, 0x00, 0x00, 0xb1,
       0x00, 0x01, 0x10, 0x0e, 0x68, 0x79, 0x00, 0x00, 0xff, 0xff},
  };
  w = decodeWorkout(totals);
  CHECK(w && w->end.hour == 11 && w->end.minute == 46 && w->end.second == 48);
  CHECK(w && w->steps == 6189 && w->distanceMeters == 5673 && w->calories == 371041 &&
        w->activity == 7156 && w->minutes.empty());
  w = decodeWorkout(std::vector<Bytes>{});
  CHECK(!w);

  // UUID wire order
  auto u = uuidToAtt(kNotifyUuid);
  CHECK(u[0] == 0x00 && u[15] == 0xF0 && u[14] == 0x08 && u[13] == 0x00 && u[12] == 0x02);

  if (failures == 0) std::puts("all protocol checks passed");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
