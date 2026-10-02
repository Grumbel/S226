// Line graph of bpm and spm over a configurable time window ending now.
// Both are per-minute rates, so they share one y-axis.

#pragma once

#include <QTimer>
#include <QWidget>

#include <deque>

class TrendGraph : public QWidget {
  Q_OBJECT

public:
  enum Series { Bpm = 0, Spm = 1 };
  static constexpr int kMaxWindowSeconds = 2 * 60 * 60;

  explicit TrendGraph(QWidget* parent = nullptr);

  void setWindowSeconds(int seconds);
  int windowSeconds() const { return windowSeconds_; }

  void addSample(Series series, qint64 msecsSinceEpoch, double value);
  void clear();

protected:
  void paintEvent(QPaintEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;

private:
  struct Sample {
    qint64 msecs;
    double value;
  };

  const Sample* nearest(Series series, qint64 msecs, qint64 tolerance) const;

  std::deque<Sample> series_[2];
  int windowSeconds_ = 300;
  qreal hoverX_ = -1;
  QTimer tick_;
};
