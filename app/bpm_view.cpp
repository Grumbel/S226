#include "bpm_view.hpp"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace {

const QColor kHeartColor(0xE0, 0x24, 0x5E);

// Heart shape in a unit box centred on the origin.
QPainterPath heartPath() {
  QPainterPath p;
  p.moveTo(0, 0.35);
  p.cubicTo(-0.55, -0.05, -0.5, -0.55, -0.25, -0.5);
  p.cubicTo(-0.1, -0.47, 0, -0.35, 0, -0.25);
  p.cubicTo(0, -0.35, 0.1, -0.47, 0.25, -0.5);
  p.cubicTo(0.5, -0.55, 0.55, -0.05, 0, 0.35);
  p.closeSubpath();
  return p;
}

// Largest pixel size at which `sample` fits into `box`.
int fitPixelSize(QFont font, const QString& sample, const QSizeF& box) {
  constexpr int kProbe = 200;
  font.setPixelSize(kProbe);
  const QRectF r = QFontMetricsF(font).tightBoundingRect(sample);
  if (r.width() <= 0 || r.height() <= 0) return 12;
  const qreal scale = std::min(box.width() / r.width(), box.height() / r.height());
  return std::max(8, static_cast<int>(kProbe * scale));
}

// Pixel size for one line of `text`: `maxPx` tall, shrunk to fit `width`.
int fitLine(QFont font, const QString& text, int maxPx, qreal width) {
  font.setPixelSize(maxPx);
  const qreal w = QFontMetricsF(font).horizontalAdvance(text);
  if (w <= width || w <= 0) return maxPx;
  return std::max(8, static_cast<int>(maxPx * width / w));
}

} // namespace

BpmView::BpmView(QWidget* parent) : QWidget(parent) {
  setMinimumSize(200, 150);
  setAutoFillBackground(true);
  pulseAnim_.setStartValue(1.0);
  pulseAnim_.setEndValue(0.0);
  pulseAnim_.setDuration(250);
  pulseAnim_.setEasingCurve(QEasingCurve::OutQuad);
  connect(&pulseAnim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
    pulse_ = v.toReal();
    update();
  });
}

void BpmView::setBpm(int bpm) {
  if (bpm_ == bpm) return;
  bpm_ = bpm;
  update();
}

void BpmView::setStale(bool stale) {
  if (stale_ == stale) return;
  stale_ = stale;
  update();
}

void BpmView::setStatus(const QString& text) {
  status_ = text;
  update();
}

void BpmView::setSecondary(const QString& text) {
  secondary_ = text;
  update();
}

void BpmView::setActivity(const QString& text) {
  if (activity_ == text) return;
  activity_ = text;
  update();
}

void BpmView::pulse() {
  pulseAnim_.stop();
  pulseAnim_.start();
}

void BpmView::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);

  const QRectF area = QRectF(rect()).adjusted(width() * 0.04, height() * 0.04,
                                              -width() * 0.04, -height() * 0.04);
  const qreal footerH = area.height() * 0.30;
  const QRectF numberRect(area.left(), area.top(), area.width(), area.height() - footerH);
  const QRectF footerRect(area.left(), numberRect.bottom(), area.width(), footerH);

  const QColor textColor = palette().color(QPalette::WindowText);
  QColor dimColor = textColor;
  dimColor.setAlphaF(0.35);

  // Number: size from "888" so the layout does not jump between values.
  QFont big = font();
  big.setBold(true);
  big.setPixelSize(fitPixelSize(big, QStringLiteral("888"), numberRect.size() * 0.92));
  p.setFont(big);
  p.setPen(stale_ || bpm_ <= 0 ? dimColor : textColor);
  const QString number = bpm_ > 0 ? QString::number(bpm_) : QStringLiteral("--");
  p.drawText(numberRect, Qt::AlignCenter, number);

  // Footer: [heart] bpm, steps line, status / secondary line.
  const qreal lineH = footerH * 0.42;
  const qreal activityH = footerH * 0.32;
  const qreal statusH = footerH - lineH - activityH;
  QFont label = font();
  label.setPixelSize(std::max(10, static_cast<int>(lineH * 0.8)));
  p.setFont(label);
  const QFontMetricsF fm(label);
  const QString unit = QStringLiteral("bpm");
  const qreal heartSize = lineH * 0.9;
  const qreal gap = heartSize * 0.35;
  const qreal total = heartSize + gap + fm.horizontalAdvance(unit);
  const qreal x0 = footerRect.center().x() - total / 2;
  const QPointF heartCenter(x0 + heartSize / 2, footerRect.top() + lineH / 2);

  p.save();
  p.translate(heartCenter);
  const qreal s = heartSize * (1.0 + 0.3 * pulse_);
  p.scale(s, s);
  QColor heart = kHeartColor;
  if (stale_ || bpm_ <= 0) heart.setAlphaF(0.4);
  p.setPen(Qt::NoPen);
  p.setBrush(heart);
  p.drawPath(heartPath());
  p.restore();

  p.setPen(textColor);
  p.drawText(QRectF(x0 + heartSize + gap, footerRect.top(), total, lineH),
             Qt::AlignLeft | Qt::AlignVCenter, unit);

  QFont mid = font();
  mid.setPixelSize(fitLine(mid, activity_, std::max(9, static_cast<int>(activityH * 0.75)),
                           footerRect.width()));
  p.setFont(mid);
  QColor activityColor = textColor;
  activityColor.setAlphaF(0.85);
  p.setPen(activityColor);
  const QRectF activityRect(footerRect.left(), footerRect.top() + lineH, footerRect.width(),
                            activityH);
  p.drawText(activityRect, Qt::AlignCenter | Qt::TextSingleLine,
             QFontMetricsF(mid).elidedText(activity_, Qt::ElideRight, footerRect.width()));

  QString line = status_;
  if (!secondary_.isEmpty()) line += (line.isEmpty() ? "" : QStringLiteral("   ·   ")) + secondary_;
  QFont small = font();
  small.setPixelSize(
      fitLine(small, line, std::max(9, static_cast<int>(statusH * 0.7)), footerRect.width()));
  p.setFont(small);
  QColor statusColor = textColor;
  statusColor.setAlphaF(0.6);
  p.setPen(statusColor);
  p.drawText(QRectF(footerRect.left(), activityRect.bottom(), footerRect.width(), statusH),
             Qt::AlignCenter | Qt::TextSingleLine,
             QFontMetricsF(small).elidedText(line, Qt::ElideRight, footerRect.width()));
}
