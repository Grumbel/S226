#include "metronome.hpp"

#include <QFile>
#include <QUrl>
#include <QtEndian>

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

// Short, soft click: 1.2 kHz tone with a fast attack and exponential decay.
QByteArray makeClickWav() {
  constexpr int kRate = 44100;
  constexpr double kSeconds = 0.06;
  constexpr int kSamples = static_cast<int>(kRate * kSeconds);
  std::vector<int16_t> pcm(kSamples);
  for (int i = 0; i < kSamples; ++i) {
    const double t = static_cast<double>(i) / kRate;
    const double attack = std::min(1.0, t / 0.002);
    const double env = attack * std::exp(-t * 70.0);
    pcm[static_cast<size_t>(i)] =
        static_cast<int16_t>(0.8 * 32767 * env * std::sin(2 * M_PI * 1200.0 * t));
  }

  QByteArray wav;
  auto u32 = [&](uint32_t v) {
    v = qToLittleEndian(v);
    wav.append(reinterpret_cast<const char*>(&v), 4);
  };
  auto u16 = [&](uint16_t v) {
    v = qToLittleEndian(v);
    wav.append(reinterpret_cast<const char*>(&v), 2);
  };
  const uint32_t dataBytes = static_cast<uint32_t>(pcm.size() * 2);
  wav.append("RIFF");
  u32(36 + dataBytes);
  wav.append("WAVEfmt ");
  u32(16);
  u16(1); // PCM
  u16(1); // mono
  u32(kRate);
  u32(kRate * 2);
  u16(2);
  u16(16);
  wav.append("data");
  u32(dataBytes);
  for (int16_t s : pcm) u16(static_cast<uint16_t>(s));
  return wav;
}

} // namespace

Metronome::Metronome(QObject* parent) : QObject(parent) {
  timer_.setSingleShot(true);
  timer_.setTimerType(Qt::PreciseTimer);
  connect(&timer_, &QTimer::timeout, this, &Metronome::tick);

  if (tempDir_.isValid()) {
    const QString path = tempDir_.filePath(QStringLiteral("click.wav"));
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
      f.write(makeClickWav());
      f.close();
      click_.setSource(QUrl::fromLocalFile(path));
    }
  }
  click_.setVolume(0.6f);
}

void Metronome::setBpm(int bpm) {
  bpm_ = bpm;
  if (bpm_ <= 0) {
    timer_.stop();
  } else if (!timer_.isActive()) {
    // Keep the running phase when the rate changes; only the next
    // interval uses the new rate.
    scheduleNext();
  }
}

void Metronome::setSoundEnabled(bool enabled) { soundEnabled_ = enabled; }

void Metronome::setVolume(qreal volume) { click_.setVolume(static_cast<float>(volume)); }

void Metronome::tick() {
  if (bpm_ <= 0) return;
  if (soundEnabled_) click_.play();
  emit beat();
  scheduleNext();
}

void Metronome::scheduleNext() { timer_.start(60000 / std::max(bpm_, 20)); }
