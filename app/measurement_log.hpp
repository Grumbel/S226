// Appends bpm / spm readings to one CSV file per day:
//   ~/.local/share/s226/s226-hr/s226-hr-YYYY-MM-DD.csv
//   time,bpm,spm
//   2026-10-02T10:15:03.123,72,
//   2026-10-02T10:15:03.480,,112

#pragma once

#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QString>

#include <optional>
#include <vector>

class MeasurementLog {
public:
  struct Row {
    qint64 msecs = 0; // since epoch
    std::optional<int> bpm;
    std::optional<int> spm;
  };

  MeasurementLog();

  QString directory() const { return dir_; }
  QString currentFile() const { return file_.fileName(); }

  void write(const QDateTime& time, std::optional<int> bpm, std::optional<int> spm);

  // Rows newer than `since` from the files of the last two days.
  std::vector<Row> readSince(const QDateTime& since) const;

private:
  QString pathFor(const QDate& date) const;

  QString dir_;
  QFile file_;
  QDate fileDate_;
};
