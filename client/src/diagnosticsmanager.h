#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>

class QTcpSocket;
class QTimer;
class QHostInfo;

// Backend for the Troubleshooting page. Runs the network-level checks QML can't
// do on its own — DNS resolution of the server host and a TCP "port ping" — and
// keeps a rolling in-app log (fed by the global Qt message handler) that the
// user can copy when reporting a connection problem.
//
// The "our PC" and "server application connected" steps live in QML: the former
// is always up, the latter is already reactive via the ConnectionManager
// (`connections`) signals. This class owns only DNS + port, which need Qt's
// network classes.
class DiagnosticsManager : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString host READ host NOTIFY targetChanged)
  Q_PROPERTY(int port READ port NOTIFY targetChanged)
  Q_PROPERTY(int dnsState READ dnsState NOTIFY dnsChanged)
  Q_PROPERTY(QString resolvedIp READ resolvedIp NOTIFY dnsChanged)
  Q_PROPERTY(int portState READ portState NOTIFY portChanged)
  Q_PROPERTY(int portLatencyMs READ portLatencyMs NOTIFY portChanged)
  Q_PROPERTY(bool running READ running NOTIFY runningChanged)

 public:
  // Kept as plain ints across the QML boundary (see the page's stepColor()).
  enum StepState { Idle = 0, InProgress = 1, Ok = 2, Failed = 3 };
  Q_ENUM(StepState)

  explicit DiagnosticsManager(QObject* parent = nullptr);
  ~DiagnosticsManager() override;

  // Installs a global Qt message handler that mirrors every log line into the
  // rolling buffer (still forwarding to the previous handler / logcat). Safe to
  // call before the QGuiApplication exists; call once, as early as possible.
  static void installLogCapture();

  QString host() const { return m_host; }
  int port() const { return m_port; }
  int dnsState() const { return m_dnsState; }
  QString resolvedIp() const { return m_resolvedIp; }
  int portState() const { return m_portState; }
  int portLatencyMs() const { return m_portLatencyMs; }
  bool running() const { return m_running; }

  // serverAddress is the bare "host:port" stored in settings (any ws(s)://
  // prefix is tolerated). Kicks off DNS, then a port ping to the resolved IP;
  // results arrive via the property-change signals.
  Q_INVOKABLE void runChecks(const QString& serverAddress);
  Q_INVOKABLE void cancel();

  // The rolling in-app log, oldest first, as one copy-ready blob.
  Q_INVOKABLE QString logText() const;
  Q_INVOKABLE void clearLog();

 signals:
  void targetChanged();
  void dnsChanged();
  void portChanged();
  void runningChanged();

 private:
  void resetResults();
  void onDnsResult(const QHostInfo& info);
  void startPortPing(const QString& ip);
  void finishPort(StepState result, int latencyMs);
  void teardownProbe();
  void setDnsState(StepState state);
  void setPortState(StepState state);
  void setRunning(bool running);

  QString m_host;
  int m_port = 0;
  StepState m_dnsState = Idle;
  QString m_resolvedIp;
  StepState m_portState = Idle;
  int m_portLatencyMs = -1;
  bool m_running = false;

  int m_dnsLookupId = -1;
  QTcpSocket* m_probe = nullptr;
  QTimer* m_probeTimeout = nullptr;
  QDateTime m_portStartedAt;
};
