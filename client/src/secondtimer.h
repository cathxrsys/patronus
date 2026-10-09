#ifndef SECONDTIMER_H
#define SECONDTIMER_H

#include <QDebug>
#include <QObject>
#include <QThread>
#include <QTimer>

#include "mainsignals.h"

class SecondTimer : public QObject {
  Q_OBJECT
 public:
  explicit SecondTimer(QObject *parent = nullptr) : QObject(parent) {}

 public slots:
  void start() {
    QTimer *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &SecondTimer::doWork);
    timer->start(1000);  // send an online request every 1 second
    qDebug() << "SecondTimer started in thread:" << QThread::currentThreadId();
  }

 private slots:
  void doWork() {
    MainSignals::instance().emitSecondTimerTicked();
    // qDebug() << "SecondTimer ticked in thread:" <<
    // QThread::currentThreadId();
  }
};

#endif  // SECONDTIMER_H