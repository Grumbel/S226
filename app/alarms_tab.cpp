#include "alarms_tab.hpp"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimeEdit>
#include <QVBoxLayout>

#include "watch_bridge.hpp"

namespace proto = s226::protocol;

namespace {
QString daysText(const proto::Alarm& a) {
  if (a.days == 0)
    return QStringLiteral("%1-%2-%3")
        .arg(a.year, 4, 10, QChar('0')).arg(a.month, 2, 10, QChar('0')).arg(a.day, 2, 10, QChar('0'));
  if (a.days == 0x7F) return QObject::tr("daily");
  QStringList parts;
  for (size_t i = 0; i < proto::kAlarmDays.size(); ++i)
    if (a.days & (1u << i))
      parts << QString::fromUtf8(proto::kAlarmDays[i].data(), int(proto::kAlarmDays[i].size()));
  return parts.join(QLatin1Char(','));
}
} // namespace

AlarmsTab::AlarmsTab(WatchBridge& bridge, QWidget* parent)
    : QWidget(parent), bridge_(bridge) {
  buildUi();
  setConnected(false);
}

void AlarmsTab::buildUi() {
  auto* lay = new QVBoxLayout(this);
  auto* btn = new QHBoxLayout;
  refreshBtn_ = new QPushButton(tr("Refresh"), this);
  addBtn_ = new QPushButton(tr("Add"), this);
  editBtn_ = new QPushButton(tr("Edit"), this);
  deleteBtn_ = new QPushButton(tr("Delete"), this);
  status_ = new QLabel(this);
  connect(refreshBtn_, &QPushButton::clicked, this, &AlarmsTab::refresh);
  connect(addBtn_, &QPushButton::clicked, this, &AlarmsTab::addAlarm);
  connect(editBtn_, &QPushButton::clicked, this, &AlarmsTab::editAlarm);
  connect(deleteBtn_, &QPushButton::clicked, this, &AlarmsTab::deleteAlarm);
  btn->addWidget(refreshBtn_);
  btn->addWidget(addBtn_);
  btn->addWidget(editBtn_);
  btn->addWidget(deleteBtn_);
  btn->addWidget(status_, 1);
  lay->addLayout(btn);

  table_ = new QTableWidget(0, 4, this);
  table_->setHorizontalHeaderLabels({tr("ID"), tr("Time"), tr("Days"), tr("Enabled")});
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  lay->addWidget(table_, 1);
}

void AlarmsTab::setConnected(bool connected) {
  connected_ = connected;
  refreshBtn_->setEnabled(connected);
  addBtn_->setEnabled(connected);
  editBtn_->setEnabled(connected);
  deleteBtn_->setEnabled(connected);
  if (!connected) {
    table_->setRowCount(0);
    alarms_.clear();
    status_->setText(tr("Connect to a watch to manage alarms."));
  }
}

void AlarmsTab::fillTable(const std::vector<proto::Alarm>& alarms) {
  alarms_ = alarms;
  table_->setRowCount(int(alarms.size()));
  for (int i = 0; i < int(alarms.size()); ++i) {
    const auto& a = alarms[size_t(i)];
    table_->setItem(i, 0, new QTableWidgetItem(QString::number(a.id)));
    table_->setItem(i, 1, new QTableWidgetItem(
        QStringLiteral("%1:%2").arg(a.hour, 2, 10, QChar('0')).arg(a.minute, 2, 10, QChar('0'))));
    table_->setItem(i, 2, new QTableWidgetItem(daysText(a)));
    table_->setItem(i, 3, new QTableWidgetItem(a.enabled ? tr("yes") : tr("no")));
  }
}

void AlarmsTab::refresh() {
  if (!connected_) return;
  status_->setText(tr("Loading alarms..."));
  refreshBtn_->setEnabled(false);
  bridge_.requestStream(
      proto::alarmsRead(),
      [](const proto::Bytes& v) { return proto::decodeAlarmFrame(v).has_value(); },
      [](const proto::Bytes& v) {
        auto f = proto::decodeAlarmFrame(v);
        return f && f->index == 0;
      },
      [this](std::vector<proto::Bytes> frames) {
        refreshBtn_->setEnabled(connected_);
        std::vector<proto::Alarm> list;
        for (const auto& fr : frames) {
          auto f = proto::decodeAlarmFrame(fr);
          if (f && f->index > 0) list.push_back(f->alarm);
        }
        fillTable(list);
        status_->setText(list.empty() ? tr("No alarms stored.")
                                      : tr("%1 alarm(s).").arg(list.size()));
      },
      5000);
}

void AlarmsTab::addAlarm() {
  QDialog dlg(this);
  dlg.setWindowTitle(tr("Add alarm"));
  auto* form = new QFormLayout(&dlg);
  auto* idSpin = new QSpinBox(&dlg);
  idSpin->setRange(1, 255);
  idSpin->setValue(alarms_.empty() ? 1 : alarms_.back().id + 1);
  auto* timeEdit = new QTimeEdit(QTime::currentTime(), &dlg);
  timeEdit->setDisplayFormat(QStringLiteral("HH:mm"));
  auto* enabled = new QCheckBox(tr("Enabled"), &dlg);
  enabled->setChecked(true);
  form->addRow(tr("ID"), idSpin);
  form->addRow(tr("Time"), timeEdit);
  form->addRow(enabled);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  form->addRow(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  if (dlg.exec() != QDialog::Accepted) return;

  proto::Alarm a;
  a.id = idSpin->value();
  a.hour = timeEdit->time().hour();
  a.minute = timeEdit->time().minute();
  a.enabled = enabled->isChecked();
  a.days = 0x7F;

  status_->setText(tr("Writing alarm..."));
  bridge_.request(proto::alarmWrite(a),
      [](const proto::Bytes& v) { return proto::decodeAlarmFrame(v).has_value(); },
      [this](std::optional<proto::Bytes> v) {
        status_->setText(v ? tr("Alarm written.") : tr("Failed to write alarm."));
        if (v) refresh();
      });
}

void AlarmsTab::editAlarm() {
  const int row = table_->currentRow();
  if (row < 0 || row >= int(alarms_.size())) {
    QMessageBox::information(this, tr("Alarms"), tr("Select an alarm first."));
    return;
  }
  proto::Alarm a = alarms_[size_t(row)];
  QDialog dlg(this);
  dlg.setWindowTitle(tr("Edit alarm"));
  auto* form = new QFormLayout(&dlg);
  auto* timeEdit = new QTimeEdit(QTime(a.hour, a.minute), &dlg);
  timeEdit->setDisplayFormat(QStringLiteral("HH:mm"));
  auto* enabled = new QCheckBox(tr("Enabled"), &dlg);
  enabled->setChecked(a.enabled);
  form->addRow(tr("Time"), timeEdit);
  form->addRow(enabled);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  form->addRow(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  if (dlg.exec() != QDialog::Accepted) return;
  a.hour = timeEdit->time().hour();
  a.minute = timeEdit->time().minute();
  a.enabled = enabled->isChecked();

  status_->setText(tr("Writing alarm..."));
  bridge_.request(proto::alarmWrite(a),
      [](const proto::Bytes& v) { return proto::decodeAlarmFrame(v).has_value(); },
      [this](std::optional<proto::Bytes> v) {
        status_->setText(v ? tr("Alarm updated.") : tr("Failed to write alarm."));
        if (v) refresh();
      });
}

void AlarmsTab::deleteAlarm() {
  const int row = table_->currentRow();
  if (row < 0 || row >= int(alarms_.size())) {
    QMessageBox::information(this, tr("Alarms"), tr("Select an alarm first."));
    return;
  }
  const proto::Alarm a = alarms_[size_t(row)];
  status_->setText(tr("Deleting alarm..."));
  bridge_.request(proto::alarmDelete(a),
      [](const proto::Bytes& v) { return proto::decodeAlarmFrame(v).has_value(); },
      [this](std::optional<proto::Bytes> v) {
        status_->setText(v ? tr("Alarm deleted.") : tr("Failed to delete alarm."));
        if (v) refresh();
      });
}
