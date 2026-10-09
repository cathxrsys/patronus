#include "settingsmanager.h"

#include "databasemanager.h"

SettingsManager::SettingsManager(DatabaseManager* db, QObject* parent)
    : QObject(parent), m_db(db), m_settingsService(db) {
  connect(m_db, &DatabaseManager::databaseOpened, this,
          [this](const QString&) { preloadSettings(); });
  connect(m_db, &DatabaseManager::defaultsReseeded, this,
          [this]() { preloadSettings(); });
}

void SettingsManager::preloadSettings() {
  m_cachedBoolSettings = m_settingsService.loadAllBoolSettings();
  m_cachedTextSettings = m_settingsService.loadAllTextSettings();
  m_cachedBlobSettings = m_settingsService.loadAllBlobSettings();
}

bool SettingsManager::getBoolSetting(const QString& settingName,
                                     bool defaultValue) {
  if (m_cachedBoolSettings.contains(settingName)) {
    return m_cachedBoolSettings.value(settingName);
  }

  bool finalValue =
      m_settingsService.loadBoolSetting(settingName, defaultValue);
  m_cachedBoolSettings.insert(settingName, finalValue);
  return finalValue;
}

void SettingsManager::setBoolSetting(const QString& settingName, bool value) {
  MainSignals::instance().emitSettingSetBool(settingName, value);

  qDebug() << "[SettingsManager] [setBoolSetting] Setting name:" << settingName
           << ", value:" << value;

  m_cachedBoolSettings.insert(settingName, value);
}

void SettingsManager::clearCache() {
  qDebug() << "[SettingsManager] [clearCache] Dropping all cached settings";
  m_cachedTextSettings.clear();
  m_cachedBoolSettings.clear();
  m_cachedBlobSettings.clear();
}

QString SettingsManager::getTextSetting(const QString& settingName,
                                        const QString& defaultValue) {
  if (m_cachedTextSettings.contains(settingName)) {
    return m_cachedTextSettings.value(settingName);
  }

  if (!m_db->isOpen()) {
    qDebug() << "[SettingsManager] [getTextSetting] Database is not open. "
                "Returning default for text setting name:"
             << settingName;
    return defaultValue;
  }

  QString finalValue =
      m_settingsService.loadTextSetting(settingName, defaultValue);
  m_cachedTextSettings.insert(settingName, finalValue);
  return finalValue;
}

void SettingsManager::setTextSetting(const QString& settingName,
                                     const QString& value) {
  MainSignals::instance().emitSettingSetText(settingName, value);

  qDebug() << "[SettingsManager] [setTextSetting] Setting text name:"
           << settingName << ", value:" << value;

  m_cachedTextSettings.insert(settingName, value);
  emit settingChanged(settingName, value);
}

QByteArray SettingsManager::getBlobSetting(const QString& settingName,
                                           const QByteArray& defaultValue) {
  if (m_cachedBlobSettings.contains(settingName)) {
    qDebug() << "[SettingsManager] [getBlobSetting] Returning cached blob for "
                "setting name:"
             << settingName
             << ", size:" << m_cachedBlobSettings.value(settingName).size();
    return m_cachedBlobSettings.value(settingName);
  }

  if (!m_db->isOpen()) {
    qDebug() << "[SettingsManager] [getBlobSetting] Database is not open. "
                "Returning default for blob setting name:"
             << settingName;
    return defaultValue;
  }

  QByteArray finalValue =
      m_settingsService.loadBlobSetting(settingName, defaultValue);
  m_cachedBlobSettings.insert(settingName, finalValue);
  return finalValue;
}

void SettingsManager::setBlobSetting(const QString& settingName,
                                     const QByteArray& value) {
  MainSignals::instance().emitSettingSetBlob(settingName, value);

  qDebug() << "[SettingsManager] [setBlobSetting] Setting blob name:"
           << settingName << ", size:" << value.size();

  m_cachedBlobSettings.insert(settingName, value);
}
