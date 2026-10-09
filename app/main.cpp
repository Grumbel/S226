#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>

#include <unistd.h>

#include "main_window.hpp"

namespace {

// Per-user local socket name so two logins do not collide.
QString instanceServerName() {
  return QStringLiteral("s226-hr-%1").arg(::getuid());
}

// If another instance is already running, ask it to raise and return true.
bool handOffToExistingInstance() {
  QLocalSocket sock;
  sock.connectToServer(instanceServerName());
  if (!sock.waitForConnected(300)) return false;
  sock.write("raise\n");
  sock.waitForBytesWritten(300);
  sock.waitForDisconnected(300);
  return true;
}

// Listen for secondary instances; on contact, bring the main window forward.
QLocalServer* startInstanceServer(MainWindow* window) {
  const QString name = instanceServerName();
  QLocalServer::removeServer(name); // clear a stale socket after a crash
  auto* server = new QLocalServer(window);
  if (!server->listen(name)) {
    // Another process won the race; treat like a secondary instance.
    delete server;
    return nullptr;
  }
  QObject::connect(server, &QLocalServer::newConnection, window, [server, window]() {
    while (QLocalSocket* client = server->nextPendingConnection()) {
      QObject::connect(client, &QLocalSocket::readyRead, window, [client, window]() {
        client->readAll();
        window->bringToFront();
        client->disconnectFromServer();
        client->deleteLater();
      });
      QObject::connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
    }
  });
  return server;
}

} // namespace

int main(int argc, char** argv) {
  // Qt Multimedia only plays the metronome click. Skip FFmpeg's hardware
  // codec probing (it loads VDPAU/VA-API drivers and complains when they are
  // missing). Explicit settings from the environment win.
  for (const char* var : {"QT_FFMPEG_DECODING_HW_DEVICE_TYPES",
                          "QT_FFMPEG_ENCODING_HW_DEVICE_TYPES"}) {
    if (!qEnvironmentVariableIsSet(var)) qputenv(var, ",");
  }

  QApplication app(argc, argv);
  QApplication::setOrganizationName("s226");
  QApplication::setApplicationName("s226-hr");
  QApplication::setApplicationDisplayName("S226 Heart Rate");
  QApplication::setApplicationVersion(S226_VERSION);
  // Matches the .desktop file: Wayland app ID, taskbar icon and grouping.
  QGuiApplication::setDesktopFileName(QStringLiteral("s226-hr"));
  QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("s226-hr"),
                                               QIcon(QStringLiteral(":/s226-hr.svg"))));

  QCommandLineParser parser;
  parser.setApplicationDescription("Live heart-rate display for the S226 watch");
  parser.addHelpOption();
  parser.addVersionOption();
  QCommandLineOption controllerOpt(
      {"c", "controller"},
      "USB Bluetooth controller: index, vid:pid, port path or hciN (see s226-cli --list).",
      "selector");
  QCommandLineOption addressOpt(
      {"a", "address"}, "Only connect to the watch with this Bluetooth address.", "address");
  QCommandLineOption noConnectOpt("no-connect", "Do not connect automatically on start.");
  QCommandLineOption fullScreenOpt({"f", "fullscreen"}, "Start in full-screen mode.");
  QCommandLineOption multiInstanceOpt(
      "multi-instance",
      "Allow multiple instances (e.g. one per watch / controller). "
      "By default a second start raises the existing window and exits.");
  parser.addOptions({controllerOpt, addressOpt, noConnectOpt, fullScreenOpt, multiInstanceOpt});
  parser.process(app);

  const bool multi = parser.isSet(multiInstanceOpt);
  if (!multi && handOffToExistingInstance()) return 0;

  MainWindow window(parser.value(controllerOpt), parser.value(addressOpt));
  if (!multi) {
    if (!startInstanceServer(&window)) {
      // Lost the race to another process that started with us.
      if (handOffToExistingInstance()) return 0;
    }
  }
  if (parser.isSet(fullScreenOpt)) {
    window.showFullScreen();
  } else {
    window.show();
  }
  if (!parser.isSet(noConnectOpt)) {
    QTimer::singleShot(0, &window, &MainWindow::connectWatch);
  }
  return app.exec();
}
