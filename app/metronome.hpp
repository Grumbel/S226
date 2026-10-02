// Beats at the current heart rate; optionally clicks.
//
// The beat() signal runs whenever a rate is set (drives the heart pulse);
// sound is separate so the visual pulse works with the metronome muted.

#pragma once

#include <QObject>
#include <QSoundEffect>
#include <QTemporaryDir>
#include <QTimer>

class Metronome : public QObject {
  Q_OBJECT

public:
  explicit Metronome(QObject* parent = nullptr);

  void setBpm(int bpm); // <= 0 stops
  void setSoundEnabled(bool enabled);
  void setVolume(qreal volume); // 0..1
  bool soundEnabled() const { return soundEnabled_; }

signals:
  void beat();

private:
  void tick();
  void scheduleNext();

  int bpm_ = 0;
  bool soundEnabled_ = false;
  QTimer timer_;
  QTemporaryDir tempDir_;
  QSoundEffect click_;
};
