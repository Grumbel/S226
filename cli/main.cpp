// s226-cli: list Bluetooth controllers and print S226 heart rate.

#include <getopt.h>
#include <signal.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <thread>

#include "s226/usb.hpp"
#include "s226/watch.hpp"

#ifndef S226_VERSION
#define S226_VERSION "dev"
#endif

namespace {

std::atomic<bool> g_quit{false};

void onSignal(int) { g_quit = true; }

std::string timestamp() {
  std::time_t now = std::time(nullptr);
  char buf[16];
  std::strftime(buf, sizeof buf, "%H:%M:%S", std::localtime(&now));
  return buf;
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
      "  -l, --list              list USB Bluetooth controllers and exit\n"
      "  -c, --controller SEL    controller: index, vid:pid, port path, hciN (default: auto)\n"
      "  -a, --address ADDR      only connect to this watch address\n"
      "      --bp                measure blood pressure instead of heart rate\n"
      "  -t, --duration SEC      stop after SEC seconds\n"
      "  -v, --verbose           show protocol log and raw notifications\n"
      "  -V, --version\n",
      argv0);
}

} // namespace

int main(int argc, char** argv) {
  s226::WatchOptions opts;
  bool verbose = false;
  bool bloodPressure = false;
  double duration = 0;

  enum { OptBp = 1000 };
  const option longopts[] = {
      {"list", no_argument, nullptr, 'l'},       {"controller", required_argument, nullptr, 'c'},
      {"address", required_argument, nullptr, 'a'}, {"bp", no_argument, nullptr, OptBp},
      {"duration", required_argument, nullptr, 't'}, {"verbose", no_argument, nullptr, 'v'},
      {"version", no_argument, nullptr, 'V'},    {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0}};
  int ch;
  while ((ch = getopt_long(argc, argv, "lc:a:t:vVh", longopts, nullptr)) != -1) {
    switch (ch) {
    case 'l': return listControllers();
    case 'c': opts.controller = optarg; break;
    case 'a': opts.address = optarg; break;
    case OptBp: bloodPressure = true; break;
    case 't': duration = std::stod(optarg); break;
    case 'v': verbose = true; break;
    case 'V': std::puts("s226-cli " S226_VERSION); return 0;
    case 'h': usage(argv[0]); return 0;
    default: usage(argv[0]); return 2;
    }
  }

  std::atomic<bool> failed{false};
  s226::Watch* watchPtr = nullptr;
  s226::WatchEvents ev;
  ev.stateChanged = [&](s226::WatchState s, const std::string& detail) {
    std::printf("[%s] %s%s%s\n", timestamp().c_str(), s226::toString(s),
                detail.empty() ? "" : ": ", detail.c_str());
    std::fflush(stdout);
    if (s == s226::WatchState::Error) {
      failed = true;
      g_quit = true;
    }
    if (s == s226::WatchState::Connected && bloodPressure) {
      watchPtr->startBloodPressure(); // only queues; safe from the callback
    }
  };
  ev.heartRate = [](const s226::protocol::HeartRateSample& s) {
    if (s.sessionEnded) return;
    if (s.bpm == 0) {
      std::printf("[%s] heart rate: measuring...\n", timestamp().c_str());
    } else {
      std::printf("[%s] heart rate: %d bpm\n", timestamp().c_str(), s.bpm);
    }
    std::fflush(stdout);
  };
  ev.bloodPressure = [](const s226::protocol::BloodPressureSample& s) {
    if (s.done) {
      std::printf("[%s] blood pressure: %d/%d mmHg\n", timestamp().c_str(), s.systolic,
                  s.diastolic);
    } else if (!s.stopped) {
      std::printf("[%s] blood pressure: measuring %d%%\n", timestamp().c_str(), s.percent);
    }
    std::fflush(stdout);
  };
  ev.activity = [last = std::optional<uint32_t>()](
                    const s226::protocol::ActivityTotals& a) mutable {
    if (last == a.steps) return;
    last = a.steps;
    std::printf("[%s] steps today: %u\n", timestamp().c_str(), a.steps);
    std::fflush(stdout);
  };
  if (verbose) {
    ev.log = [](const std::string& m) {
      std::printf("[%s] %s\n", timestamp().c_str(), m.c_str());
      std::fflush(stdout);
    };
    ev.rawNotification = [](const s226::protocol::Bytes& v) {
      std::string h;
      char b[4];
      for (uint8_t x : v) {
        std::snprintf(b, sizeof b, "%02x ", x);
        h += b;
      }
      std::printf("[%s] notify: %s\n", timestamp().c_str(), h.c_str());
    };
  } else {
    ev.log = [](const std::string& m) {
      if (m.rfind("Warning", 0) == 0 || m.rfind("Using", 0) == 0) {
        std::printf("[%s] %s\n", timestamp().c_str(), m.c_str());
        std::fflush(stdout);
      }
    };
  }

  struct sigaction sa{};
  sa.sa_handler = onSignal;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  opts.startHeartRate = !bloodPressure;
  s226::Watch watch(ev);
  watchPtr = &watch;
  watch.start(opts);

  const auto start = std::chrono::steady_clock::now();
  while (!g_quit && watch.isRunning()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (duration > 0 &&
        std::chrono::steady_clock::now() - start > std::chrono::duration<double>(duration)) {
      break;
    }
  }
  watch.stop();
  return failed ? 1 : 0;
}
