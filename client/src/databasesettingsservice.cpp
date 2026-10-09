#include "databasesettingsservice.h"

#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include "databasemanager.h"

DatabaseSettingsService::DatabaseSettingsService(DatabaseManager* db)
    : m_db(db) {}

void DatabaseSettingsService::setDatabaseManager(DatabaseManager* db) {
  m_db = db;
}

QHash<QString, bool> DatabaseSettingsService::loadAllBoolSettings() const {
  QHash<QString, bool> values;
  if (m_db == nullptr || !m_db->isOpen()) {
    return values;
  }

  const QVariantList rows =
      m_db->select("SELECT name, value FROM boolsettings", {});
  for (const QVariant& rowVar : rows) {
    const QVariantMap row = rowVar.toMap();
    values.insert(row.value("name").toString(), row.value("value").toBool());
  }

  return values;
}

QHash<QString, QString> DatabaseSettingsService::loadAllTextSettings() const {
  QHash<QString, QString> values;
  if (m_db == nullptr || !m_db->isOpen()) {
    return values;
  }

  const QVariantList rows =
      m_db->select("SELECT name, value FROM textsettings", {});
  for (const QVariant& rowVar : rows) {
    const QVariantMap row = rowVar.toMap();
    values.insert(row.value("name").toString(), row.value("value").toString());
  }

  return values;
}

QHash<QString, QByteArray> DatabaseSettingsService::loadAllBlobSettings()
    const {
  QHash<QString, QByteArray> values;
  if (m_db == nullptr || !m_db->isOpen()) {
    return values;
  }

  const QVariantList rows =
      m_db->select("SELECT name, value FROM blobsettings", {});
  for (const QVariant& rowVar : rows) {
    const QVariantMap row = rowVar.toMap();
    values.insert(row.value("name").toString(),
                  row.value("value").toByteArray());
  }

  return values;
}

bool DatabaseSettingsService::loadBoolSetting(const QString& name,
                                              bool defaultValue) const {
  if (m_db == nullptr || !m_db->isOpen()) {
    return defaultValue;
  }

  const QVariantMap row =
      m_db->selectOne("SELECT value FROM boolsettings WHERE name = ?", {name})
          .toMap();
  return row.contains("value") ? row.value("value").toBool() : defaultValue;
}

QString DatabaseSettingsService::loadTextSetting(
    const QString& name, const QString& defaultValue) const {
  if (m_db == nullptr || !m_db->isOpen()) {
    return defaultValue;
  }

  const QVariantMap row =
      m_db->selectOne("SELECT value FROM textsettings WHERE name = ?", {name})
          .toMap();
  return row.contains("value") ? row.value("value").toString() : defaultValue;
}

QByteArray DatabaseSettingsService::loadBlobSetting(
    const QString& name, const QByteArray& defaultValue) const {
  if (m_db == nullptr || !m_db->isOpen()) {
    return defaultValue;
  }

  const QVariantMap row =
      m_db->selectOne("SELECT value FROM blobsettings WHERE name = ?", {name})
          .toMap();
  return row.contains("value") ? row.value("value").toByteArray()
                               : defaultValue;
}

QByteArray DatabaseSettingsService::loadBlobValue(const QString& name) const {
  if (m_db == nullptr || !m_db->isOpen()) {
    return {};
  }

  const QVariantMap row =
      m_db->selectOne("SELECT value FROM blobvalues WHERE name = ?", {name})
          .toMap();
  return row.value("value").toByteArray();
}

bool DatabaseSettingsService::saveBlobValue(const QString& name,
                                            const QByteArray& value) const {
  if (m_db == nullptr || !m_db->isOpen()) {
    return false;
  }

  return m_db->execute(
      "INSERT OR REPLACE INTO blobvalues (name, value) VALUES (?, ?)",
      {name, value});
}