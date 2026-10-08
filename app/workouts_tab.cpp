#include "workouts_tab.hpp"

#include <algorithm>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <functional>
#include <memory>

#include "watch_bridge.hpp"

namespace proto = s226::protocol;

WorkoutsTab::WorkoutsTab(WatchBridge& bridge, QWidget* parent)
    : QWidget(parent), bridge_(bridge) {
  buildUi();
  setConnected(false);
}

void WorkoutsTab::buildUi() {
  auto* lay = new QVBoxLayout(this);
  auto* row = new QHBoxLayout;
  fetchBtn_ = new QPushButton(tr("Fetch workouts"), this);
  status_ = new QLabel(this);
  connect(fetchBtn_, &QPushButton::clicked, this, &WorkoutsTab::fetch);
  row->addWidget(fetchBtn_);
  row->addWidget(status_, 1);
  lay->addLayout(row);

  tree_ = new QTreeWidget(this);
  tree_->setHeaderLabels(
      {tr("Workout"), tr("Duration"), tr("Steps"), tr("Distance"), tr("kcal"), tr("HR")});
  tree_->setUniformRowHeights(true);
  tree_->setAlternatingRowColors(true);
  tree_->setRootIsDecorated(true);
  tree_->setAnimated(true);
  tree_->header()->setStretchLastSection(true);
  lay->addWidget(tree_, 1);
}

void WorkoutsTab::setConnected(bool connected) {
  connected_ = connected;
  fetchBtn_->setEnabled(connected);
  if (!connected) {
    tree_->clear();
    status_->setText(tr("Connect to a watch to fetch workouts."));
  }
}

void WorkoutsTab::fetch() {
  if (!connected_) return;
  status_->setText(tr("Fetching workouts..."));
  fetchBtn_->setEnabled(false);
  tree_->clear();

  auto slot = std::make_shared<int>(1);
  auto found = std::make_shared<int>(0);
  auto fetchOne = std::make_shared<std::function<void()>>();

  *fetchOne = [this, slot, found, fetchOne]() {
    if (*slot > proto::kWorkoutSlots) {
      fetchBtn_->setEnabled(connected_);
      status_->setText(*found == 0 ? tr("No workouts stored.")
                                   : tr("%1 workout(s).").arg(*found));
      if (tree_->topLevelItemCount() > 0) tree_->expandItem(tree_->topLevelItem(0));
      for (int c = 0; c < tree_->columnCount(); ++c) tree_->resizeColumnToContents(c);
      return;
    }
    const int s = *slot;
    bridge_.requestStream(
        proto::workoutRead(s),
        [s](const proto::Bytes& v) {
          auto f = proto::decodeWorkoutFrame(v);
          return f && (f->slot == s || f->count == 0);
        },
        [](const proto::Bytes& v) {
          auto f = proto::decodeWorkoutFrame(v);
          return f && (f->count == 0 || f->index >= f->count);
        },
        [this, slot, found, fetchOne](std::vector<proto::Bytes> frames) {
          if (!frames.empty()) {
            auto f0 = proto::decodeWorkoutFrame(frames.front());
            if (f0 && f0->count > 0) {
              auto w = proto::decodeWorkout(frames);
              if (w) {
                ++(*found);
                int hrSum = 0, hrCount = 0, hrMax = 0;
                for (const auto& m : w->minutes) {
                  if (m.heartRate == 0) continue;
                  hrSum += m.heartRate;
                  ++hrCount;
                  hrMax = std::max(hrMax, m.heartRate);
                }
                auto* top = new QTreeWidgetItem(tree_);
                top->setText(0, QStringLiteral("%1-%2-%3 %4:%5 → %6:%7")
                    .arg(w->start.year, 4, 10, QChar('0'))
                    .arg(w->start.month, 2, 10, QChar('0'))
                    .arg(w->start.day, 2, 10, QChar('0'))
                    .arg(w->start.hour, 2, 10, QChar('0'))
                    .arg(w->start.minute, 2, 10, QChar('0'))
                    .arg(w->end.hour, 2, 10, QChar('0'))
                    .arg(w->end.minute, 2, 10, QChar('0')));
                top->setText(1, tr("%1 min").arg(w->minutes.size()));
                top->setText(2, QString::number(w->steps));
                top->setText(3, tr("%1 m").arg(w->distanceMeters));
                top->setText(4, QString::number(w->calories / 1000.0, 'f', 1));
                if (hrCount)
                  top->setText(5, tr("avg %1 max %2").arg(hrSum / hrCount).arg(hrMax));
                for (size_t i = 0; i < w->minutes.size(); ++i) {
                  const auto& m = w->minutes[i];
                  auto* child = new QTreeWidgetItem(top);
                  child->setText(0, tr("min %1").arg(i + 1));
                  child->setText(2, QString::number(m.steps));
                  child->setText(3, tr("%1 m").arg(m.distanceMeters));
                  child->setText(4, QString::number(m.calories / 1000.0, 'f', 3));
                  child->setText(5, m.heartRate ? QString::number(m.heartRate) : QStringLiteral("—"));
                }
              }
            }
          }
          ++(*slot);
          (*fetchOne)();
        },
        15000);
  };
  (*fetchOne)();
}
