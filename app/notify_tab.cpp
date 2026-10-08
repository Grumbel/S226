#include "notify_tab.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "watch_bridge.hpp"

namespace proto = s226::protocol;

NotifyTab::NotifyTab(WatchBridge& bridge, QWidget* parent)
    : QWidget(parent), bridge_(bridge) {
  buildUi();
  setConnected(false);
  connect(&bridge_, &WatchBridge::musicControl, this, &NotifyTab::onMusicControl);
}

void NotifyTab::buildUi() {
  auto* lay = new QVBoxLayout(this);

  auto* msgBox = new QGroupBox(tr("Message"), this);
  auto* msgRow = new QHBoxLayout(msgBox);
  messageEdit_ = new QLineEdit(msgBox);
  messageEdit_->setPlaceholderText(tr("Message text"));
  messageEdit_->setClearButtonEnabled(true);
  connect(messageEdit_, &QLineEdit::returnPressed, this, &NotifyTab::sendMessage);
  messageType_ = new QComboBox(msgBox);
  for (size_t i = 0; i < proto::kMessageTypeNames.size(); ++i)
    messageType_->addItem(
        QString::fromUtf8(proto::kMessageTypeNames[i].data(), int(proto::kMessageTypeNames[i].size())),
        int(i));
  messageType_->setCurrentIndex(int(proto::MessageType::Other));
  sendBtn_ = new QPushButton(tr("Send message"), msgBox);
  sendBtn_->setToolTip(tr("Show this text on the watch (type must be enabled in Settings)"));
  connect(sendBtn_, &QPushButton::clicked, this, &NotifyTab::sendMessage);
  msgRow->addWidget(messageEdit_, 1);
  msgRow->addWidget(messageType_);
  msgRow->addWidget(sendBtn_);
  lay->addWidget(msgBox);

  auto* callBox = new QGroupBox(tr("Incoming call"), this);
  auto* callRow = new QHBoxLayout(callBox);
  callName_ = new QLineEdit(callBox);
  callName_->setPlaceholderText(tr("Caller name"));
  callName_->setClearButtonEnabled(true);
  connect(callName_, &QLineEdit::returnPressed, this, &NotifyTab::sendCall);
  callBtn_ = new QPushButton(tr("Ring"), callBox);
  callBtn_->setToolTip(tr("Show the incoming-call screen with this name"));
  endCallBtn_ = new QPushButton(tr("End call"), callBox);
  endCallBtn_->setToolTip(tr("Dismiss the call screen on the watch"));
  connect(callBtn_, &QPushButton::clicked, this, &NotifyTab::sendCall);
  connect(endCallBtn_, &QPushButton::clicked, this, &NotifyTab::endCall);
  callRow->addWidget(callName_, 1);
  callRow->addWidget(callBtn_);
  callRow->addWidget(endCallBtn_);
  lay->addWidget(callBox);

  auto* musicBox = new QGroupBox(tr("Music (now playing)"), this);
  auto* form = new QFormLayout(musicBox);
  musicTitle_ = new QLineEdit(musicBox);
  musicTitle_->setPlaceholderText(tr("Title"));
  musicArtist_ = new QLineEdit(musicBox);
  musicArtist_->setPlaceholderText(tr("Artist"));
  musicAlbum_ = new QLineEdit(musicBox);
  musicAlbum_->setPlaceholderText(tr("Album"));
  musicPlaying_ = new QCheckBox(tr("Playing"), musicBox);
  musicPlaying_->setChecked(true);
  musicVolume_ = new QSpinBox(musicBox);
  musicVolume_->setRange(0, 100);
  musicVolume_->setValue(50);
  musicVolume_->setSuffix(tr("%"));
  form->addRow(tr("Title"), musicTitle_);
  form->addRow(tr("Artist"), musicArtist_);
  form->addRow(tr("Album"), musicAlbum_);
  form->addRow(musicPlaying_);
  form->addRow(tr("Volume"), musicVolume_);
  auto* musicRow = new QHBoxLayout;
  musicPushBtn_ = new QPushButton(tr("Push to watch"), musicBox);
  musicPushBtn_->setToolTip(
      tr("Send now-playing metadata so the watch can show the track "
         "(enable the music feature in Settings if needed)"));
  connect(musicPushBtn_, &QPushButton::clicked, this, &NotifyTab::pushNowPlaying);
  musicLastAction_ = new QLabel(tr("Watch keys: —"), musicBox);
  musicLastAction_->setToolTip(tr("Last media key received from the watch"));
  musicRow->addWidget(musicPushBtn_);
  musicRow->addWidget(musicLastAction_, 1);
  form->addRow(musicRow);
  lay->addWidget(musicBox);

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
  musicPushBtn_->setEnabled(connected);
  if (!connected) {
    status_->setText(tr("Connect to a watch to send notifications."));
    musicLastAction_->setText(tr("Watch keys: —"));
  }
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
  bridge_.request(
      proto::callAlert(), [](const proto::Bytes& v) { return !v.empty() && v[0] == 0xC1; },
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

void NotifyTab::pushNowPlaying() {
  if (!connected_) return;
  proto::NowPlaying np;
  np.title = musicTitle_->text().trimmed().toStdString();
  np.artist = musicArtist_->text().trimmed().toStdString();
  np.album = musicAlbum_->text().trimmed().toStdString();
  np.playing = musicPlaying_->isChecked();
  np.volume = musicVolume_->value();
  if (np.title.empty() && np.artist.empty() && np.album.empty()) {
    status_->setText(tr("Enter at least a title, artist or album."));
    return;
  }
  const auto packets = proto::nowPlayingPackets(np);
  for (const auto& p : packets) bridge_.send(p);
  status_->setText(tr("Now-playing pushed (%1 packet(s)).").arg(packets.size()));
}

void NotifyTab::onMusicControl(proto::MusicAction action) {
  musicLastAction_->setText(tr("Watch keys: %1").arg(QString::fromUtf8(proto::toString(action))));
  status_->setText(tr("Watch media key: %1").arg(QString::fromUtf8(proto::toString(action))));
}
