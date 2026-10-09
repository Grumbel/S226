#pragma once

#include <QIcon>
#include <QPushButton>
#include <QSize>
#include <QString>

// Classic shaded SVG icons shipped in the app resource bundle.
inline QIcon uiIcon(const QString& name) {
  return QIcon(QStringLiteral(":/menu-%1.svg").arg(name));
}

inline void setButtonIcon(QPushButton* btn, const QString& name, int px = 20) {
  if (!btn) return;
  btn->setIcon(uiIcon(name));
  btn->setIconSize(QSize(px, px));
}
