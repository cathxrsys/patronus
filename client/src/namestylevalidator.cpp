#include "namestylevalidator.h"

#include <QDebug>
#include <QFile>
#include <QStringList>

namespace {
// The parser source is embedded by qt_add_qml_module (URI "client") under this
// resource path; see client/CMakeLists.txt.
constexpr auto kParserResource = ":/qt/qml/client/NameStyleParser.js";
}  // namespace

NameStyleValidator::NameStyleValidator() {
  QFile file(QString::fromLatin1(kParserResource));
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    qWarning() << "[NameStyleValidator] cannot open parser resource"
               << kParserResource << "- all styles will be rejected";
    return;
  }
  QString source = QString::fromUtf8(file.readAll());
  file.close();

  // Strip QML-only directives (.pragma / .import). A plain QJSEngine can't parse
  // them, and they'd be illegal inside the function wrapper below. NameStyleParser
  // only uses ".pragma library". Blank the lines (rather than delete them) so
  // line numbers in any error message still line up with the source file.
  QStringList lines = source.split(QLatin1Char('\n'));
  for (QString& line : lines) {
    const QStringView trimmed = QStringView(line).trimmed();
    if (trimmed.startsWith(QLatin1String(".pragma")) ||
        trimmed.startsWith(QLatin1String(".import"))) {
      line.clear();
    }
  }
  source = lines.join(QLatin1Char('\n'));

  // Wrap the library in an IIFE so its many helper functions stay private and
  // don't pollute the engine's global object; export only isValid().
  const QString program = QLatin1String("(function(){\n") + source +
                          QLatin1String("\nreturn { isValid: isValid };\n})()");

  const QJSValue module =
      m_engine.evaluate(program, QString::fromLatin1(kParserResource));
  if (module.isError()) {
    qWarning() << "[NameStyleValidator] failed to load parser:"
               << module.property(QStringLiteral("message")).toString();
    return;
  }

  m_isValidFn = module.property(QStringLiteral("isValid"));
  if (!m_isValidFn.isCallable()) {
    qWarning() << "[NameStyleValidator] isValid() is not callable";
    m_isValidFn = QJSValue();
  }
}

bool NameStyleValidator::isValid(const QString& css) {
  if (!m_isValidFn.isCallable()) {
    // Parser unavailable -> fail closed rather than accept an unchecked style.
    return false;
  }
  // Pass the string as a real argument (never interpolated into the program),
  // so a hostile style string can't inject code into the engine.
  const QJSValue result = m_isValidFn.call(QJSValueList{QJSValue(css)});
  if (result.isError()) {
    qWarning() << "[NameStyleValidator] parser threw:"
               << result.property(QStringLiteral("message")).toString();
    return false;
  }
  return result.toBool();
}
