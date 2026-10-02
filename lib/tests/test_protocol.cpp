// Decoder checks against frames from the H-Band capture (2026-08-25).

#include <cstdio>
#include <cstdlib>

#include "s226/protocol.hpp"

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

  // UUID wire order
  auto u = uuidToAtt(kNotifyUuid);
  CHECK(u[0] == 0x00 && u[15] == 0xF0 && u[14] == 0x08 && u[13] == 0x00 && u[12] == 0x02);

  if (failures == 0) std::puts("all protocol checks passed");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
