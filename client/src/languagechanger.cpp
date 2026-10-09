#include "languagechanger.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QSettings>
#include <QTranslator>

LanguageChanger::LanguageChanger(QObject *parent) : QObject(parent) {}

void LanguageChanger::changeLanguage(int index) {
  qDebug() << "Changing language to index:" << index;
  static QTranslator translator;
  QGuiApplication::removeTranslator(&translator);

  const QString locale = localeutils::localeFromIndex(index);

  if (locale == "en") {
    QGuiApplication::removeTranslator(&translator);
    currentLocale = locale;
    currentLocaleFullName = localeutils::displayNameForLanguageCode(locale);
    saveLanguage(locale);
    emit currentLocaleChanged();
    emit currentLocaleFullNameChanged();
    emit languageChanged(index);
    reloadQml();
    return;
  }
  bool loaded = translator.load(":/translations/client_" + locale + ".qm");
  if (loaded) {
    QGuiApplication::installTranslator(&translator);
    currentLocale = locale;
    currentLocaleFullName = localeutils::displayNameForLanguageCode(locale);
    saveLanguage(locale);
    emit currentLocaleChanged();
    emit currentLocaleFullNameChanged();
    emit languageChanged(index);
    reloadQml();
  } else {
    qWarning() << "Failed to load translation for locale:" << locale;
  }
}

void LanguageChanger::saveLanguage(const QString &locale) {
  const QString normalized = localeutils::normalizeLanguageCode(locale);
  qDebug() << "Saving language setting:" << normalized;
  QSettings settings(apppaths::settingsFilePath(), QSettings::IniFormat);
  settings.setValue("ui/language", normalized);
}

void LanguageChanger::setEngine(QQmlApplicationEngine *engine) {
  m_engine = engine;
}

void LanguageChanger::reloadQml() {
  if (!m_engine) return;
  m_engine->retranslate();
}