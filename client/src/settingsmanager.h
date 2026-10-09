#pragma once

#include <QObject>

#include "databasemanager.h"
#include "databasesettingsservice.h"
#include "mainsignals.h"

class SettingsManager : public QObject {
  Q_OBJECT
 public:
  explicit SettingsManager(DatabaseManager* db, QObject* parent = nullptr);

  Q_INVOKABLE bool getBoolSetting(const QString& settingName,
                                  bool defaultValue = false);
  Q_INVOKABLE void setBoolSetting(const QString& settingName, bool value);

  Q_INVOKABLE QString getTextSetting(const QString& settingName,
                                     const QString& defaultValue = QString());
  Q_INVOKABLE void setTextSetting(const QString& settingName,
                                  const QString& value);

  Q_INVOKABLE QByteArray
  getBlobSetting(const QString& settingName,
                 const QByteArray& defaultValue = QByteArray());
  Q_INVOKABLE void setBlobSetting(const QString& settingName,
                                  const QByteArray& value = QByteArray());

  // Drops all in-memory cached settings so subsequent reads come from the
  // (possibly wiped) database. Used by the account-reset flow.
  Q_INVOKABLE void clearCache();

 signals:
  void settingChanged(const QString& settingName, const QString& value);

 private slots:
  void preloadSettings();

 private:
  DatabaseManager* m_db;
  DatabaseSettingsService m_settingsService;
  QHash<QString, QString> m_cachedTextSettings;
  QHash<QString, bool> m_cachedBoolSettings;
  QHash<QString, QByteArray> m_cachedBlobSettings;
};