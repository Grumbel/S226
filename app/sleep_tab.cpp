#include "sleep_tab.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include "ui_icons.hpp"
#include <QTableWidget>
#include <QVBoxLayout>

#include "watch_bridge.hpp"

namespace proto = s226::protocol;

SleepTab::SleepTab(WatchBridge& bridge, QWidget* parent)
    : QWidget(parent), bridge_(bridge) {
  buildUi();
  setConnected(false);
}

void SleepTab::buildUi() {
  auto* lay = new QVBoxLayout(this);
  auto* row = new QHBoxLayout;
  dayBox_ = new QComboBox(this);
  for (int d = 0; d <= 7; ++d)
    dayBox_->addItem(d == 0 ? tr("Last night / today") : tr("%1 day(s) ago").arg(d), d);
  fetchBtn_ = new QPushButton(tr("Fetch"), this);
  setButtonIcon(fetchBtn_, QStringLiteral("fetch"));
  status_ = new QLabel(this);
  connect(fetchBtn_, &QPushButton::clicked, this, &SleepTab::fetch);
  row->addWidget(new QLabel(tr("Day:"), this));
  row->addWidget(dayBox_);
  row->addWidget(fetchBtn_);
  row->addWidget(status_, 1);
  lay->addLayout(row);

  table_ = new QTableWidget(0, 8, this);
  table_->setHorizontalHeaderLabels({tr("Fell asleep"), tr("Woke"), tr("Deep (min)"),
                                     tr("Light (min)"), tr("Total (min)"), tr("Quality"),
                                     tr("Wakes"), tr("Stages")});
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setAlternatingRowColors(true);
  table_->setShowGrid(false);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->verticalHeader()->setVisible(false);
  lay->addWidget(table_, 1);
}

void SleepTab::setConnected(bool connected) {
  connected_ = connected;
  fetchBtn_->setEnabled(connected);
  if (!connected) {
    table_->setRowCount(0);
    status_->setText(tr("Connect to a watch to fetch sleep data."));
  }
}

void SleepTab::fetch() {
  if (!connected_) return;
  status_->setText(tr("Fetching sleep..."));
  fetchBtn_->setEnabled(false);
  table_->setRowCount(0);
  const int day = dayBox_->currentData().toInt();

  bridge_.requestStream(
      proto::sleepRead(day),
      [day](const proto::Bytes& v) {
        auto f = proto::decodeSleepFrame(v);
        return f && f->dayIndex == day;
      },
      [](const proto::Bytes& v) {
        auto f = proto::decodeSleepFrame(v);
        return f && f->packetIndex == 0;
      },
      [this](std::vector<proto::Bytes> frames) {
        fetchBtn_->setEnabled(connected_);
        if (frames.empty()) {
          status_->setText(tr("No sleep data (or transfer failed)."));
          return;
        }
        auto dayData = proto::decodeSleepDay(frames);
        if (!dayData) {
          status_->setText(tr("Could not decode sleep data."));
          return;
        }
        if (dayData->empty) {
          status_->setText(tr("No sleep recorded for this day."));
          return;
        }
        auto fmtTime = [](const proto::SleepTime& t) -> QString {
          if (t.year > 0 && t.month > 0)
            return QStringLiteral("%1-%2-%3 %4:00")
                .arg(t.year, 4, 10, QChar('0'))
                .arg(t.month, 2, 10, QChar('0'))
                .arg(t.day, 2, 10, QChar('0'))
                .arg(t.hour, 2, 10, QChar('0'));
          return QStringLiteral("%1:00").arg(t.hour, 2, 10, QChar('0'));
        };
        table_->setRowCount(int(dayData->sessions.size()));
        int row = 0;
        for (const auto& s : dayData->sessions) {
          table_->setItem(row, 0, new QTableWidgetItem(fmtTime(s.sleepDown)));
          table_->setItem(row, 1, new QTableWidgetItem(fmtTime(s.sleepUp)));
          table_->setItem(row, 2, new QTableWidgetItem(QString::number(s.deepMinutes)));
          table_->setItem(row, 3, new QTableWidgetItem(QString::number(s.lightMinutes)));
          table_->setItem(row, 4, new QTableWidgetItem(QString::number(s.totalMinutes)));
          table_->setItem(row, 5, new QTableWidgetItem(s.quality ? QString::number(s.quality)
                                                                : QStringLiteral("—")));
          table_->setItem(row, 6, new QTableWidgetItem(QString::number(s.wakeCount)));
          QString stages = QString::fromStdString(s.stages);
          if (stages.size() > 80) stages = stages.left(80) + QChar(0x2026);
          auto* stagesItem = new QTableWidgetItem(stages.isEmpty() ? QStringLiteral("—") : stages);
          if (s.v1)
            stagesItem->setToolTip(
                tr("V1 scores — up:%1 deep:%2 eff:%3 fall:%4 time:%5; %6 min/point")
                    .arg(s.getUpScore)
                    .arg(s.deepScore)
                    .arg(s.efficiencyScore)
                    .arg(s.fallAsleepScore)
                    .arg(s.sleepTimeScore)
                    .arg(s.onePointDuration));
          table_->setItem(row, 7, stagesItem);
          ++row;
        }
        table_->resizeColumnsToContents();
        table_->horizontalHeader()->setStretchLastSection(true);
        status_->setText(tr("%1 session(s).").arg(row));
      },
      30000);
}
