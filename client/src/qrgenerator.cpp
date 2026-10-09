#include "qrgenerator.h"

#include <QPainter>

extern "C" {
#include "qrcodegen.h"
}

QrImageItem::QrImageItem(QQuickItem* parent) : QQuickPaintedItem(parent) {
  // QR modules are hard squares; keep them crisp instead of smoothed.
  setAntialiasing(false);
  setSmooth(false);
}

void QrImageItem::setText(const QString& text) {
  if (m_text == text) {
    return;
  }
  m_text = text;
  emit textChanged();
  regenerate();
}

void QrImageItem::setForeground(const QColor& color) {
  if (m_foreground == color) {
    return;
  }
  m_foreground = color;
  emit foregroundChanged();
  update();
}

void QrImageItem::setBackground(const QColor& color) {
  if (m_background == color) {
    return;
  }
  m_background = color;
  emit backgroundChanged();
  update();
}

void QrImageItem::setQuietZone(int modules) {
  const int clamped = qMax(0, modules);
  if (m_quietZone == clamped) {
    return;
  }
  m_quietZone = clamped;
  emit quietZoneChanged();
  update();
}

void QrImageItem::regenerate() {
  const bool wasValid = m_valid;
  m_valid = false;
  m_size = 0;

  const QByteArray utf8 = m_text.toUtf8();
  if (!utf8.isEmpty()) {
    QByteArray qrcode(qrcodegen_BUFFER_LEN_MAX, Qt::Uninitialized);
    QByteArray temp(qrcodegen_BUFFER_LEN_MAX, Qt::Uninitialized);
    const bool ok = qrcodegen_encodeText(
        utf8.constData(), reinterpret_cast<uint8_t*>(temp.data()),
        reinterpret_cast<uint8_t*>(qrcode.data()), qrcodegen_Ecc_MEDIUM,
        qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO,
        /*boostEcl=*/true);
    if (ok) {
      m_qrcode = qrcode;
      m_size = qrcodegen_getSize(
          reinterpret_cast<const uint8_t*>(m_qrcode.constData()));
      m_valid = m_size > 0;
    }
  }

  if (!m_valid) {
    m_qrcode.clear();
  }

  if (m_valid != wasValid) {
    emit validChanged();
  }

  update();
}

void QrImageItem::paint(QPainter* painter) {
  const QRectF bounds(0, 0, width(), height());

  // The background is painted across the whole item so the mandatory quiet
  // zone around the code stays part of the styled surface.
  painter->fillRect(bounds, m_background);

  if (!m_valid || m_size <= 0) {
    return;
  }

  const int totalModules = m_size + 2 * m_quietZone;
  if (totalModules <= 0) {
    return;
  }

  // Snap the module size to whole pixels so every square is identical and the
  // code stays sharp at any item size.
  const qreal available = qMin(width(), height());
  const int moduleSize = static_cast<int>(available / totalModules);
  if (moduleSize <= 0) {
    return;
  }

  const int drawnSize = moduleSize * totalModules;
  const qreal offsetX = (width() - drawnSize) / 2.0;
  const qreal offsetY = (height() - drawnSize) / 2.0;

  painter->setRenderHint(QPainter::Antialiasing, false);
  painter->setPen(Qt::NoPen);
  painter->setBrush(m_foreground);

  const uint8_t* qr = reinterpret_cast<const uint8_t*>(m_qrcode.constData());
  for (int y = 0; y < m_size; ++y) {
    for (int x = 0; x < m_size; ++x) {
      if (!qrcodegen_getModule(qr, x, y)) {
        continue;
      }
      const qreal px = offsetX + (x + m_quietZone) * moduleSize;
      const qreal py = offsetY + (y + m_quietZone) * moduleSize;
      painter->drawRect(QRectF(px, py, moduleSize, moduleSize));
    }
  }
}
