#include "settings_tab.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimeEdit>
#include <QVBoxLayout>
#include <functional>
#include <memory>

#include "watch_bridge.hpp"

namespace proto = s226::protocol;

SettingsTab::SettingsTab(WatchBridge& bridge, QWidget* parent)
    : QWidget(parent), bridge_(bridge) {
  buildUi();
  setConnected(false);
}

void SettingsTab::buildUi() {
  auto* outer = new QVBoxLayout(this);

  auto* btnRow = new QHBoxLayout;
  refreshBtn_ = new QPushButton(tr("Refresh"), this);
  applyBtn_ = new QPushButton(tr("Apply"), this);
  status_ = new QLabel(this);
  status_->setWordWrap(true);
  connect(refreshBtn_, &QPushButton::clicked, this, &SettingsTab::refresh);
  connect(applyBtn_, &QPushButton::clicked, this, &SettingsTab::apply);
  btnRow->addWidget(refreshBtn_);
  btnRow->addWidget(applyBtn_);
  btnRow->addWidget(status_, 1);
  outer->addLayout(btnRow);

  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  auto* formHost = new QWidget;
  auto* form = new QVBoxLayout(formHost);

  {
    auto* g = new QGroupBox(tr("Sedentary reminder"), formHost);
    auto* f = new QFormLayout(g);
    sedentaryOn_ = new QCheckBox(tr("Enabled"), g);
    sedentaryStart_ = new QTimeEdit(QTime(8, 0), g);
    sedentaryStart_->setDisplayFormat(QStringLiteral("HH:mm"));
    sedentaryEnd_ = new QTimeEdit(QTime(18, 0), g);
    sedentaryEnd_->setDisplayFormat(QStringLiteral("HH:mm"));
    sedentaryInterval_ = new QSpinBox(g);
    sedentaryInterval_->setRange(1, 255);
    sedentaryInterval_->setSuffix(tr(" min"));
    f->addRow(sedentaryOn_);
    f->addRow(tr("From"), sedentaryStart_);
    f->addRow(tr("To"), sedentaryEnd_);
    f->addRow(tr("Interval"), sedentaryInterval_);
    form->addWidget(g);
  }
  {
    auto* g = new QGroupBox(tr("Heart-rate alarm"), formHost);
    auto* f = new QFormLayout(g);
    hrAlarmOn_ = new QCheckBox(tr("Enabled"), g);
    hrAlarmLow_ = new QSpinBox(g);
    hrAlarmLow_->setRange(30, 200);
    hrAlarmLow_->setSuffix(tr(" bpm"));
    hrAlarmHigh_ = new QSpinBox(g);
    hrAlarmHigh_->setRange(30, 220);
    hrAlarmHigh_->setSuffix(tr(" bpm"));
    f->addRow(hrAlarmOn_);
    f->addRow(tr("Low"), hrAlarmLow_);
    f->addRow(tr("High"), hrAlarmHigh_);
    form->addWidget(g);
  }
  {
    auto* g = new QGroupBox(tr("Screen on time"), formHost);
    auto* f = new QFormLayout(g);
    screenOn_ = new QSpinBox(g);
    screenOn_->setRange(1, 255);
    screenOn_->setSuffix(tr(" s"));
    screenOnRange_ = new QLabel(g);
    f->addRow(tr("Seconds"), screenOn_);
    f->addRow(tr("Watch range"), screenOnRange_);
    form->addWidget(g);
  }
  {
    auto* g = new QGroupBox(tr("Brightness"), formHost);
    auto* f = new QFormLayout(g);
    brightnessMode_ = new QComboBox(g);
    brightnessMode_->addItem(tr("Manual"), 0);
    brightnessMode_->addItem(tr("Automatic"), 1);
    brightnessLevel_ = new QSpinBox(g);
    brightnessLevel_->setRange(1, 5);
    brightnessOther_ = new QSpinBox(g);
    brightnessOther_->setRange(1, 5);
    brightnessStart_ = new QTimeEdit(QTime(22, 0), g);
    brightnessStart_->setDisplayFormat(QStringLiteral("HH:mm"));
    brightnessEnd_ = new QTimeEdit(QTime(8, 0), g);
    brightnessEnd_->setDisplayFormat(QStringLiteral("HH:mm"));
    brightnessMax_ = new QLabel(g);
    f->addRow(tr("Mode"), brightnessMode_);
    f->addRow(tr("Night level"), brightnessLevel_);
    f->addRow(tr("Day level"), brightnessOther_);
    f->addRow(tr("Night from"), brightnessStart_);
    f->addRow(tr("Night to"), brightnessEnd_);
    f->addRow(tr("Max level"), brightnessMax_);
    form->addWidget(g);
  }
  {
    auto* g = new QGroupBox(tr("Countdown preset"), formHost);
    auto* f = new QFormLayout(g);
    countdownSec_ = new QSpinBox(g);
    countdownSec_->setRange(0, 24 * 3600);
    countdownSec_->setSuffix(tr(" s"));
    countdownShow_ = new QCheckBox(tr("Show on watch"), g);
    f->addRow(tr("Seconds"), countdownSec_);
    f->addRow(countdownShow_);
    form->addWidget(g);
  }
  {
    auto* g = new QGroupBox(tr("Watch face"), formHost);
    auto* f = new QFormLayout(g);
    watchFace_ = new QSpinBox(g);
    watchFace_->setRange(0, 20);
    f->addRow(tr("Style"), watchFace_);
    form->addWidget(g);
  }
  {
    auto* g = new QGroupBox(tr("Personal data"), formHost);
    auto* f = new QFormLayout(g);
    personHeight_ = new QSpinBox(g);
    personHeight_->setRange(50, 250);
    personHeight_->setSuffix(tr(" cm"));
    personHeight_->setValue(170);
    personWeight_ = new QSpinBox(g);
    personWeight_->setRange(20, 200);
    personWeight_->setSuffix(tr(" kg"));
    personWeight_->setValue(70);
    personAge_ = new QSpinBox(g);
    personAge_->setRange(1, 120);
    personAge_->setValue(30);
    personSex_ = new QComboBox(g);
    personSex_->addItem(tr("Male"), true);
    personSex_->addItem(tr("Female"), false);
    personStepGoal_ = new QSpinBox(g);
    personStepGoal_->setRange(1000, 50000);
    personStepGoal_->setSingleStep(500);
    personStepGoal_->setValue(8000);
    personSleepGoal_ = new QSpinBox(g);
    personSleepGoal_->setRange(60, 24 * 60);
    personSleepGoal_->setSuffix(tr(" min"));
    personSleepGoal_->setValue(480);
    f->addRow(tr("Height"), personHeight_);
    f->addRow(tr("Weight"), personWeight_);
    f->addRow(tr("Age"), personAge_);
    f->addRow(tr("Sex"), personSex_);
    f->addRow(tr("Step goal"), personStepGoal_);
    f->addRow(tr("Sleep goal"), personSleepGoal_);
    form->addWidget(g);
  }
  {
    auto* g = new QGroupBox(tr("Features"), formHost);
    auto* lay = new QVBoxLayout(g);
    featureChecks_.clear();
    for (const auto& feat : proto::kWatchFeatures) {
      auto* cb = new QCheckBox(QString::fromUtf8(feat.name.data(), int(feat.name.size())), g);
      cb->setProperty("featureOffset", int(feat.offset));
      lay->addWidget(cb);
      featureChecks_.push_back(cb);
    }
    form->addWidget(g);
  }
  {
    auto* g = new QGroupBox(tr("Message types shown on the watch"), formHost);
    auto* lay = new QVBoxLayout(g);
    messageChecks_.clear();
    for (size_t i = 0; i < proto::kMessageTypeNames.size(); ++i) {
      auto* cb = new QCheckBox(
          QString::fromUtf8(proto::kMessageTypeNames[i].data(), int(proto::kMessageTypeNames[i].size())), g);
      cb->setProperty("messageIndex", int(i));
      lay->addWidget(cb);
      messageChecks_.push_back(cb);
    }
    form->addWidget(g);
  }

  form->addStretch(1);
  scroll->setWidget(formHost);
  outer->addWidget(scroll, 1);
}

void SettingsTab::setConnected(bool connected) {
  connected_ = connected;
  refreshBtn_->setEnabled(connected && !busy_);
  applyBtn_->setEnabled(connected && !busy_);
  if (!connected) status_->setText(tr("Connect to a watch to load or change settings."));
}

void SettingsTab::setBusy(bool busy) {
  busy_ = busy;
  refreshBtn_->setEnabled(connected_ && !busy_);
  applyBtn_->setEnabled(connected_ && !busy_);
}

void SettingsTab::refresh() {
  if (!connected_ || busy_) return;
  setBusy(true);
  status_->setText(tr("Loading settings..."));

  auto next = std::make_shared<std::function<void()>>();
  auto step = std::make_shared<int>(0);

  *next = [this, next, step]() {
    const int s = (*step)++;
    if (s == 0) {
      bridge_.request(proto::sedentaryRead(),
          [](const proto::Bytes& v) { return proto::decodeSedentary(v).has_value(); },
          [this, next](std::optional<proto::Bytes> v) {
            if (v) if (auto r = proto::decodeSedentary(*v)) {
              sedentaryOn_->setChecked(r->enabled);
              sedentaryStart_->setTime(QTime(r->startHour, r->startMinute));
              sedentaryEnd_->setTime(QTime(r->endHour, r->endMinute));
              sedentaryInterval_->setValue(r->intervalMinutes);
            }
            (*next)();
          });
    } else if (s == 1) {
      bridge_.request(proto::heartRateAlarmRead(),
          [](const proto::Bytes& v) { return proto::decodeHeartRateAlarm(v).has_value(); },
          [this, next](std::optional<proto::Bytes> v) {
            if (v) if (auto a = proto::decodeHeartRateAlarm(*v)) {
              hrAlarmOn_->setChecked(a->enabled);
              hrAlarmLow_->setValue(a->low);
              hrAlarmHigh_->setValue(a->high);
            }
            (*next)();
          });
    } else if (s == 2) {
      bridge_.request(proto::screenOnTimeRead(),
          [](const proto::Bytes& v) { return proto::decodeScreenOnTime(v).has_value(); },
          [this, next](std::optional<proto::Bytes> v) {
            if (v) if (auto t = proto::decodeScreenOnTime(*v)) {
              screenOn_->setValue(t->seconds);
              screenOnRange_->setText(tr("%1–%2 s").arg(t->minSeconds).arg(t->maxSeconds));
              if (t->maxSeconds > 0)
                screenOn_->setRange(std::max(1, t->minSeconds), std::max(t->minSeconds, t->maxSeconds));
            }
            (*next)();
          });
    } else if (s == 3) {
      bridge_.request(proto::brightnessRead(),
          [](const proto::Bytes& v) { return proto::decodeBrightness(v).has_value(); },
          [this, next](std::optional<proto::Bytes> v) {
            if (v) if (auto b = proto::decodeBrightness(*v)) {
              brightnessMode_->setCurrentIndex(b->automatic ? 1 : 0);
              brightnessLevel_->setValue(b->level);
              brightnessOther_->setValue(b->otherLevel);
              brightnessStart_->setTime(QTime(b->startHour, b->startMinute));
              brightnessEnd_->setTime(QTime(b->endHour, b->endMinute));
              brightnessMax_->setText(b->maxLevel > 0 ? QString::number(b->maxLevel) : tr("—"));
              if (b->maxLevel > 0) {
                brightnessLevel_->setRange(1, b->maxLevel);
                brightnessOther_->setRange(1, b->maxLevel);
              }
            }
            (*next)();
          });
    } else if (s == 4) {
      bridge_.request(proto::countdownRead(),
          [](const proto::Bytes& v) { return proto::decodeCountdown(v).has_value(); },
          [this, next](std::optional<proto::Bytes> v) {
            if (v) if (auto c = proto::decodeCountdown(*v)) {
              countdownSec_->setValue(c->seconds);
              countdownShow_->setChecked(c->showOnWatch);
            }
            (*next)();
          });
    } else if (s == 5) {
      bridge_.request(proto::watchFaceRead(),
          [](const proto::Bytes& v) { return proto::decodeWatchFace(v).has_value(); },
          [this, next](std::optional<proto::Bytes> v) {
            if (v) if (auto style = proto::decodeWatchFace(*v)) watchFace_->setValue(*style);
            (*next)();
          });
    } else if (s == 6) {
      bridge_.request(proto::watchFeaturesRead(),
          [](const proto::Bytes& v) { return proto::decodeWatchFeatures(v).has_value(); },
          [this, next](std::optional<proto::Bytes> v) {
            if (v) if (auto f = proto::decodeWatchFeatures(*v)) {
              for (auto* cb : featureChecks_) {
                const int off = cb->property("featureOffset").toInt();
                const uint8_t st = f->raw[static_cast<size_t>(off)];
                cb->setEnabled(st != proto::WatchFeatures::Unsupported);
                cb->setChecked(st == proto::WatchFeatures::On);
              }
            }
            (*next)();
          });
    } else if (s == 7) {
      bridge_.request(proto::messageSwitchesRead(),
          [](const proto::Bytes& v) { return proto::decodeMessageSwitches(v).has_value(); },
          [this, next](std::optional<proto::Bytes> v) {
            if (v) if (auto m = proto::decodeMessageSwitches(*v)) {
              for (auto* cb : messageChecks_) {
                const int idx = cb->property("messageIndex").toInt();
                const uint8_t st = m->state[static_cast<size_t>(idx)];
                const bool unsupported = idx >= 2 && st == proto::MessageSwitches::Unsupported;
                cb->setEnabled(!unsupported);
                cb->setChecked(st == proto::MessageSwitches::On ||
                               (idx < 2 && st != proto::MessageSwitches::Off));
              }
            }
            (*next)();
          });
    } else {
      setBusy(false);
      status_->setText(tr("Settings loaded."));
    }
  };
  (*next)();
}

void SettingsTab::apply() {
  if (!connected_ || busy_) return;
  setBusy(true);
  status_->setText(tr("Applying settings..."));

  auto next = std::make_shared<std::function<void()>>();
  auto step = std::make_shared<int>(0);
  auto ok = std::make_shared<bool>(true);

  *next = [this, next, step, ok]() {
    const int s = (*step)++;
    if (s == 0) {
      proto::SedentaryReminder r;
      r.enabled = sedentaryOn_->isChecked();
      r.startHour = sedentaryStart_->time().hour();
      r.startMinute = sedentaryStart_->time().minute();
      r.endHour = sedentaryEnd_->time().hour();
      r.endMinute = sedentaryEnd_->time().minute();
      r.intervalMinutes = sedentaryInterval_->value();
      bridge_.request(proto::sedentaryWrite(r),
          [](const proto::Bytes& v) { return proto::decodeSedentary(v).has_value(); },
          [next, ok](std::optional<proto::Bytes> v) { if (!v) *ok = false; (*next)(); });
    } else if (s == 1) {
      proto::HeartRateAlarm a;
      a.enabled = hrAlarmOn_->isChecked();
      a.low = hrAlarmLow_->value();
      a.high = hrAlarmHigh_->value();
      bridge_.request(proto::heartRateAlarmWrite(a),
          [](const proto::Bytes& v) { return proto::decodeHeartRateAlarm(v).has_value(); },
          [next, ok](std::optional<proto::Bytes> v) { if (!v) *ok = false; (*next)(); });
    } else if (s == 2) {
      bridge_.request(proto::screenOnTimeWrite(screenOn_->value()),
          [](const proto::Bytes& v) { return proto::decodeScreenOnTime(v).has_value(); },
          [next, ok](std::optional<proto::Bytes> v) { if (!v) *ok = false; (*next)(); });
    } else if (s == 3) {
      proto::Brightness b;
      if (brightnessMode_->currentData().toInt() == 1) {
        b.automatic = true;
        b.level = brightnessLevel_->value();
        b.otherLevel = brightnessOther_->value();
        b.startHour = brightnessStart_->time().hour();
        b.startMinute = brightnessStart_->time().minute();
        b.endHour = brightnessEnd_->time().hour();
        b.endMinute = brightnessEnd_->time().minute();
      } else {
        b = proto::manualBrightness(brightnessOther_->value());
      }
      bridge_.request(proto::brightnessWrite(b),
          [](const proto::Bytes& v) { return proto::decodeBrightness(v).has_value(); },
          [next, ok](std::optional<proto::Bytes> v) { if (!v) *ok = false; (*next)(); });
    } else if (s == 4) {
      bridge_.request(proto::countdownWrite(countdownSec_->value(), countdownShow_->isChecked()),
          [](const proto::Bytes& v) { return proto::decodeCountdown(v).has_value(); },
          [next, ok](std::optional<proto::Bytes> v) { if (!v) *ok = false; (*next)(); });
    } else if (s == 5) {
      bridge_.request(proto::watchFaceWrite(watchFace_->value()),
          [](const proto::Bytes& v) { return proto::decodeWatchFace(v).has_value(); },
          [next, ok](std::optional<proto::Bytes> v) { if (!v) *ok = false; (*next)(); });
    } else if (s == 6) {
      bridge_.request(proto::watchFeaturesRead(),
          [](const proto::Bytes& v) { return proto::decodeWatchFeatures(v).has_value(); },
          [this, next, ok](std::optional<proto::Bytes> v) {
            if (!v) { *ok = false; (*next)(); return; }
            auto f = *proto::decodeWatchFeatures(*v);
            for (auto* cb : featureChecks_) {
              const int off = cb->property("featureOffset").toInt();
              if (f.raw[static_cast<size_t>(off)] == proto::WatchFeatures::Unsupported) continue;
              f.raw[static_cast<size_t>(off)] =
                  cb->isChecked() ? proto::WatchFeatures::On : proto::WatchFeatures::Off;
            }
            bridge_.request(proto::watchFeaturesWrite(f),
                [](const proto::Bytes& v) { return proto::decodeWatchFeatures(v).has_value(); },
                [next, ok](std::optional<proto::Bytes> v2) { if (!v2) *ok = false; (*next)(); });
          });
    } else if (s == 7) {
      bridge_.request(proto::messageSwitchesRead(),
          [](const proto::Bytes& v) { return proto::decodeMessageSwitches(v).has_value(); },
          [this, next, ok](std::optional<proto::Bytes> v) {
            if (!v) { *ok = false; (*next)(); return; }
            auto m = *proto::decodeMessageSwitches(*v);
            for (auto* cb : messageChecks_) {
              const int idx = cb->property("messageIndex").toInt();
              if (idx >= 2 && m.state[static_cast<size_t>(idx)] == proto::MessageSwitches::Unsupported)
                continue;
              m.state[static_cast<size_t>(idx)] =
                  cb->isChecked() ? proto::MessageSwitches::On : proto::MessageSwitches::Off;
            }
            bridge_.request(proto::messageSwitchesWrite(m),
                [](const proto::Bytes& v) { return proto::decodeMessageSwitches(v).has_value(); },
                [next, ok](std::optional<proto::Bytes> v2) { if (!v2) *ok = false; (*next)(); });
          });
    } else if (s == 8) {
      proto::PersonInfo p;
      p.heightCm = personHeight_->value();
      p.weightKg = personWeight_->value();
      p.age = personAge_->value();
      p.male = personSex_->currentData().toBool();
      p.stepGoal = personStepGoal_->value();
      p.sleepGoalMinutes = personSleepGoal_->value();
      bridge_.request(proto::personInfoWrite(p),
          [](const proto::Bytes& v) { return proto::isPersonInfoAck(v); },
          [next, ok](std::optional<proto::Bytes> v) { if (!v) *ok = false; (*next)(); });
    } else {
      setBusy(false);
      status_->setText(*ok ? tr("Settings applied.") : tr("Some settings could not be applied."));
    }
  };
  (*next)();
}
