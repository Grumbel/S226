#include "trend_graph.hpp"

#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {

// Validated pair (dataviz palette: red / blue, light and dark steps).
struct SeriesStyle {
  QColor light;
  QColor dark;
  const char* unit;
};
const SeriesStyle kStyle[2] = {
    {QColor(0xE3, 0x49, 0x48), QColor(0xE6, 0x67, 0x67), "bpm"},
    {QColor(0x2A, 0x78, 0xD6), QColor(0x39, 0x87, 0xE5), "spm"},
};

// Samples further apart than this are drawn as separate segments.
constexpr qint64 kGapMs = 10'000;

double niceStep(double range, int maxTicks) {
  for (double step : {5.0, 10.0, 20.0, 25.0, 50.0, 100.0}) {
    if (range / step <= maxTicks) return step;
  }
  return 200.0;
}

qint64 timeStep(qint64 windowMs, qreal width) {
  const int maxTicks = std::max(2, static_cast<int>(width / 90));
  for (qint64 s : {10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600}) {
    if (windowMs / (s * 1000) <= maxTicks) return s * 1000;
  }
  return 7200 * 1000;
}

} // namespace

TrendGraph::TrendGraph(QWidget* parent) : QWidget(parent) {
  setMinimumHeight(110);
  setMouseTracking(true);
  tick_.setInterval(1000);
  connect(&tick_, &QTimer::timeout, this, qOverload<>(&QWidget::update));
  tick_.start();
}

void TrendGraph::setWindowSeconds(int seconds) {
  windowSeconds_ = std::clamp(seconds, 10, kMaxWindowSeconds);
  update();
}

void TrendGraph::addSample(Series series, qint64 msecs, double value) {
  auto& s = series_[series];
  if (!s.empty() && msecs < s.back().msecs) return; // keep sorted
  s.push_back({msecs, value});
  const qint64 cutoff = msecs - qint64(kMaxWindowSeconds) * 1000;
  while (!s.empty() && s.front().msecs < cutoff) s.pop_front();
}

void TrendGraph::clear() {
  series_[Bpm].clear();
  series_[Spm].clear();
  update();
}

const TrendGraph::Sample* TrendGraph::nearest(Series series, qint64 msecs,
                                              qint64 tolerance) const {
  const auto& s = series_[series];
  auto it = std::lower_bound(s.begin(), s.end(), msecs,
                             [](const Sample& a, qint64 t) { return a.msecs < t; });
  const Sample* best = nullptr;
  qint64 bestDist = tolerance + 1;
  for (auto c : {it, it == s.begin() ? s.end() : std::prev(it)}) {
    if (c == s.end()) continue;
    const qint64 d = std::llabs(c->msecs - msecs);
    if (d < bestDist) {
      bestDist = d;
      best = &*c;
    }
  }
  return best;
}

void TrendGraph::mouseMoveEvent(QMouseEvent* event) {
  hoverX_ = event->position().x();
  update();
}

void TrendGraph::leaveEvent(QEvent*) {
  hoverX_ = -1;
  update();
}

void TrendGraph::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);

  const QColor surface = palette().color(QPalette::Window);
  const bool dark = surface.lightness() < 128;
  const QColor text = palette().color(QPalette::WindowText);
  QColor muted = text;
  muted.setAlphaF(0.6);
  QColor grid = text;
  grid.setAlphaF(0.12);
  auto color = [&](int i) { return dark ? kStyle[i].dark : kStyle[i].light; };

  QFont f = font();
  f.setPixelSize(std::clamp(height() / 11, 10, 14));
  p.setFont(f);
  const QFontMetricsF fm(f);

  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const qint64 windowMs = qint64(windowSeconds_) * 1000;
  const qint64 t0 = now - windowMs;

  const QRectF outer = QRectF(rect()).adjusted(12, 4, -12, -4);
  const qreal legendH = fm.height() + 6;
  const qreal yLabelW = fm.horizontalAdvance(QStringLiteral("200")) + 8;
  const qreal xLabelH = fm.height() + 4;
  const QRectF plot(outer.left() + yLabelW, outer.top() + legendH,
                    outer.width() - yLabelW - 6, outer.height() - legendH - xLabelH);
  if (plot.width() < 40 || plot.height() < 20) return;

  // Y range from the visible data, padded to round numbers.
  double lo = 1e9, hi = -1e9;
  for (const auto& s : series_) {
    for (const auto& v : s) {
      if (v.msecs < t0) continue;
      lo = std::min(lo, v.value);
      hi = std::max(hi, v.value);
    }
  }
  const bool empty = lo > hi;
  if (empty) {
    lo = 40;
    hi = 140;
  }
  const double step = niceStep(std::max(20.0, hi - lo), std::max(2, int(plot.height() / 28)));
  lo = std::max(0.0, std::floor((lo - 2) / step) * step);
  hi = std::ceil((hi + 2) / step) * step;
  if (hi - lo < 2 * step) hi = lo + 2 * step;

  auto xOf = [&](qint64 ms) { return plot.left() + double(ms - t0) / windowMs * plot.width(); };
  auto yOf = [&](double v) { return plot.bottom() - (v - lo) / (hi - lo) * plot.height(); };

  // Grid and y labels.
  p.setPen(QPen(grid, 1));
  for (double v = lo; v <= hi + 1e-9; v += step) {
    const qreal y = std::round(yOf(v)) + 0.5;
    p.setPen(QPen(grid, 1));
    p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    p.setPen(muted);
    p.drawText(QRectF(outer.left(), y - fm.height() / 2, yLabelW - 6, fm.height()),
               Qt::AlignRight | Qt::AlignVCenter, QString::number(v));
  }

  // Time labels at round clock times.
  const qint64 tStep = timeStep(windowMs, plot.width());
  const qint64 offset = qint64(QDateTime::currentDateTime().offsetFromUtc()) * 1000;
  const QString timeFormat = tStep < 60'000 ? QStringLiteral("HH:mm:ss") : QStringLiteral("HH:mm");
  p.setPen(muted);
  for (qint64 t = ((t0 + offset) / tStep + 1) * tStep - offset; t <= now; t += tStep) {
    const qreal x = xOf(t);
    const QString label = QDateTime::fromMSecsSinceEpoch(t).toString(timeFormat);
    const qreal w = fm.horizontalAdvance(label);
    if (x - w / 2 < plot.left() || x + w / 2 > plot.right() + 6) continue;
    p.drawText(QRectF(x - w / 2, plot.bottom() + 2, w, xLabelH), Qt::AlignCenter, label);
  }

  // Series lines.
  p.save();
  p.setClipRect(plot.adjusted(-6, -6, 6, 6));
  for (int i : {Spm, Bpm}) {
    QPainterPath path;
    qint64 prev = -1;
    for (const auto& v : series_[i]) {
      if (v.msecs < t0 - kGapMs) continue;
      const QPointF pt(xOf(v.msecs), yOf(v.value));
      if (prev < 0 || v.msecs - prev > kGapMs) {
        path.moveTo(pt);
      } else {
        path.lineTo(pt);
      }
      prev = v.msecs;
    }
    p.setPen(QPen(color(i), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);

    // End marker for a live series: 8px dot with a 2px surface ring.
    if (!series_[i].empty() && now - series_[i].back().msecs < kGapMs) {
      const auto& last = series_[i].back();
      p.setPen(QPen(surface, 2));
      p.setBrush(color(i));
      p.drawEllipse(QPointF(xOf(last.msecs), yOf(last.value)), 5, 5);
    }
  }
  p.restore();

  // Legend: line key + current value in text color.
  qreal lx = plot.left();
  for (int i : {Bpm, Spm}) {
    const bool live = !series_[i].empty() && now - series_[i].back().msecs < kGapMs;
    const QString label =
        live ? QStringLiteral("%1 %2").arg(std::lround(series_[i].back().value)).arg(kStyle[i].unit)
             : QString::fromLatin1(kStyle[i].unit);
    const qreal cy = outer.top() + legendH / 2 - 1;
    p.setPen(QPen(color(i), 2, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(lx, cy), QPointF(lx + 14, cy));
    p.setPen(text);
    p.drawText(QRectF(lx + 20, outer.top(), 200, legendH - 2), Qt::AlignLeft | Qt::AlignVCenter,
               label);
    lx += 20 + fm.horizontalAdvance(label) + 18;
  }

  if (empty) {
    p.setPen(muted);
    p.drawText(plot, Qt::AlignCenter, tr("No readings in this time window"));
    return;
  }

  // Hover: crosshair and tooltip with the nearest readings.
  if (hoverX_ >= plot.left() && hoverX_ <= plot.right()) {
    const qint64 t = t0 + qint64((hoverX_ - plot.left()) / plot.width() * windowMs);
    const qint64 tolerance = std::max<qint64>(3000, qint64(windowMs / plot.width() * 6));
    const Sample* hits[2] = {nearest(Bpm, t, tolerance), nearest(Spm, t, tolerance)};
    if (hits[0] || hits[1]) {
      const qint64 at = hits[0] ? hits[0]->msecs : hits[1]->msecs;
      const qreal x = std::round(xOf(at)) + 0.5;
      p.setPen(QPen(muted, 1));
      p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));

      QStringList lines{QDateTime::fromMSecsSinceEpoch(at).toString("HH:mm:ss")};
      for (int i : {Bpm, Spm}) {
        if (hits[i]) {
          lines << QStringLiteral("%1 %2").arg(std::lround(hits[i]->value)).arg(kStyle[i].unit);
        }
      }
      qreal w = 0;
      for (const auto& l : lines) w = std::max(w, fm.horizontalAdvance(l));
      w += 34;
      const qreal h = lines.size() * fm.height() + 10;
      QRectF box(x + 10, plot.top() + 4, w, h);
      if (box.right() > plot.right()) box.moveRight(x - 10);
      QColor boxBg = palette().color(QPalette::Base);
      p.setPen(QPen(grid, 1));
      p.setBrush(boxBg);
      p.drawRoundedRect(box, 4, 4);
      qreal y = box.top() + 5;
      int row = 0;
      for (const auto& l : lines) {
        if (row > 0) {
          const int i = (row == 1 && hits[0]) ? Bpm : Spm;
          const QPointF dot(box.left() + 12, y + fm.height() / 2);
          p.setPen(QPen(color(i), 2));
          p.drawLine(dot - QPointF(5, 0), dot + QPointF(5, 0));
          const qreal vy = yOf(hits[i]->value);
          p.setPen(QPen(surface, 2));
          p.setBrush(color(i));
          p.drawEllipse(QPointF(x, vy), 4, 4);
        }
        p.setPen(row == 0 ? muted : text);
        p.drawText(QRectF(box.left() + 24, y, w, fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   l);
        y += fm.height();
        ++row;
      }
    }
  }
}
