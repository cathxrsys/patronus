#pragma once

#include <QLocale>
#include <QString>

namespace localeutils {

inline QString normalizeLanguageCode(const QString& locale) {
  QString normalized = locale.trimmed();
  if (normalized.isEmpty()) {
    return QStringLiteral("en");
  }

  const int underscore = normalized.indexOf(QLatin1Char('_'));
  const int dash = normalized.indexOf(QLatin1Char('-'));
  int separator = -1;
  if (underscore >= 0 && dash >= 0) {
    separator = qMin(underscore, dash);
  } else if (underscore >= 0) {
    separator = underscore;
  } else if (dash >= 0) {
    separator = dash;
  }
  if (separator > 0) {
    normalized = normalized.left(separator);
  }

  normalized = normalized.toLower();

  if (normalized == QStringLiteral("ua")) return QStringLiteral("uk");
  if (normalized == QStringLiteral("kz")) return QStringLiteral("kk");
  if (normalized == QStringLiteral("cn")) return QStringLiteral("zh");
  if (normalized == QStringLiteral("by")) return QStringLiteral("be");
  if (normalized == QStringLiteral("ch")) return QStringLiteral("sv");
  if (normalized == QStringLiteral("tj")) return QStringLiteral("tg");

  return normalized;
}

inline QString displayNameForLanguageCode(const QString& locale) {
  const QString normalized = normalizeLanguageCode(locale);

  if (normalized == QStringLiteral("en")) return QStringLiteral("English");
  if (normalized == QStringLiteral("ru")) return QStringLiteral("Русский");
  if (normalized == QStringLiteral("uk")) return QStringLiteral("Українська");
  if (normalized == QStringLiteral("kk")) return QStringLiteral("Қазақша");
  if (normalized == QStringLiteral("pl")) return QStringLiteral("Polski");
  if (normalized == QStringLiteral("fr")) return QStringLiteral("Français");
  if (normalized == QStringLiteral("de")) return QStringLiteral("Deutsch");
  if (normalized == QStringLiteral("it")) return QStringLiteral("Italiano");
  if (normalized == QStringLiteral("es")) return QStringLiteral("Español");
  if (normalized == QStringLiteral("nl")) return QStringLiteral("Nederlands");
  if (normalized == QStringLiteral("zh")) return QStringLiteral("中文");
  if (normalized == QStringLiteral("be")) return QStringLiteral("Беларуская");
  if (normalized == QStringLiteral("fi")) return QStringLiteral("Suomi");
  if (normalized == QStringLiteral("sv")) return QStringLiteral("Svenska");
  if (normalized == QStringLiteral("ro")) return QStringLiteral("Română");
  if (normalized == QStringLiteral("tg")) return QStringLiteral("Тоҷикӣ");

  return normalized;
}

inline QString flagAssetCodeForLocale(const QString& locale) {
  const QString normalized = normalizeLanguageCode(locale);

  if (normalized == QStringLiteral("uk")) return QStringLiteral("ua");
  if (normalized == QStringLiteral("kk")) return QStringLiteral("kz");
  if (normalized == QStringLiteral("zh")) return QStringLiteral("cn");
  if (normalized == QStringLiteral("be")) return QStringLiteral("by");
  if (normalized == QStringLiteral("sv")) return QStringLiteral("se");
  if (normalized == QStringLiteral("tg")) return QStringLiteral("tj");

  return normalized;
}

inline QLocale qLocaleForUiLanguage(const QString& locale) {
  const QString normalized = normalizeLanguageCode(locale);

  if (normalized == QStringLiteral("en")) return QLocale(QStringLiteral("en_US"));
  if (normalized == QStringLiteral("ru")) return QLocale(QStringLiteral("ru_RU"));
  if (normalized == QStringLiteral("uk")) return QLocale(QStringLiteral("uk_UA"));
  if (normalized == QStringLiteral("kk")) return QLocale(QStringLiteral("kk_KZ"));
  if (normalized == QStringLiteral("pl")) return QLocale(QStringLiteral("pl_PL"));
  if (normalized == QStringLiteral("fr")) return QLocale(QStringLiteral("fr_FR"));
  if (normalized == QStringLiteral("de")) return QLocale(QStringLiteral("de_DE"));
  if (normalized == QStringLiteral("it")) return QLocale(QStringLiteral("it_IT"));
  if (normalized == QStringLiteral("es")) return QLocale(QStringLiteral("es_ES"));
  if (normalized == QStringLiteral("nl")) return QLocale(QStringLiteral("nl_NL"));
  if (normalized == QStringLiteral("zh")) return QLocale(QStringLiteral("zh_CN"));
  if (normalized == QStringLiteral("be")) return QLocale(QStringLiteral("be_BY"));
  if (normalized == QStringLiteral("fi")) return QLocale(QStringLiteral("fi_FI"));
  if (normalized == QStringLiteral("sv")) return QLocale(QStringLiteral("sv_SE"));
  if (normalized == QStringLiteral("ro")) return QLocale(QStringLiteral("ro_RO"));
  if (normalized == QStringLiteral("tg")) return QLocale(QStringLiteral("tg_TJ"));

  return QLocale::system();
}

inline int localeToIndex(const QString& locale) {
  const QString normalized = normalizeLanguageCode(locale);

  if (normalized == QStringLiteral("en")) return 0;
  if (normalized == QStringLiteral("ru")) return 1;
  if (normalized == QStringLiteral("uk")) return 2;
  if (normalized == QStringLiteral("kk")) return 3;
  if (normalized == QStringLiteral("pl")) return 4;
  if (normalized == QStringLiteral("fr")) return 5;
  if (normalized == QStringLiteral("de")) return 6;
  if (normalized == QStringLiteral("it")) return 7;
  if (normalized == QStringLiteral("es")) return 8;
  if (normalized == QStringLiteral("nl")) return 9;
  if (normalized == QStringLiteral("zh")) return 10;
  if (normalized == QStringLiteral("be")) return 11;
  if (normalized == QStringLiteral("fi")) return 12;
  if (normalized == QStringLiteral("sv")) return 13;
  if (normalized == QStringLiteral("ro")) return 14;
  if (normalized == QStringLiteral("tg")) return 15;

  return 0;
}

inline QString localeFromIndex(int index) {
  switch (index) {
    case 0:
      return QStringLiteral("en");
    case 1:
      return QStringLiteral("ru");
    case 2:
      return QStringLiteral("uk");
    case 3:
      return QStringLiteral("kk");
    case 4:
      return QStringLiteral("pl");
    case 5:
      return QStringLiteral("fr");
    case 6:
      return QStringLiteral("de");
    case 7:
      return QStringLiteral("it");
    case 8:
      return QStringLiteral("es");
    case 9:
      return QStringLiteral("nl");
    case 10:
      return QStringLiteral("zh");
    case 11:
      return QStringLiteral("be");
    case 12:
      return QStringLiteral("fi");
    case 13:
      return QStringLiteral("sv");
    case 14:
      return QStringLiteral("ro");
    case 15:
      return QStringLiteral("tg");
    default:
      return QStringLiteral("en");
  }
}

}  // namespace localeutils