#include <QGuiApplication>
#include <QObject>
#ifndef Q_OS_ANDROID
#include <QApplication>
#include <QCursor>
#include <QEventLoop>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QQuickWindow>
#include <QScreen>
#include <QSvgRenderer>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#endif
#ifdef Q_OS_ANDROID
#include <QPermissions>
#endif

#include <QDebug>
#include <QIcon>
#include <QQmlContext>
#include <QSettings>
#include <QSqlDatabase>
#include <QSurfaceFormat>
#include <QThread>

#include "src/accountmanager.h"
#include "src/androidsystemui.h"
#include "src/appnotifier.h"
#include "src/apppaths.h"
#include "src/appsessionmanager.h"
#include "src/appversion.h"
#include "src/audiodevicemanager.h"
#include "src/callmanager.h"
#include "src/clipboardhelper.h"
#include "src/connectionmanager.h"
#include "src/connectionstorage.h"
#include "src/contactsmodel.h"
#include "src/corecryptoadapter.h"
#include "src/databasemanager.h"
#include "src/diagnosticsmanager.h"
#include "src/filetransfermanager.h"
#include "src/fonthelper.h"
#include "src/imageprovider.h"
#include "src/languagechanger.h"
#include "src/localeutils.h"
#include "src/mainsignals.h"
#include "src/messagemodel.h"
#include "src/mobilepushmanager.h"
#include "src/onlinemanager.h"
#include "src/qrgenerator.h"
#include "src/qrscanner.h"
#include "src/secondtimer.h"
#include "src/settingsmanager.h"
#include "src/updatemanager.h"
#include "src/voicemessagemanager.h"
#include "src/wheelscroller.h"

#define DEFAULT_DB_PASSWORD "DefaultEmptyPassword|CoreClient|v1.0"

namespace {

#ifndef Q_OS_ANDROID
QScreen* desktopLaunchScreen() {
  if (QScreen* screen = QGuiApplication::screenAt(QCursor::pos())) {
    return screen;
  }

  return QGuiApplication::primaryScreen();
}

void moveWindowToScreen(QWindow* window, QScreen* screen) {
  if (!window || !screen) {
    return;
  }

  window->setScreen(screen);

  if (window->visibility() == QWindow::Maximized ||
      window->visibility() == QWindow::FullScreen) {
    return;
  }

  const QRect availableGeometry = screen->availableGeometry();
  window->setX(availableGeometry.center().x() - window->width() / 2);
  window->setY(availableGeometry.center().y() - window->height() / 2);
}

QWidget* createDesktopStartupSplash(QScreen* screen) {
  auto* splash =
      new QWidget(nullptr, Qt::FramelessWindowHint | Qt::SplashScreen);
  splash->setAttribute(Qt::WA_DeleteOnClose);
  splash->setObjectName(QStringLiteral("desktopStartupSplash"));
  splash->setFixedSize(420, 320);
  splash->setStyleSheet(QStringLiteral("background-color: #0a0f14;"));

  auto* layout = new QVBoxLayout(splash);
  layout->setContentsMargins(40, 36, 40, 36);
  layout->setSpacing(8);
  layout->addStretch();

  auto* logoLabel = new QLabel(splash);
  logoLabel->setFixedSize(90, 100);
  logoLabel->setAlignment(Qt::AlignCenter);

  QPixmap logoPixmap(logoLabel->size());
  logoPixmap.fill(Qt::transparent);
  {
    QPainter painter(&logoPixmap);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    QSvgRenderer renderer(QStringLiteral(":/qt/qml/client/resources/logo.svg"));
    renderer.render(&painter,
                    QRectF(0, 0, logoLabel->width(), logoLabel->height()));

    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(logoPixmap.rect(), QColor(QStringLiteral("#ffffff")));
  }

  

  logoLabel->setPixmap(logoPixmap);

  auto* titleLabel = new QLabel(QStringLiteral("Patronus"), splash);
  titleLabel->setAlignment(Qt::AlignCenter);
  titleLabel->setStyleSheet(
      QStringLiteral("color: #ffffff; background: transparent;"));

  const int fontId = QFontDatabase::addApplicationFont(
      QStringLiteral(":/qt/qml/client/resources/fonts/geologica.ttf"));
  QFont titleFont;
  if (fontId >= 0) {
    const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
    if (!families.isEmpty()) {
      titleFont.setFamily(families.constFirst());
    }
  }
  titleFont.setPixelSize(40);
  titleFont.setWeight(QFont::Medium);
  titleLabel->setFont(titleFont);

  layout->addWidget(logoLabel, 0, Qt::AlignHCenter);
  layout->addWidget(titleLabel, 0, Qt::AlignHCenter);
  layout->addStretch();

  splash->setWindowFlag(Qt::WindowStaysOnTopHint, true);
  splash->setWindowFlag(Qt::NoDropShadowWindowHint, true);

  if (screen) {
    const QRect availableGeometry = screen->availableGeometry();
    splash->move(availableGeometry.center() -
                 QPoint(splash->width() / 2, splash->height() / 2));
  }

  splash->show();
  splash->winId();
  splash->raise();
  splash->repaint();
  QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
  QCoreApplication::processEvents(QEventLoop::AllEvents);

  QEventLoop warmupLoop;
  QTimer::singleShot(90, &warmupLoop, &QEventLoop::quit);
  warmupLoop.exec(QEventLoop::AllEvents);

  splash->repaint();
  QCoreApplication::processEvents(QEventLoop::AllEvents);
  return splash;
}
#endif

#ifdef Q_OS_ANDROID
void requestAndroidMicrophonePermission() {
  auto* application = QCoreApplication::instance();
  if (application == nullptr) {
    return;
  }

  QMicrophonePermission permission;
  const auto status = application->checkPermission(permission);
  if (status == Qt::PermissionStatus::Granted) {
    return;
  }

  if (status == Qt::PermissionStatus::Denied) {
    qWarning() << "[Android] Microphone permission denied";
    return;
  }

  application->requestPermission(
      permission, application, [](const QPermission& result) {
        if (result.status() != Qt::PermissionStatus::Granted) {
          qWarning() << "[Android] Microphone permission was not granted";
        }
      });
}
#endif

}  // namespace

int main(int argc, char* argv[]) {

  std::cout << R"(

   ▄███████▄    ▄████████     ███        ▄████████  ▄██████▄  ███▄▄▄▄   ███    █▄     ▄████████ 
  ███    ███   ███    ███ ▀█████████▄   ███    ███ ███    ███ ███▀▀▀██▄ ███    ███   ███    ███ 
  ███    ███   ███    ███    ▀███▀▀██   ███    ███ ███    ███ ███   ███ ███    ███   ███    █▀  
  ███    ███   ███    ███     ███   ▀  ▄███▄▄▄▄██▀ ███    ███ ███   ███ ███    ███   ███        
▀█████████▀  ▀███████████     ███     ▀▀███▀▀▀▀▀   ███    ███ ███   ███ ███    ███ ▀███████████ 
  ███          ███    ███     ███     ▀███████████ ███    ███ ███   ███ ███    ███          ███ 
  ███          ███    ███     ███       ███    ███ ███    ███ ███   ███ ███    ███    ▄█    ███ 
 ▄████▀        ███    █▀     ▄████▀     ███    ███  ▀██████▀   ▀█   █▀  ████████▀   ▄████████▀  
                                        ███    ███                                              
              

 )";

  // Capture every Qt log line into a rolling in-app buffer (still forwarded to
  // logcat/stderr) so the Troubleshooting page can show and copy it. Install as
  // early as possible to catch startup logging.
  DiagnosticsManager::installLogCapture();

  // Without this, some Android GL configs (notably emulators using swiftshader
  // software rendering) hand Qt a low-precision surface (e.g. RGB565/4444)
  // instead of true 8-bit-per-channel color, making dark theme colors band and
  // washing out the MultiEffect-masked icon tinting (see ColorImage.qml).
  QSurfaceFormat format = QSurfaceFormat::defaultFormat();
  format.setRedBufferSize(8);
  format.setGreenBufferSize(8);
  format.setBlueBufferSize(8);
  format.setAlphaBufferSize(8);
  // Without an explicit color space, the Android compositor can negotiate a
  // different gamma than the sRGB values our QML colors are authored in,
  // lifting/tinting the darkest, most channel-imbalanced theme colors the
  // most (e.g. ThemeTelegraph's near-black-but-bluish #0a0f14) while barely
  // affecting lighter or more neutral themes.
  format.setColorSpace(QSurfaceFormat::sRGBColorSpace);
  QSurfaceFormat::setDefaultFormat(format);

#ifdef Q_OS_ANDROID
  QGuiApplication app(argc, argv);
#else
  QApplication app(argc, argv);
#endif

  app.setApplicationName(QStringLiteral("patronus"));
  app.setApplicationVersion(QString::fromUtf8(appversion::kApplicationVersion));

#ifndef Q_OS_ANDROID
  const QIcon applicationIcon(
      QStringLiteral(":/icons/main.png"));
  if (!applicationIcon.isNull()) {
    app.setWindowIcon(applicationIcon);
  }
#endif

#ifdef Q_OS_ANDROID
  requestAndroidMicrophonePermission();
#endif

#ifndef Q_OS_ANDROID
  QScreen* launchScreen = desktopLaunchScreen();
  QPointer<QWidget> startupSplash = createDesktopStartupSplash(launchScreen);
#endif

  QQmlApplicationEngine engine;
  AndroidSystemUi androidSystemUi;
  AppNotifier appNotifier(&androidSystemUi);

  QThread* _thread = new QThread(&app);

  qmlRegisterSingletonType<CoreCryptoAdapter>(
      "CoreCryptoAdapter", 1, 0, "CoreCrypto",
      [](QQmlEngine*, QJSEngine*) -> QObject* {
        return new CoreCryptoAdapter();
      });

  qmlRegisterSingletonType<ClipboardHelper>(
      "Clipboard", 1, 0, "ClipboardHelper",
      [](QQmlEngine*, QJSEngine*) -> QObject* {
        return new ClipboardHelper();
      });

  qmlRegisterType<DatabaseManager>("App.Database", 1, 0, "DatabaseManager");

  qmlRegisterType<WheelScroller>("App.Ui", 1, 0, "WheelScroller");

  qmlRegisterType<QrImageItem>("Qr", 1, 0, "QrImageItem");
  qmlRegisterType<QrScanner>("Qr", 1, 0, "QrScanner");

  qmlRegisterSingletonInstance("App.Signals", 1, 0, "MainSignals",
                               &MainSignals::instance());

  QObject::connect(&MainSignals::instance(), &MainSignals::doubleRatchetUpdate,
                   [](const QString& serverId, const QString& fromPubKey,
                      const nlohmann::json& doubleRatchetData) {
                     dr::DoubleRatchet td;
                     td.from_json(doubleRatchetData);
                     QString newSessionFingerprint = QString::fromStdString(
                         td.get_session_key_fingerprint());
                     // qDebug() << "Received new session fingerprint:" <<
                     // newSessionFingerprint;
                     MainSignals::instance().emitSessionFingerprintChanged(
                         serverId, fromPubKey, newSessionFingerprint);
                   });

  qDebug() << "Available SQL drivers:" << QSqlDatabase::drivers();

  // if (!QSqlDatabase::isDriverAvailable("QSQLCIPHER")) {
  //     qFatal("SQLCipher driver (QSQLCIPHER) is not available. Make sure it is
  //     correctly installed."); return -1;
  // } return in the future
  AvatarProvider* avatarProv = new AvatarProvider();

  DatabaseManager db(nullptr, avatarProv);
  AppSessionManager appSession(&db);
  SettingsManager settingsManager(&db);
  AudioDeviceManager audioDeviceManager(&settingsManager);

  engine.addImageProvider(QLatin1String("avatars"), avatarProv);

  AccountManager account(&db, nullptr, &settingsManager, avatarProv);
  ContactsModel contactsModel(nullptr, &db);
  MessageModel messageModel(nullptr, &db, &contactsModel);
  ConnectionStorage connectionStorage(&db);

  ConnectionManager connectionManager(nullptr, &connectionStorage, &account,
                                      &contactsModel, &settingsManager,
                                      &messageModel);
  MobilePushManager mobilePushManager(&connectionManager, &settingsManager,
                                      &androidSystemUi);
  UpdateManager updateManager(&settingsManager, &androidSystemUi);
  FileTransferManager fileTransferManager(
      &connectionManager, &db, &messageModel, &contactsModel, &settingsManager);
  VoiceMessageManager voiceMessageManager(&fileTransferManager,
                                          &audioDeviceManager);

  OnlineManager* onlineManager =
      new OnlineManager(&db, &connectionManager, &contactsModel);
  onlineManager->moveToThread(_thread);
  QObject::connect(_thread, &QThread::started, onlineManager,
                   &OnlineManager::start);
  QObject::connect(_thread, &QThread::finished, onlineManager,
                   &QObject::deleteLater);

  SecondTimer* secondTimer = new SecondTimer();
  secondTimer->moveToThread(_thread);
  QObject::connect(_thread, &QThread::started, secondTimer,
                   &SecondTimer::start);
  QObject::connect(_thread, &QThread::finished, secondTimer,
                   &QObject::deleteLater);

  connectionManager.setReconnectInterval(3000);  // 3 seconds before trying to reconnect
  connectionManager.setMaxReconnectAttempts(-1);  // unlimited reconnect attempts
  connectionManager.setConnectionTimeout(10000);  // 10 seconds connection timeout
  connectionManager.setPingInterval(15000);  // heartbeat during true idle
  connectionManager.setDeadConnectionTimeout(
      60000);  // dead only after 60s with NO inbound byte activity at all.
               // Large payloads are chunked (see protocol ChunkThreshold), so a
               // slow transfer keeps delivering ~32 KB pieces well inside this
               // window — the watchdog now only fires on a genuinely silent link,
               // never mid-download, however slow the peer's connection is.

  QObject::connect(&db, &DatabaseManager::databaseOpened, &connectionManager,
                   &ConnectionManager::addServers);
  QObject::connect(&db, &DatabaseManager::databaseOpened, &contactsModel,
                   &ContactsModel::loadFromDatabase);
  QObject::connect(&db, &DatabaseManager::databaseOpened, &account,
                   &AccountManager::loadOwnAvatar);
  QObject::connect(&db, &DatabaseManager::databaseOpened, &audioDeviceManager,
                   &AudioDeviceManager::refreshInputDevices);

  // -============= ==== ==== == ===  = =====

  const QString dbPath = apppaths::databasePath();

  bool public_key_found = false;
  if (!db.openDatabase(dbPath, DEFAULT_DB_PASSWORD)) {
  } else {
    if (account.getPublicKeyRaw().isEmpty()) {
      qWarning() << "No existing data found in the new database.";
    } else {
      qWarning() << "Data found in the new database, which is unexpected.";
      public_key_found = true;
    }
  }

  // qmlRegisterSingletonType<SettingsManager>(
  //     "SettingsManager", 1, 0, "Settings",
  //     [&db](QQmlEngine*, QJSEngine*) -> QObject* { return new
  //     SettingsManager(&db); }
  // );

  CallManager callManager(&engine, nullptr, &connectionManager, &db,
                          &messageModel, &contactsModel, &audioDeviceManager);
#ifndef Q_OS_ANDROID
  callManager.setPreferredDesktopScreen(launchScreen);
#endif
  QObject::connect(&androidSystemUi, &AndroidSystemUi::speakerphoneOnChanged,
                   &callManager, &CallManager::refreshAudioOutputRoute);
  ClipboardHelper clipboardHelper;
  FontHelper fontHelper;

  DiagnosticsManager diagnosticsManager;

  LanguageChanger langChanger;
  langChanger.setEngine(&engine);

  QSettings settings(apppaths::settingsFilePath(), QSettings::IniFormat);
  const QString savedLocale = settings.value("ui/language", "en").toString();
  const QString locale = localeutils::normalizeLanguageCode(savedLocale);
  if (savedLocale != locale) {
    settings.setValue("ui/language", locale);
  }
  langChanger.currentLocale = locale;
  langChanger.currentLocaleFullName =
      localeutils::displayNameForLanguageCode(locale);

  qDebug() << "Saved language:" << locale;

  if (locale != "en") {
    langChanger.loadLanguage(locale);
  }

  engine.rootContext()->setContextProperty("languageChanger", &langChanger);

  engine.rootContext()->setContextProperty("Settings", &settingsManager);
  engine.rootContext()->setContextProperty("clipboardHelper", &clipboardHelper);
  engine.rootContext()->setContextProperty("Fonts", &fontHelper);
  engine.rootContext()->setContextProperty("callManager", &callManager);
  engine.rootContext()->setContextProperty("contactsModel", &contactsModel);
  engine.rootContext()->setContextProperty("messageModel", &messageModel);
  engine.rootContext()->setContextProperty("db", &db);
  engine.rootContext()->setContextProperty("AppSession", &appSession);
  engine.rootContext()->setContextProperty("public_key_found",
                                           public_key_found);
  engine.rootContext()->setContextProperty("DEFAULT_DB_PASSWORD",
                                           DEFAULT_DB_PASSWORD);
  engine.rootContext()->setContextProperty("connections", &connectionManager);
  engine.rootContext()->setContextProperty("diagnostics", &diagnosticsManager);
  engine.rootContext()->setContextProperty("fileTransferManager",
                                           &fileTransferManager);
  engine.rootContext()->setContextProperty("audioDeviceManager",
                                           &audioDeviceManager);
  engine.rootContext()->setContextProperty("voiceMessageManager",
                                           &voiceMessageManager);
  engine.rootContext()->setContextProperty("Account", &account);
  engine.rootContext()->setContextProperty("AndroidSystemUi", &androidSystemUi);
  engine.rootContext()->setContextProperty("AppNotifier", &appNotifier);
  engine.rootContext()->setContextProperty("UpdateManager", &updateManager);

  engine.rootContext()->setContextProperty(
      "AvatarManager", avatarProv);

  _thread->start();

  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
      []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

  engine.loadFromModule("client", "Main");

#ifndef Q_OS_ANDROID
  QObject* rootObject = engine.rootObjects().isEmpty()
                            ? nullptr
                            : engine.rootObjects().constFirst();
  if (auto* window = qobject_cast<QQuickWindow*>(rootObject)) {
    if (!app.windowIcon().isNull()) {
      window->setIcon(app.windowIcon());
    }
    moveWindowToScreen(window, launchScreen);
  }

  if (startupSplash) {
    if (auto* window = qobject_cast<QQuickWindow*>(rootObject)) {
      QObject::connect(
          window, &QQuickWindow::frameSwapped, startupSplash,
          [startupSplash]() {
            if (startupSplash) {
              startupSplash->close();
            }
          },
          Qt::SingleShotConnection);
    } else {
      startupSplash->close();
    }
  }
#endif

  return app.exec();
}
