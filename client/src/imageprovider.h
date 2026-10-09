#pragma once

#include <QCache>
#include <QImage>
#include <QQuickImageProvider>
#include <QUrl>

class AvatarProvider : public QQuickImageProvider {
 public:
  AvatarProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

  // Method called by the QML engine
  QImage requestImage(const QString &id, QSize *size,
                      const QSize &requestedSize) override {
    // 1. Strip everything after the question mark (our QML cache-busting trick)
    // If id = "0?t=176899...", cleanId becomes "0"
    QString cleanId = QUrl::fromPercentEncoding(id.split('?').at(0).toUtf8());

    // 2. Look up the image in the cache using the CLEAN key
    // QMutexLocker locker(&m_mutex); // if you use multithreading

    if (m_cache.contains(cleanId)) {
      QImage img = *m_cache.object(cleanId);

      if (size) {
        *size = img.size();
      }

      // If QML requested a specific size via sourceSize, scale the image
      if (requestedSize.isValid()) {
        return img.scaled(requestedSize, Qt::KeepAspectRatio,
                          Qt::SmoothTransformation);
      }

      qDebug() << "[AvatarProvider] Found avatar for" << cleanId
               << "(requested as" << id << ")";
      return img;
    }

    qDebug() << "[AvatarProvider] Resource not found for key:" << cleanId;
    return QImage();
  }

  // Method for adding data from your DB to the provider cache
  void addAvatar(const QString &id, const QByteArray &data) {
    QImage img;
    if (img.loadFromData(data)) {
      m_cache.insert(id, new QImage(img));
    }
  }

  bool hasAvatar(const QString &id) const {
    // If you use ids with parameters (?t=...),
    // strip them here as well before checking
    QString rawId = id.contains('?') ? id.split('?').at(0) : id;
    QString cleanId = QUrl::fromPercentEncoding(rawId.toUtf8());

    return m_cache.contains(cleanId);
  }

  bool removeAvatar(const QString &id) {
    // Like the other methods, strip QML cache parameters from the ID
    QString rawId = id.contains('?') ? id.split('?').at(0) : id;
    QString cleanId = QUrl::fromPercentEncoding(rawId.toUtf8());

    // QMutexLocker locker(&m_mutex); // if you use multithreading

    if (m_cache.contains(cleanId)) {
      return m_cache.remove(cleanId);
    }
    return false;
  }

  // Clear the cache completely
  void clearCache() {
    // QMutexLocker locker(&m_mutex);
    m_cache.clear();
  }

 private:
  QCache<QString, QImage>
  m_cache;  // Cache to avoid decoding the blob every time
};