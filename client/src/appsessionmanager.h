#pragma once

#include <QObject>

class DatabaseManager;

class AppSessionManager : public QObject {
  Q_OBJECT

  Q_PROPERTY(bool databaseOpen READ databaseOpen NOTIFY databaseOpenChanged)
  Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

 public:
  explicit AppSessionManager(DatabaseManager* db, QObject* parent = nullptr);

  Q_INVOKABLE bool openWithPassword(const QString& password);
  Q_INVOKABLE bool changeDatabasePassword(const QString& newPassword);

  bool databaseOpen() const;
  QString lastError() const;

 signals:
  void databaseOpenChanged();
  void lastErrorChanged();

 private:
  DatabaseManager* m_db;
};