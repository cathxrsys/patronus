#ifndef APPNOTIFIER_H
#define APPNOTIFIER_H

#include <QObject>

class AndroidSystemUi;

#ifndef Q_OS_ANDROID
class QSystemTrayIcon;
#endif

class AppNotifier : public QObject {
  Q_OBJECT

 public:
  explicit AppNotifier(AndroidSystemUi* androidSystemUi,
                       QObject* parent = nullptr);
  ~AppNotifier() override;

  Q_INVOKABLE void showNotification(const QString& title,
                                    const QString& message);
  Q_INVOKABLE void showInfo(const QString& message);

 private:
  AndroidSystemUi* m_androidSystemUi = nullptr;

#ifndef Q_OS_ANDROID
  QSystemTrayIcon* m_trayIcon = nullptr;
#endif
};

#endif