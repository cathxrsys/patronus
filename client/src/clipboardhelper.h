#pragma once

#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QObject>
#include <QtQml>

class ClipboardHelper : public QObject {
  Q_OBJECT
 public:
  explicit ClipboardHelper(QObject* parent = nullptr) : QObject(parent) {}

  Q_INVOKABLE void setText(const QString& text) {
    QGuiApplication::clipboard()->setText(text, QClipboard::Clipboard);
  }

  Q_INVOKABLE QString getText() const {
    return QGuiApplication::clipboard()->text(QClipboard::Clipboard);
  }

  // Loads an image file and puts it on the clipboard. Used as the desktop
  // fallback for "Share QR" where there is no system share sheet.
  Q_INVOKABLE bool setImageFromFile(const QString& path) {
    QImage image(path);
    if (image.isNull()) {
      return false;
    }
    QGuiApplication::clipboard()->setImage(image, QClipboard::Clipboard);
    return true;
  }
};