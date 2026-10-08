#include "notify_tab.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "watch_bridge.hpp"

namespace proto = s226::protocol;

NotifyTab::NotifyTab(WatchBridge& bridge, QWidget* parent)
    : QWidget(parent), bridge_(bridge) {
  buildUi();
  setConnected(false);
}

void NotifyTab::buildUi() {
  auto* lay = new QVBoxLayout(this);

  auto* msgRow = new QHBoxLayout;
  messageEdit_ = new QLineEdit(this);
  messageEdit_->setPlaceholderText(tr("Message text"));
  messageType_ = new QComboBox(this);
  for (size_t i = 0; i < proto::kMessageTypeNames.size(); ++i)
    messageType_->addItem(
        QString::fromUtf8(proto::kMessageTypeNames[i].data(), int(proto::kMessageTypeNames[i].size())),
        int(i));
  messageType_->setCurrentIndex(int(proto::MessageType::Other));
  sendBtn_ = new QPushButton(tr("Send message"), this);
  connect(sendBtn_, &QPushButton::clicked, this, &NotifyTab::sendMessage);
  msgRow->addWidget(messageEdit_, 1);
  msgRow->addWidget(messageType_);
  msgRow->addWidget(sendBtn_);
  lay->addLayout(msgRow);

  auto* callRow = new QHBoxLayout;
  callName_ = new QLineEdit(this);
  callName_->setPlaceholderText(tr("Caller name"));
  callBtn_ = new QPushButton(tr("Incoming call"), this);
  endCallBtn_ = new QPushButton(tr("End call"), this);
  connect(callBtn_, &QPushButton::clicked, this, &NotifyTab::sendCall);
  connect(endCallBtn_, &QPushButton::clicked, this, &NotifyTab::endCall);
  callRow->addWidget(callName_, 1);
  callRow->addWidget(callBtn_);
  callRow->addWidget(endCallBtn_);
  lay->addLayout(callRow);

  status_ = new QLabel(this);
  status_->setWordWrap(true);
  lay->addWidget(status_);
  lay->addStretch(1);
}

void NotifyTab::setConnected(bool connected) {
  connected_ = connected;
  sendBtn_->setEnabled(connected);
  callBtn_->setEnabled(connected);
  endCallBtn_->setEnabled(connected);
  if (!connected) status_->setText(tr("Connect to a watch to send notifications."));
}

void NotifyTab::sendMessage() {
  if (!connected_) return;
  const QString text = messageEdit_->text().trimmed();
  if (text.isEmpty()) {
    status_->setText(tr("Enter a message."));
    return;
  }
  const auto type = static_cast<proto::MessageType>(messageType_->currentData().toInt());
  if (type == proto::MessageType::Sms) bridge_.send(proto::smsAlert());
  const auto packets = proto::messagePackets(text.toStdString(), type);
  for (const auto& p : packets) bridge_.send(p);
  status_->setText(tr("Message sent (%1 packet(s)).").arg(packets.size()));
}

void NotifyTab::sendCall() {
  if (!connected_) return;
  const QString name = callName_->text().trimmed();
  if (name.isEmpty()) {
    status_->setText(tr("Enter a caller name."));
    return;
  }
  status_->setText(tr("Ringing..."));
  bridge_.request(proto::callAlert(),
      [](const proto::Bytes& v) { return !v.empty() && v[0] == 0xC1; },
      [this, name](std::optional<proto::Bytes> v) {
        if (!v) {
          status_->setText(tr("Call alert failed."));
          return;
        }
        for (const auto& p : proto::callerPackets(name.toStdString())) bridge_.send(p);
        status_->setText(tr("Call screen shown (use End call to stop)."));
      });
}

void NotifyTab::endCall() {
  if (!connected_) return;
  bridge_.send(proto::callEnd());
  status_->setText(tr("Call ended."));
}
