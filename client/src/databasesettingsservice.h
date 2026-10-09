#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>

class DatabaseManager;

class DatabaseSettingsService {
 public:
  explicit DatabaseSettingsService(DatabaseManager* db = nullptr);

  void setDatabaseManager(DatabaseManager* db);

  QHash<QString, bool> loadAllBoolSettings() const;
  QHash<QString, QString> loadAllTextSettings() const;
  QHash<QString, QByteArray> loadAllBlobSettings() const;

  bool loadBoolSetting(const QString& name, bool defaultValue = false) const;
  QString loadTextSetting(const QString& name,
                          const QString& defaultValue = QString()) const;
  QByteArray loadBlobSetting(
      const QString& name, const QByteArray& defaultValue = QByteArray()) const;

  QByteArray loadBlobValue(const QString& name) const;
  bool saveBlobValue(const QString& name, const QByteArray& value) const;

 private:
  DatabaseManager* m_db = nullptr;
};