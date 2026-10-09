#include "notify_tab.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include "ui_icons.hpp"
#include <QSpinBox>
#include <QHeaderView>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <functional>
#include <memory>
#include <algorithm>
#include <QVBoxLayout>

#include "watch_bridge.hpp"
#include "mpris_controller.hpp"
#include "notification_forwarder.hpp"

namespace proto = s226::protocol;

NotifyTab::NotifyTab(WatchBridge& bridge, MprisController* mpris,
                     NotificationForwarder* desktopNotify, QWidget* parent)
    : QWidget(parent), bridge_(bridge), mpris_(mpris), desktopNotify_(desktopNotify) {
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
  setButtonIcon(sendBtn_, QStringLiteral("send"));
  sendBtn_->setToolTip(tr("Show this text on the watch (type must be enabled in Settings)"));
  connect(sendBtn_, &QPushButton::clicked, this, &NotifyTab::sendMessage);
  msgRow->addWidget(messageEdit_, 1);
  msgRow->addWidget(messageType_);
  msgRow->addWidget(sendBtn_);
  lay->addWidget(msgBox);

  auto* deskBox = new QGroupBox(tr("Desktop notifications"), this);
  auto* deskLay = new QVBoxLayout(deskBox);
  desktopNotifyForward_ = new QCheckBox(tr("Forward to the watch"), deskBox);
  desktopNotifyForward_->setToolTip(
      tr("Listen for org.freedesktop.Notifications.Notify on the session bus "
         "and show matching ones on the watch as message type 'other'."));
  deskLay->addWidget(desktopNotifyForward_);

  auto* filterForm = new QFormLayout;
  notifyUrgency_ = new QComboBox(deskBox);
  notifyUrgency_->addItem(tr("All (including Low)"), 0);
  notifyUrgency_->addItem(tr("Normal and Critical"), 1);
  notifyUrgency_->addItem(tr("Critical only"), 2);
  notifyUrgency_->setToolTip(
      tr("Freedesktop urgency hint: 0=Low, 1=Normal, 2=Critical. "
         "Volume and brightness OSDs often use Low."));
  filterForm->addRow(tr("Minimum urgency"), notifyUrgency_);

  notifySkipSync_ = new QCheckBox(tr("Skip OSD / synchronous (volume, brightness)"), deskBox);
  notifySkipSync_->setToolTip(
      tr("Skip notifications with the x-canonical-private-synchronous hint "
         "(used by many volume and brightness overlays)."));
  filterForm->addRow(QString(), notifySkipSync_);

  notifySkipTransient_ = new QCheckBox(tr("Skip transient notifications"), deskBox);
  notifySkipTransient_->setToolTip(tr("Skip notifications marked transient in hints."));
  filterForm->addRow(QString(), notifySkipTransient_);

  notifyBlockedApps_ = new QLineEdit(deskBox);
  notifyBlockedApps_->setPlaceholderText(tr("e.g. volume,gnome-settings-daemon"));
  notifyBlockedApps_->setToolTip(
      tr("Comma-separated app-name substrings to block (case-insensitive)."));
  filterForm->addRow(tr("Blocked apps"), notifyBlockedApps_);

  notifyBlockedCats_ = new QLineEdit(deskBox);
  notifyBlockedCats_->setPlaceholderText(tr("e.g. device,transfer"));
  notifyBlockedCats_->setToolTip(
      tr("Comma-separated freedesktop categories to block "
         "(exact or prefix, e.g. device blocks device.added). "
         "Common: device, email, im, network, presence, transfer."));
  filterForm->addRow(tr("Blocked categories"), notifyBlockedCats_);
  deskLay->addLayout(filterForm);

  if (desktopNotify_) {
    desktopNotifyForward_->setChecked(desktopNotify_->isEnabled());
    connect(desktopNotifyForward_, &QCheckBox::toggled, desktopNotify_,
            &NotificationForwarder::setEnabled);
    connect(desktopNotify_, &NotificationForwarder::enabledChanged, desktopNotifyForward_,
            &QCheckBox::setChecked);

    const int urg = desktopNotify_->minUrgency();
    const int idx = notifyUrgency_->findData(urg);
    notifyUrgency_->setCurrentIndex(idx >= 0 ? idx : 1);
    connect(notifyUrgency_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
      if (desktopNotify_)
        desktopNotify_->setMinUrgency(notifyUrgency_->currentData().toInt());
    });

    notifySkipSync_->setChecked(desktopNotify_->skipSynchronous());
    connect(notifySkipSync_, &QCheckBox::toggled, desktopNotify_,
            &NotificationForwarder::setSkipSynchronous);
    notifySkipTransient_->setChecked(desktopNotify_->skipTransient());
    connect(notifySkipTransient_, &QCheckBox::toggled, desktopNotify_,
            &NotificationForwarder::setSkipTransient);

    notifyBlockedApps_->setText(desktopNotify_->blockedApps().join(QStringLiteral(", ")));
    connect(notifyBlockedApps_, &QLineEdit::editingFinished, this, [this]() {
      if (!desktopNotify_) return;
      desktopNotify_->setBlockedApps(
          notifyBlockedApps_->text().split(QLatin1Char(','), Qt::SkipEmptyParts));
    });
    notifyBlockedCats_->setText(desktopNotify_->blockedCategories().join(QStringLiteral(", ")));
    connect(notifyBlockedCats_, &QLineEdit::editingFinished, this, [this]() {
      if (!desktopNotify_) return;
      desktopNotify_->setBlockedCategories(
          notifyBlockedCats_->text().split(QLatin1Char(','), Qt::SkipEmptyParts));
    });
  } else {
    desktopNotifyForward_->setEnabled(false);
    notifyUrgency_->setEnabled(false);
    notifySkipSync_->setEnabled(false);
    notifySkipTransient_->setEnabled(false);
    notifyBlockedApps_->setEnabled(false);
    notifyBlockedCats_->setEnabled(false);
  }
  lay->addWidget(deskBox);

  auto* callBox = new QGroupBox(tr("Incoming call"), this);
  auto* callRow = new QHBoxLayout(callBox);
  callName_ = new QLineEdit(callBox);
  callName_->setPlaceholderText(tr("Caller name"));
  callName_->setClearButtonEnabled(true);
  connect(callName_, &QLineEdit::returnPressed, this, &NotifyTab::sendCall);
  callBtn_ = new QPushButton(tr("Ring"), callBox);
  setButtonIcon(callBtn_, QStringLiteral("call"));
  callBtn_->setToolTip(tr("Show the incoming-call screen with this name"));
  endCallBtn_ = new QPushButton(tr("End call"), callBox);
  setButtonIcon(endCallBtn_, QStringLiteral("clear"));
  endCallBtn_->setToolTip(tr("Dismiss the call screen on the watch"));
  connect(callBtn_, &QPushButton::clicked, this, &NotifyTab::sendCall);
  connect(endCallBtn_, &QPushButton::clicked, this, &NotifyTab::endCall);
  callRow->addWidget(callName_, 1);
  callRow->addWidget(callBtn_);
  callRow->addWidget(endCallBtn_);
  lay->addWidget(callBox);

  auto* contactBox = new QGroupBox(tr("Contacts"), this);
  auto* contactLay = new QVBoxLayout(contactBox);
  contactsTable_ = new QTableWidget(0, 2, contactBox);
  contactsTable_->setHorizontalHeaderLabels({tr("Name"), tr("Phone")});
  contactsTable_->horizontalHeader()->setStretchLastSection(true);
  contactsTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  contactsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
  contactsTable_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  contactsTable_->setMaximumHeight(140);
  contactLay->addWidget(contactsTable_);
  auto* contactBtns = new QHBoxLayout;
  contactAddBtn_ = new QPushButton(tr("Add"), contactBox);
  setButtonIcon(contactAddBtn_, QStringLiteral("add"));
  contactRemoveBtn_ = new QPushButton(tr("Remove"), contactBox);
  setButtonIcon(contactRemoveBtn_, QStringLiteral("delete"));
  contactPushBtn_ = new QPushButton(tr("Push to watch"), contactBox);
  setButtonIcon(contactPushBtn_, QStringLiteral("contact"));
  contactClearBtn_ = new QPushButton(tr("Clear on watch"), contactBox);
  setButtonIcon(contactClearBtn_, QStringLiteral("clear"));
  contactPushBtn_->setToolTip(tr("Write the table as the watch contact list (0x72)."));
  contactClearBtn_->setToolTip(tr("Push an empty contact list to clear the watch."));
  connect(contactAddBtn_, &QPushButton::clicked, this, &NotifyTab::addContactRow);
  connect(contactRemoveBtn_, &QPushButton::clicked, this, &NotifyTab::removeSelectedContacts);
  connect(contactPushBtn_, &QPushButton::clicked, this, &NotifyTab::pushContacts);
  connect(contactClearBtn_, &QPushButton::clicked, this, &NotifyTab::clearContactsOnWatch);
  contactBtns->addWidget(contactAddBtn_);
  contactBtns->addWidget(contactRemoveBtn_);
  contactBtns->addStretch();
  contactBtns->addWidget(contactPushBtn_);
  contactBtns->addWidget(contactClearBtn_);
  contactLay->addLayout(contactBtns);
  lay->addWidget(contactBox);

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
  setButtonIcon(musicPushBtn_, QStringLiteral("send"));
  musicPushBtn_->setToolTip(
      tr("Send now-playing metadata so the watch can show the track "
         "(enable the music feature in Settings if needed)"));
  connect(musicPushBtn_, &QPushButton::clicked, this, &NotifyTab::pushNowPlaying);
  musicPullBtn_ = new QPushButton(tr("From player"), musicBox);
  setButtonIcon(musicPullBtn_, QStringLiteral("music"));
  musicPullBtn_->setToolTip(tr("Fill the fields from the active MPRIS media player"));
  connect(musicPullBtn_, &QPushButton::clicked, this, &NotifyTab::pullFromMpris);
  musicLastAction_ = new QLabel(tr("Watch keys: —"), musicBox);
  musicLastAction_->setToolTip(tr("Last media key received from the watch"));
  musicRow->addWidget(musicPushBtn_);
  musicRow->addWidget(musicPullBtn_);
  musicRow->addWidget(musicLastAction_, 1);
  mprisForward_ = new QCheckBox(tr("Forward watch media keys to system player (MPRIS)"), musicBox);
  mprisForward_->setToolTip(
      tr("When enabled, Next / Previous / Play-Pause from the watch control the "
         "active MPRIS player (e.g. browsers, music apps)."));
  if (mpris_) {
    mprisForward_->setChecked(mpris_->forwardKeys());
    connect(mprisForward_, &QCheckBox::toggled, mpris_, &MprisController::setForwardKeys);
  } else {
    mprisForward_->setEnabled(false);
  }
  form->addRow(mprisForward_);
  mprisAutoPush_ = new QCheckBox(tr("Push system track changes to the watch"), musicBox);
  mprisAutoPush_->setToolTip(
      tr("When enabled and connected, now-playing metadata from the active "
         "MPRIS player is sent to the watch whenever the track or play state changes."));
  if (mpris_) {
    mprisAutoPush_->setChecked(mpris_->autoPush());
    connect(mprisAutoPush_, &QCheckBox::toggled, mpris_, &MprisController::setAutoPush);
  } else {
    mprisAutoPush_->setEnabled(false);
  }
  form->addRow(mprisAutoPush_);
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
  if (musicPullBtn_) musicPullBtn_->setEnabled(true); // MPRIS does not need the watch
  contactPushBtn_->setEnabled(connected);
  contactClearBtn_->setEnabled(connected);
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
  const QString name = QString::fromUtf8(proto::toString(action));
  QString extra;
  if (mpris_ && mpris_->forwardKeys())
    extra = mpris_->hasPlayers() ? tr(" → player") : tr(" → no MPRIS player");
  musicLastAction_->setText(tr("Watch keys: %1%2").arg(name, extra));
  status_->setText(tr("Watch media key: %1").arg(name));
}

void NotifyTab::pullFromMpris() {
  if (!mpris_) {
    status_->setText(tr("MPRIS helper not available."));
    return;
  }
  auto np = mpris_->currentTrack();
  if (!np) {
    status_->setText(tr("No MPRIS player with track metadata found."));
    return;
  }
  musicTitle_->setText(QString::fromStdString(np->title));
  musicArtist_->setText(QString::fromStdString(np->artist));
  musicAlbum_->setText(QString::fromStdString(np->album));
  musicPlaying_->setChecked(np->playing);
  musicVolume_->setValue(np->volume);
  status_->setText(tr("Filled from system player."));
}

void NotifyTab::addContactRow() {
  const int row = contactsTable_->rowCount();
  contactsTable_->insertRow(row);
  contactsTable_->setItem(row, 0, new QTableWidgetItem);
  contactsTable_->setItem(row, 1, new QTableWidgetItem);
  contactsTable_->editItem(contactsTable_->item(row, 0));
}

void NotifyTab::removeSelectedContacts() {
  const auto rows = contactsTable_->selectionModel()->selectedRows();
  QList<int> indexes;
  for (const QModelIndex& i : rows) indexes.push_back(i.row());
  std::sort(indexes.begin(), indexes.end(), std::greater<int>());
  for (int r : indexes) contactsTable_->removeRow(r);
}

void NotifyTab::pushContacts() {
  if (!connected_) return;
  std::vector<proto::Contact> list;
  for (int r = 0; r < contactsTable_->rowCount(); ++r) {
    const auto* nameItem = contactsTable_->item(r, 0);
    const auto* phoneItem = contactsTable_->item(r, 1);
    const QString name = nameItem ? nameItem->text().trimmed() : QString();
    const QString phone = phoneItem ? phoneItem->text().trimmed() : QString();
    if (name.isEmpty() && phone.isEmpty()) continue;
    proto::Contact c;
    c.id = static_cast<uint8_t>(list.size() + 1);
    c.name = name.toStdString();
    c.phone = phone.toStdString();
    list.push_back(std::move(c));
  }
  if (list.empty()) {
    status_->setText(tr("Add at least one contact, or use Clear on watch."));
    return;
  }
  auto packets = std::make_shared<std::vector<proto::Bytes>>(proto::contactWritePackets(list));
  auto index = std::make_shared<size_t>(0);
  auto sendNext = std::make_shared<std::function<void()>>();
  *sendNext = [this, packets, index, sendNext]() {
    if (!connected_ || *index >= packets->size()) return;
    bridge_.send((*packets)[(*index)++]);
    if (*index < packets->size()) QTimer::singleShot(120, this, *sendNext);
  };
  (*sendNext)();
  status_->setText(tr("Contacts pushed (%1 entries, %2 packet(s)).")
                       .arg(list.size())
                       .arg(packets->size()));
}

void NotifyTab::clearContactsOnWatch() {
  if (!connected_) return;
  // Empty list write clears the stored contacts (same as CLI --contacts '').
  auto packets = std::make_shared<std::vector<proto::Bytes>>(
      proto::contactWritePackets({}));
  auto index = std::make_shared<size_t>(0);
  auto sendNext = std::make_shared<std::function<void()>>();
  *sendNext = [this, packets, index, sendNext]() {
    if (!connected_ || *index >= packets->size()) return;
    bridge_.send((*packets)[(*index)++]);
    if (*index < packets->size()) QTimer::singleShot(120, this, *sendNext);
  };
  (*sendNext)();
  status_->setText(tr("Cleared contacts on the watch (%1 packet(s)).").arg(packets->size()));
}
