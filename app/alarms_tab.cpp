#include "alarms_tab.hpp"

#include <QAbstractItemView>
#include <array>
#include <ctime>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include "ui_icons.hpp"
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
        .arg(a.year, 4, 10, QChar('0'))
        .arg(a.month, 2, 10, QChar('0'))
        .arg(a.day, 2, 10, QChar('0'));
  if (a.days == 0x7F) return QObject::tr("daily");
  QStringList parts;
  for (size_t i = 0; i < proto::kAlarmDays.size(); ++i) {
    if (a.days & (1u << i))
      parts << QString::fromUtf8(proto::kAlarmDays[i].data(), int(proto::kAlarmDays[i].size()));
  }
  return parts.join(QLatin1Char(','));
}

// Shared add/edit dialog. Returns nullopt if cancelled.
// For add: pass a template alarm (id/time defaults). For edit: pass the existing alarm.
std::optional<proto::Alarm> alarmDialog(QWidget* parent, const proto::Alarm& initial, bool isNew) {
  QDialog dlg(parent);
  dlg.setWindowTitle(isNew ? QObject::tr("Add alarm") : QObject::tr("Edit alarm"));
  auto* form = new QFormLayout(&dlg);

  QSpinBox* idSpin = nullptr;
  if (isNew) {
    idSpin = new QSpinBox(&dlg);
    idSpin->setRange(1, 255);
    idSpin->setValue(initial.id);
    form->addRow(QObject::tr("ID"), idSpin);
  }

  auto* timeEdit = new QTimeEdit(QTime(initial.hour, initial.minute), &dlg);
  timeEdit->setDisplayFormat(QStringLiteral("HH:mm"));
  form->addRow(QObject::tr("Time"), timeEdit);

  auto* enabled = new QCheckBox(QObject::tr("Enabled"), &dlg);
  enabled->setChecked(initial.enabled);
  form->addRow(enabled);

  // Weekday checkboxes (mon..sun). Bit mask matches protocol kAlarmDays.
  auto* daysHost = new QWidget(&dlg);
  auto* daysLay = new QHBoxLayout(daysHost);
  daysLay->setContentsMargins(0, 0, 0, 0);
  std::array<QCheckBox*, 7> dayCbs{};
  const uint8_t seedDays = (initial.days == 0) ? 0x7F : initial.days;
  for (size_t i = 0; i < proto::kAlarmDays.size(); ++i) {
    const QString label =
        QString::fromUtf8(proto::kAlarmDays[i].data(), int(proto::kAlarmDays[i].size()));
    dayCbs[i] = new QCheckBox(label, daysHost);
    dayCbs[i]->setChecked(seedDays & (1u << i));
    daysLay->addWidget(dayCbs[i]);
  }
  daysLay->addStretch(1);
  form->addRow(QObject::tr("Days"), daysHost);

  auto* allBtn = new QPushButton(QObject::tr("All days"), &dlg);
  auto* noneBtn = new QPushButton(QObject::tr("Clear"), &dlg);
  setButtonIcon(noneBtn, QStringLiteral("clear"));
  auto* quick = new QHBoxLayout;
  quick->addWidget(allBtn);
  quick->addWidget(noneBtn);
  quick->addStretch(1);
  form->addRow(quick);
  QObject::connect(allBtn, &QPushButton::clicked, [&] {
    for (auto* cb : dayCbs) cb->setChecked(true);
  });
  QObject::connect(noneBtn, &QPushButton::clicked, [&] {
    for (auto* cb : dayCbs) cb->setChecked(false);
  });

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  form->addRow(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

  if (dlg.exec() != QDialog::Accepted) return std::nullopt;

  proto::Alarm a = initial;
  if (idSpin) a.id = idSpin->value();
  a.hour = timeEdit->time().hour();
  a.minute = timeEdit->time().minute();
  a.enabled = enabled->isChecked();
  a.days = 0;
  for (size_t i = 0; i < dayCbs.size(); ++i) {
    if (dayCbs[i]->isChecked()) a.days |= static_cast<uint8_t>(1u << i);
  }
  // No days selected → treat as once (today/tomorrow) like the CLI "once" helper.
  if (a.days == 0) {
    std::time_t t = std::time(nullptr);
    std::tm now{};
    localtime_r(&t, &now);
    if (a.hour * 60 + a.minute <= now.tm_hour * 60 + now.tm_min) t += 24 * 3600;
    localtime_r(&t, &now);
    a.year = now.tm_year + 1900;
    a.month = now.tm_mon + 1;
    a.day = now.tm_mday;
  }
  return a;
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
  setButtonIcon(refreshBtn_, QStringLiteral("refresh"));
  refreshBtn_->setToolTip(tr("Reload alarms from the watch"));
  addBtn_ = new QPushButton(tr("Add"), this);
  setButtonIcon(addBtn_, QStringLiteral("add"));
  addBtn_->setToolTip(tr("Create a new alarm"));
  editBtn_ = new QPushButton(tr("Edit"), this);
  setButtonIcon(editBtn_, QStringLiteral("edit"));
  deleteBtn_ = new QPushButton(tr("Delete"), this);
  setButtonIcon(deleteBtn_, QStringLiteral("delete"));
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
  table_->setAlternatingRowColors(true);
  table_->setShowGrid(false);
  table_->verticalHeader()->setVisible(false);
  connect(table_, &QTableWidget::doubleClicked, this, [this](const QModelIndex&) { editAlarm(); });
  lay->addWidget(table_, 1);
}

void AlarmsTab::setConnected(bool connected) {
  connected_ = connected;
  needRefresh_ = connected;
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

void AlarmsTab::onShown() {
  if (connected_ && needRefresh_) {
    needRefresh_ = false;
    refresh();
  }
}

void AlarmsTab::fillTable(const std::vector<proto::Alarm>& alarms) {
  alarms_ = alarms;
  table_->setRowCount(int(alarms.size()));
  for (int i = 0; i < int(alarms.size()); ++i) {
    const auto& a = alarms[size_t(i)];
    table_->setItem(i, 0, new QTableWidgetItem(QString::number(a.id)));
    table_->setItem(i, 1,
                    new QTableWidgetItem(QStringLiteral("%1:%2")
                                             .arg(a.hour, 2, 10, QChar('0'))
                                             .arg(a.minute, 2, 10, QChar('0'))));
    table_->setItem(i, 2, new QTableWidgetItem(daysText(a)));
    table_->setItem(i, 3, new QTableWidgetItem(a.enabled ? tr("yes") : tr("no")));
  }
  table_->resizeColumnsToContents();
  table_->horizontalHeader()->setStretchLastSection(true);
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
  proto::Alarm seed;
  seed.id = alarms_.empty() ? 1 : alarms_.back().id + 1;
  const QTime now = QTime::currentTime();
  seed.hour = now.hour();
  seed.minute = now.minute();
  seed.enabled = true;
  seed.days = 0x7F;

  auto a = alarmDialog(this, seed, true);
  if (!a) return;

  status_->setText(tr("Writing alarm..."));
  bridge_.request(
      proto::alarmWrite(*a),
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
  auto a = alarmDialog(this, alarms_[size_t(row)], false);
  if (!a) return;

  status_->setText(tr("Writing alarm..."));
  bridge_.request(
      proto::alarmWrite(*a),
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
  bridge_.request(
      proto::alarmDelete(a),
      [](const proto::Bytes& v) { return proto::decodeAlarmFrame(v).has_value(); },
      [this](std::optional<proto::Bytes> v) {
        status_->setText(v ? tr("Alarm deleted.") : tr("Failed to delete alarm."));
        if (v) refresh();
      });
}
