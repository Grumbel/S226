#include "measurement_log.hpp"

#include <QDir>
#include <QStandardPaths>
#include <QTextStream>

namespace {

constexpr auto kTimeFormat = "yyyy-MM-ddTHH:mm:ss.zzz";

std::optional<int> parseField(const QString& s) {
  bool ok = false;
  const int v = s.toInt(&ok);
  return ok ? std::optional<int>(v) : std::nullopt;
}

} // namespace

MeasurementLog::MeasurementLog()
    : dir_(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)) {}

QString MeasurementLog::pathFor(const QDate& date) const {
  return QDir(dir_).filePath(QStringLiteral("s226-hr-%1.csv").arg(date.toString(Qt::ISODate)));
}

void MeasurementLog::write(const QDateTime& time, std::optional<int> bpm,
                           std::optional<int> spm) {
  const QDate date = time.date();
  if (!file_.isOpen() || date != fileDate_) {
    file_.close();
    QDir().mkpath(dir_);
    file_.setFileName(pathFor(date));
    const bool fresh = !file_.exists() || file_.size() == 0;
    if (!file_.open(QIODevice::Append | QIODevice::Text)) return;
    fileDate_ = date;
    if (fresh) file_.write("time,bpm,spm\n");
  }
  const QString line = QStringLiteral("%1,%2,%3\n")
                           .arg(time.toString(kTimeFormat),
                                bpm ? QString::number(*bpm) : QString(),
                                spm ? QString::number(*spm) : QString());
  file_.write(line.toUtf8());
  file_.flush();
}

std::vector<MeasurementLog::Row> MeasurementLog::readSince(const QDateTime& since) const {
  std::vector<Row> rows;
  const QDate today = QDate::currentDate();
  for (const QDate& date : {today.addDays(-1), today}) {
    if (date < since.date()) continue;
    QFile f(pathFor(date));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
    QTextStream in(&f);
    in.readLine(); // header
    while (!in.atEnd()) {
      const QStringList fields = in.readLine().split(',');
      if (fields.size() < 3) continue;
      const QDateTime t = QDateTime::fromString(fields[0], kTimeFormat);
      if (!t.isValid() || t < since) continue;
      rows.push_back({t.toMSecsSinceEpoch(), parseField(fields[1]), parseField(fields[2])});
    }
  }
  return rows;
}
