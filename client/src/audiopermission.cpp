#include "audiopermission.h"

#include <QObject>

#if defined(Q_OS_ANDROID) || defined(Q_OS_MACOS) || defined(Q_OS_IOS)
#include <QCoreApplication>
#include <QPermissions>
#endif

namespace audiopermission {

void ensureMicrophoneAccess(QObject* context,
                            std::function<void(bool granted)> onResult) {
#if defined(Q_OS_ANDROID) || defined(Q_OS_MACOS) || defined(Q_OS_IOS)
  auto* application = QCoreApplication::instance();
  if (application == nullptr) {
    onResult(false);
    return;
  }

  QMicrophonePermission permission;
  switch (application->checkPermission(permission)) {
    case Qt::PermissionStatus::Granted:
      onResult(true);
      return;
    case Qt::PermissionStatus::Denied:
      onResult(false);
      return;
    case Qt::PermissionStatus::Undetermined:
      application->requestPermission(
          permission, context, [onResult](const QPermission& result) {
            onResult(result.status() == Qt::PermissionStatus::Granted);
          });
      return;
  }
#else
  Q_UNUSED(context);
  onResult(true);
#endif
}

}  // namespace audiopermission
