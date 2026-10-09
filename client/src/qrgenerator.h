#pragma once

#include <QByteArray>
#include <QColor>
#include <QQuickPaintedItem>
#include <QString>

// QrImageItem renders the payload string as a QR Code, styled with arbitrary
// foreground/background colours so it can follow the active theme. It is a
// QQuickPaintedItem so it repaints crisply whenever the text, colours or size
// change. Exposed to QML as Qr.QrImageItem.
class QrImageItem : public QQuickPaintedItem {
  Q_OBJECT
  Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
  Q_PROPERTY(
      QColor foreground READ foreground WRITE setForeground NOTIFY foregroundChanged)
  Q_PROPERTY(
      QColor background READ background WRITE setBackground NOTIFY backgroundChanged)
  Q_PROPERTY(int quietZone READ quietZone WRITE setQuietZone NOTIFY quietZoneChanged)
  Q_PROPERTY(bool valid READ isValid NOTIFY validChanged)

 public:
  explicit QrImageItem(QQuickItem* parent = nullptr);

  QString text() const { return m_text; }
  void setText(const QString& text);

  QColor foreground() const { return m_foreground; }
  void setForeground(const QColor& color);

  QColor background() const { return m_background; }
  void setBackground(const QColor& color);

  int quietZone() const { return m_quietZone; }
  void setQuietZone(int modules);

  bool isValid() const { return m_valid; }

  void paint(QPainter* painter) override;

 signals:
  void textChanged();
  void foregroundChanged();
  void backgroundChanged();
  void quietZoneChanged();
  void validChanged();

 private:
  void regenerate();

  QString m_text;
  QColor m_foreground = QColor(Qt::black);
  QColor m_background = QColor(Qt::white);
  int m_quietZone = 4;
  bool m_valid = false;

  QByteArray m_qrcode;  // raw qrcodegen output buffer
  int m_size = 0;       // module count per side (QR matrix, without quiet zone)
};
