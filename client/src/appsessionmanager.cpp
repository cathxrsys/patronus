#include "appsessionmanager.h"

#include "databasemanager.h"

AppSessionManager::AppSessionManager(DatabaseManager* db, QObject* parent)
    : QObject(parent), m_db(db) {
  if (m_db == nullptr) {
    return;
  }

  connect(m_db, &DatabaseManager::isOpenChanged, this,
          &AppSessionManager::databaseOpenChanged);
  connect(m_db, &DatabaseManager::lastErrorChanged, this,
          &AppSessionManager::lastErrorChanged);
}

bool AppSessionManager::openWithPassword(const QString& password) {
  if (m_db == nullptr) {
    return false;
  }

  return m_db->openDatabase(m_db->databasePath(), password);
}

bool AppSessionManager::changeDatabasePassword(const QString& newPassword) {
  if (m_db == nullptr) {
    return false;
  }

  return m_db->changePassword(newPassword);
}

bool AppSessionManager::databaseOpen() const {
  return m_db != nullptr && m_db->isOpen();
}

QString AppSessionManager::lastError() const {
  return m_db ? m_db->lastError() : QString();
}