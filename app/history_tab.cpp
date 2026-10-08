#include "history_tab.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "watch_bridge.hpp"

namespace proto = s226::protocol;

HistoryTab::HistoryTab(WatchBridge& bridge, QWidget* parent)
    : QWidget(parent), bridge_(bridge) {
  buildUi();
  setConnected(false);
}

void HistoryTab::buildUi() {
  auto* lay = new QVBoxLayout(this);
  auto* row = new QHBoxLayout;
  dayBox_ = new QComboBox(this);
  for (int d = 0; d <= 7; ++d)
    dayBox_->addItem(d == 0 ? tr("Today") : tr("%1 day(s) ago").arg(d), d);
  fetchBtn_ = new QPushButton(tr("Fetch"), this);
  status_ = new QLabel(this);
  connect(fetchBtn_, &QPushButton::clicked, this, &HistoryTab::fetch);
  row->addWidget(new QLabel(tr("Day:"), this));
  row->addWidget(dayBox_);
  row->addWidget(fetchBtn_);
  row->addWidget(status_, 1);
  lay->addLayout(row);

  table_ = new QTableWidget(0, 6, this);
  table_->setHorizontalHeaderLabels(
      {tr("Time"), tr("Steps"), tr("Distance (m)"), tr("kcal"), tr("Activity"), tr("HR")});
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setAlternatingRowColors(true);
  table_->setShowGrid(false);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->verticalHeader()->setVisible(false);
  lay->addWidget(table_, 1);
}

void HistoryTab::setConnected(bool connected) {
  connected_ = connected;
  fetchBtn_->setEnabled(connected);
  if (!connected) {
    table_->setRowCount(0);
    status_->setText(tr("Connect to a watch to fetch history."));
  }
}

void HistoryTab::fetch() {
  if (!connected_) return;
  status_->setText(tr("Fetching history..."));
  fetchBtn_->setEnabled(false);
  table_->setRowCount(0);
  const int day = dayBox_->currentData().toInt();

  bridge_.requestStream(
      proto::historyRead(day),
      [](const proto::Bytes& v) { return proto::decodeHistorySlot(v).has_value(); },
      [](const proto::Bytes& v) {
        auto s = proto::decodeHistorySlot(v);
        return s && s->index >= s->count;
      },
      [this](std::vector<proto::Bytes> frames) {
        fetchBtn_->setEnabled(connected_);
        if (frames.empty()) {
          status_->setText(tr("No history data (or transfer failed)."));
          return;
        }
        int row = 0;
        table_->setRowCount(int(frames.size()));
        for (const auto& fr : frames) {
          auto s = proto::decodeHistorySlot(fr);
          if (!s) continue;
          table_->setItem(row, 0, new QTableWidgetItem(
              QStringLiteral("%1:%2").arg(s->hour, 2, 10, QChar('0')).arg(s->minute, 2, 10, QChar('0'))));
          table_->setItem(row, 1, new QTableWidgetItem(QString::number(s->steps)));
          table_->setItem(row, 2, new QTableWidgetItem(QString::number(s->distanceMeters)));
          table_->setItem(row, 3, new QTableWidgetItem(QString::number(s->calories / 10.0, 'f', 1)));
          table_->setItem(row, 4, new QTableWidgetItem(QString::number(s->activity)));
          table_->setItem(row, 5, new QTableWidgetItem(QString::number(s->heartRate)));
          ++row;
        }
        table_->setRowCount(row);
        table_->resizeColumnsToContents();
        table_->horizontalHeader()->setStretchLastSection(true);
        status_->setText(tr("%1 slot(s).").arg(row));
      },
      30000);
}
