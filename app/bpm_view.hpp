// Big heart-rate number that scales with the widget, plus a pulsing heart.

#pragma once

#include <QString>
#include <QVariantAnimation>
#include <QWidget>

class BpmView : public QWidget {
  Q_OBJECT

public:
  explicit BpmView(QWidget* parent = nullptr);

  void setBpm(int bpm);        // <= 0 shows a placeholder
  void setStale(bool stale);   // dim the number (no recent reading)
  void setStatus(const QString& text);
  void setSecondary(const QString& text); // e.g. last blood pressure
  void setActivity(const QString& text);  // e.g. "7,916 steps"

public slots:
  void pulse();

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  int bpm_ = 0;
  bool stale_ = true;
  QString status_;
  QString secondary_;
  QString activity_;
  qreal pulse_ = 0.0; // 1 right after a beat, decays to 0
  QVariantAnimation pulseAnim_;
};
