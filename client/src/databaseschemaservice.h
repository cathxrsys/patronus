#pragma once

#include <QString>

class QSqlDatabase;

class DatabaseSchemaService {
 public:
  bool ensureSchema(QSqlDatabase* database, QString* lastError = nullptr) const;
};