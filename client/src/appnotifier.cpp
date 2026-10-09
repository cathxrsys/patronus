#include "appnotifier.h"

#include <QCoreApplication>

#include "androidsystemui.h"

#ifndef Q_OS_ANDROID
#include <QIcon>
#include <QSystemTrayIcon>
#endif

AppNotifier::AppNotifier(AndroidSystemUi* androidSystemUi, QObject* parent)
    : QObject(parent), m_androidSystemUi(androidSystemUi) {
#ifndef Q_OS_ANDROID
  if (QSystemTrayIcon::isSystemTrayAvailable()) {
    m_trayIcon = new QSystemTrayIcon(
        QIcon(QStringLiteral(":/icons/main.png")), this);
    m_trayIcon->setToolTip(QStringLiteral("patronus"));
    m_trayIcon->show();
  }
#endif
}

AppNotifier::~AppNotifier() = default;

void AppNotifier::showNotification(const QString& title,
                                   const QString& message) {
  const QString resolvedTitle =
      title.isEmpty() ? QCoreApplication::applicationName() : title;

#ifdef Q_OS_ANDROID
  if (m_androidSystemUi != nullptr) {
    m_androidSystemUi->showSystemNotification(resolvedTitle, message);
  }
#else
  if (m_trayIcon != nullptr) {
    m_trayIcon->showMessage(resolvedTitle, message,
                            QSystemTrayIcon::Information, 5000);
  }
#endif
}

void AppNotifier::showInfo(const QString& message) {
  showNotification(QCoreApplication::applicationName(), message);
}