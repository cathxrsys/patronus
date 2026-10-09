#ifndef NAMESTYLEVALIDATOR_H
#define NAMESTYLEVALIDATOR_H

#include <QJSEngine>
#include <QJSValue>
#include <QString>

// Validates "unique name style" CSS strings against the *same* strict whitelist
// parser the QML layer uses at render time (client/NameStyleParser.js). Running
// the real parser here — instead of a hand-written C++ reimplementation — means
// the accept/reject decision on the receive path can never drift from what the
// UI would actually draw.
//
// NOTE: owns a QJSEngine and is therefore NOT thread-safe. Construct and call it
// from a single thread (in this app: the main/GUI thread that runs the payload
// processor).
class NameStyleValidator {
 public:
  NameStyleValidator();

  // True iff `css` is a valid style per NameStyleParser.parse(). Fails closed:
  // if the parser could not be loaded, everything is treated as invalid.
  bool isValid(const QString& css);

 private:
  QJSEngine m_engine;
  QJSValue m_isValidFn;
};

#endif  // NAMESTYLEVALIDATOR_H
