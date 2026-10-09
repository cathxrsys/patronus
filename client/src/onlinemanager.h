#ifndef ONLINEMANAGER_H
#define ONLINEMANAGER_H

#include <QDebug>
#include <QObject>
#include <QThread>
#include <QTimer>

#include "connectionmanager.h"
#include "contactsmodel.h"
#include "databasemanager.h"
#include "mainsignals.h"

class OnlineManager : public QObject {
  Q_OBJECT
 public:
  explicit OnlineManager(DatabaseManager* db, ConnectionManager* connections,
                         ContactsModel* contactsModel,
                         QObject* parent = nullptr)
      : QObject(parent),
        m_db(db),
        m_connections(connections),
        m_contactsModel(contactsModel) {}

 public slots:
  void start() {
    QTimer* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &OnlineManager::doWork);
    timer->start(10000);  // send an online request every 10 seconds

    qDebug() << "OnlineManager started in thread:"
             << QThread::currentThreadId();
  }

 private slots:
  void doWork() { MainSignals::instance().emitTimerTicked(); }

 private:
  DatabaseManager* m_db;
  ConnectionManager* m_connections;
  ContactsModel* m_contactsModel;
};

#endif  // ONLINEMANAGER_H