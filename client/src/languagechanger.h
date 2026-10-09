#include <QObject>
#include <QQmlApplicationEngine>
#include <QSettings>

#include "apppaths.h"
#include "localeutils.h"

class LanguageChanger : public QObject {
  Q_OBJECT
  Q_PROPERTY(
      QString currentLocale READ getCurrentLocale NOTIFY currentLocaleChanged)
  Q_PROPERTY(QString currentFlagAssetCode READ getCurrentFlagAssetCode NOTIFY
                 currentLocaleChanged)
  Q_PROPERTY(QString currentLocaleFullName READ getCurrentLocaleFullName NOTIFY
                 currentLocaleFullNameChanged)
 public:
  explicit LanguageChanger(QObject *parent = nullptr);
  void setEngine(QQmlApplicationEngine *engine);
  void saveLanguage(const QString &locale);
  void loadLanguage(const QString &locale) {
    changeLanguage(localeutils::localeToIndex(locale));
  }

  QString currentLocale;
  QString currentLocaleFullName;

  Q_INVOKABLE QString getCurrentLocale() const { return currentLocale; }

  QString getCurrentFlagAssetCode() const {
    return localeutils::flagAssetCodeForLocale(currentLocale);
  }

  Q_INVOKABLE QString flagAssetCodeForLocale(const QString& locale) const {
    return localeutils::flagAssetCodeForLocale(locale);
  }

  Q_INVOKABLE QString getCurrentLocaleFullName() const {
    return currentLocaleFullName;
  }

  Q_INVOKABLE QString savedLanguage() const {
    QSettings settings(apppaths::settingsFilePath(), QSettings::IniFormat);
    return localeutils::normalizeLanguageCode(
        settings.value("ui/language", "en").toString());
  }

 signals:
  void currentLocaleChanged();
  void currentLocaleFullNameChanged();
  void languageChanged(int index);

 public slots:
  void changeLanguage(int index);
  void reloadQml();

 private:
  QQmlApplicationEngine *m_engine = nullptr;
};