#include <QApplication>
#include <QCommandLineParser>
#include <QLoggingCategory>
#include <QTimer>

#include "main_window.hpp"

int main(int argc, char** argv) {
  // Qt Multimedia only plays the metronome click. Skip FFmpeg's hardware
  // codec probing (it loads VDPAU/VA-API drivers and complains when they are
  // missing) and its version banner. Explicit settings from the environment win.
  for (const char* var : {"QT_FFMPEG_DECODING_HW_DEVICE_TYPES",
                          "QT_FFMPEG_ENCODING_HW_DEVICE_TYPES"}) {
    if (!qEnvironmentVariableIsSet(var)) qputenv(var, ",");
  }
  QLoggingCategory::setFilterRules(QStringLiteral("qt.multimedia.ffmpeg.info=false"));

  QApplication app(argc, argv);
  QApplication::setOrganizationName("s226");
  QApplication::setApplicationName("s226-hr");
  QApplication::setApplicationDisplayName("S226 Heart Rate");
  QApplication::setApplicationVersion(S226_VERSION);

  QCommandLineParser parser;
  parser.setApplicationDescription("Live heart-rate display for the S226 watch");
  parser.addHelpOption();
  parser.addVersionOption();
  QCommandLineOption controllerOpt(
      {"c", "controller"},
      "USB Bluetooth controller: index, vid:pid, port path or hciN (see s226-cli --list).",
      "selector");
  QCommandLineOption noConnectOpt("no-connect", "Do not connect automatically on start.");
  QCommandLineOption fullScreenOpt({"f", "fullscreen"}, "Start in full-screen mode.");
  parser.addOptions({controllerOpt, noConnectOpt, fullScreenOpt});
  parser.process(app);

  MainWindow window(parser.value(controllerOpt));
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
