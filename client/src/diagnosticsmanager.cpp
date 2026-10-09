#include "diagnosticsmanager.h"

#include <QHostAddress>
#include <QHostInfo>
#include <QMutex>
#include <QStringList>
#include <QTcpSocket>
#include <QTime>
#include <QTimer>
#include <QUrl>

#if defined(Q_OS_ANDROID)
#include <android/log.h>
#endif

namespace {

constexpr int kProbeTimeoutMs = 6000;
constexpr int kMaxLogLines = 1000;
constexpr int kDefaultPort = 443;

// Rolling log buffer, written from the global message handler on arbitrary
// threads, read from the GUI thread via logText(). Guarded by its own mutex.
QMutex g_logMutex;
QStringList g_logLines;
QtMessageHandler g_prevHandler = nullptr;
bool g_logInstalled = false;

char levelChar(QtMsgType type) {
  switch (type) {
    case QtDebugMsg:
      return 'D';
    case QtInfoMsg:
      return 'I';
    case QtWarningMsg:
      return 'W';
    case QtCriticalMsg:
      return 'E';
    case QtFatalMsg:
      return 'F';
  }
  return '?';
}

#if defined(Q_OS_ANDROID)
android_LogPriority androidPriority(QtMsgType type) {
  switch (type) {
    case QtDebugMsg:
      return ANDROID_LOG_DEBUG;
    case QtInfoMsg:
      return ANDROID_LOG_INFO;
    case QtWarningMsg:
      return ANDROID_LOG_WARN;
    case QtCriticalMsg:
      return ANDROID_LOG_ERROR;
    case QtFatalMsg:
      return ANDROID_LOG_FATAL;
  }
  return ANDROID_LOG_DEFAULT;
}
#endif

void captureMessageHandler(QtMsgType type, const QMessageLogContext& context,
                           const QString& message) {
  const QString line = QStringLiteral("%1 %2 %3")
                           .arg(QTime::currentTime().toString(
                               QStringLiteral("HH:mm:ss.zzz")))
                           .arg(levelChar(type))
                           .arg(message);
  {
    QMutexLocker locker(&g_logMutex);
    g_logLines.append(line);
    if (g_logLines.size() > kMaxLogLines) {
      g_logLines.remove(0, g_logLines.size() - kMaxLogLines);
    }
  }

  // Keep the original output alive so logcat / stderr still receive everything.
  if (g_prevHandler) {
    g_prevHandler(type, context, message);
    return;
  }
#if defined(Q_OS_ANDROID)
  const char* tag =
      (context.category && *context.category) ? context.category : "default";
  __android_log_write(androidPriority(type), tag, message.toUtf8().constData());
#else
  fprintf(stderr, "%s\n",
          qUtf8Printable(qFormatLogMessage(type, context, message)));
  fflush(stderr);
#endif
}

}  // namespace

DiagnosticsManager::DiagnosticsManager(QObject* parent) : QObject(parent) {}

DiagnosticsManager::~DiagnosticsManager() { teardownProbe(); }

void DiagnosticsManager::installLogCapture() {
  if (g_logInstalled) {
    return;
  }
  g_logInstalled = true;
  g_prevHandler = qInstallMessageHandler(captureMessageHandler);
}

QString DiagnosticsManager::logText() const {
  QMutexLocker locker(&g_logMutex);
  return g_logLines.join(QLatin1Char('\n'));
}

void DiagnosticsManager::clearLog() {
  QMutexLocker locker(&g_logMutex);
  g_logLines.clear();
}

void DiagnosticsManager::runChecks(const QString& serverAddress) {
  cancel();

  // Accept the bare "host:port" from settings, tolerating a ws(s):// prefix.
  QString target = serverAddress.trimmed();
  const int scheme = target.indexOf(QStringLiteral("://"));
  if (scheme >= 0) {
    target = target.mid(scheme + 3);
  }
  const int slash = target.indexOf(QLatin1Char('/'));
  if (slash >= 0) {
    target = target.left(slash);
  }

  QString host = target;
  int port = kDefaultPort;
  const int colon = target.lastIndexOf(QLatin1Char(':'));
  if (colon > 0) {
    host = target.left(colon);
    const int parsed = target.mid(colon + 1).toInt();
    if (parsed > 0 && parsed <= 65535) {
      port = parsed;
    }
  }

  m_host = host;
  m_port = port;
  emit targetChanged();

  resetResults();

  if (m_host.isEmpty()) {
    setDnsState(Failed);
    return;
  }

  setRunning(true);
  setDnsState(InProgress);
  m_dnsLookupId = QHostInfo::lookupHost(m_host, this,
                                        &DiagnosticsManager::onDnsResult);
}

void DiagnosticsManager::cancel() {
  if (m_dnsLookupId != -1) {
    QHostInfo::abortHostLookup(m_dnsLookupId);
    m_dnsLookupId = -1;
  }
  teardownProbe();
  setRunning(false);
}

void DiagnosticsManager::resetResults() {
  m_resolvedIp.clear();
  m_dnsState = Idle;
  m_portState = Idle;
  m_portLatencyMs = -1;
  emit dnsChanged();
  emit portChanged();
}

void DiagnosticsManager::onDnsResult(const QHostInfo& info) {
  m_dnsLookupId = -1;

  if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
    m_resolvedIp.clear();
    setDnsState(Failed);
    setRunning(false);
    return;
  }

  // Prefer an IPv4 address (what the app actually connects over here); fall back
  // to whatever resolved first.
  QHostAddress chosen = info.addresses().constFirst();
  for (const QHostAddress& address : info.addresses()) {
    if (address.protocol() == QAbstractSocket::IPv4Protocol) {
      chosen = address;
      break;
    }
  }

  m_resolvedIp = chosen.toString();
  setDnsState(Ok);
  startPortPing(m_resolvedIp);
}

void DiagnosticsManager::startPortPing(const QString& ip) {
  setPortState(InProgress);
  m_portStartedAt = QDateTime::currentDateTime();

  m_probe = new QTcpSocket(this);
  connect(m_probe, &QTcpSocket::connected, this, [this]() {
    finishPort(Ok, static_cast<int>(m_portStartedAt.msecsTo(
                        QDateTime::currentDateTime())));
  });
  connect(m_probe, &QAbstractSocket::errorOccurred, this,
          [this](QAbstractSocket::SocketError) { finishPort(Failed, -1); });

  m_probeTimeout = new QTimer(this);
  m_probeTimeout->setSingleShot(true);
  connect(m_probeTimeout, &QTimer::timeout, this,
          [this]() { finishPort(Failed, -1); });
  m_probeTimeout->start(kProbeTimeoutMs);

  m_probe->connectToHost(ip, static_cast<quint16>(m_port));
}

void DiagnosticsManager::finishPort(StepState result, int latencyMs) {
  if (m_portState != InProgress) {
    return;
  }
  teardownProbe();
  m_portLatencyMs = latencyMs;
  setPortState(result);
  setRunning(false);
}

void DiagnosticsManager::teardownProbe() {
  if (m_probeTimeout) {
    m_probeTimeout->stop();
    m_probeTimeout->deleteLater();
    m_probeTimeout = nullptr;
  }
  if (m_probe) {
    m_probe->disconnect(this);
    m_probe->abort();
    m_probe->deleteLater();
    m_probe = nullptr;
  }
}

void DiagnosticsManager::setDnsState(StepState state) {
  if (m_dnsState == state) {
    return;
  }
  m_dnsState = state;
  emit dnsChanged();
}

void DiagnosticsManager::setPortState(StepState state) {
  if (m_portState == state) {
    return;
  }
  m_portState = state;
  emit portChanged();
}

void DiagnosticsManager::setRunning(bool running) {
  if (m_running == running) {
    return;
  }
  m_running = running;
  emit runningChanged();
}
