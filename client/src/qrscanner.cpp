#include "qrscanner.h"

#include <QImage>
#include <QVideoFrame>
#include <QVideoSink>

#include <cstring>

#if defined(Q_OS_ANDROID) || defined(Q_OS_MACOS) || defined(Q_OS_IOS)
#include <QCoreApplication>
#include <QPermissions>
#endif

extern "C" {
#include "quirc.h"
}

namespace {
// Decode at most a few frames per second; QR recognition is comparatively heavy
// and the camera delivers far more frames than we need.
constexpr int kThrottleMs = 120;
// Large frames are downscaled before decoding to keep each pass cheap while
// still leaving enough detail to resolve the code.
constexpr int kMaxDimension = 640;
}  // namespace

QrScanner::QrScanner(QObject* parent) : QObject(parent) {
  m_quirc = quirc_new();
  m_throttle.start();
}

QrScanner::~QrScanner() {
  if (m_quirc != nullptr) {
    quirc_destroy(m_quirc);
  }
}

void QrScanner::setVideoSink(QVideoSink* sink) {
  if (m_videoSink == sink) {
    return;
  }
  if (m_videoSink != nullptr) {
    disconnect(m_videoSink, nullptr, this, nullptr);
  }
  m_videoSink = sink;
  if (m_videoSink != nullptr) {
    connect(m_videoSink, &QVideoSink::videoFrameChanged, this,
            &QrScanner::onVideoFrame);
  }
  emit videoSinkChanged();
}

void QrScanner::setActive(bool active) {
  if (m_active == active) {
    return;
  }
  m_active = active;
  if (m_active) {
    m_haveResult = false;
    m_throttle.restart();
  }
  emit activeChanged();
}

void QrScanner::requestCameraPermission() {
#if defined(Q_OS_ANDROID) || defined(Q_OS_MACOS) || defined(Q_OS_IOS)
  auto* application = QCoreApplication::instance();
  if (application == nullptr) {
    emit cameraPermissionDenied();
    return;
  }

  QCameraPermission permission;
  switch (application->checkPermission(permission)) {
    case Qt::PermissionStatus::Granted:
      emit cameraPermissionGranted();
      return;
    case Qt::PermissionStatus::Denied:
      emit cameraPermissionDenied();
      return;
    case Qt::PermissionStatus::Undetermined:
      application->requestPermission(
          permission, this, [this](const QPermission& result) {
            if (result.status() == Qt::PermissionStatus::Granted) {
              emit cameraPermissionGranted();
            } else {
              emit cameraPermissionDenied();
            }
          });
      return;
  }
#else
  emit cameraPermissionGranted();
#endif
}

void QrScanner::onVideoFrame(const QVideoFrame& frame) {
  if (!m_active || m_haveResult) {
    return;
  }
  if (!frame.isValid()) {
    return;
  }
  if (m_throttle.elapsed() < kThrottleMs) {
    return;
  }
  m_throttle.restart();

  QImage image = frame.toImage();
  if (image.isNull()) {
    return;
  }

  if (image.width() > kMaxDimension || image.height() > kMaxDimension) {
    image = image.scaled(kMaxDimension, kMaxDimension, Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
  }

  decodeImage(image);
}

void QrScanner::decodeImage(const QImage& imageIn) {
  if (m_quirc == nullptr) {
    return;
  }

  const QImage gray = imageIn.convertToFormat(QImage::Format_Grayscale8);
  const int w = gray.width();
  const int h = gray.height();
  if (w <= 0 || h <= 0) {
    return;
  }

  if (w != m_quircW || h != m_quircH) {
    if (quirc_resize(m_quirc, w, h) < 0) {
      return;
    }
    m_quircW = w;
    m_quircH = h;
  }

  int qw = 0;
  int qh = 0;
  uint8_t* buffer = quirc_begin(m_quirc, &qw, &qh);
  if (buffer == nullptr) {
    return;
  }
  // quirc's buffer is tightly packed (stride == qw); QImage scanlines may be
  // padded, so copy row by row.
  for (int y = 0; y < qh; ++y) {
    std::memcpy(buffer + static_cast<qsizetype>(y) * qw, gray.constScanLine(y),
                qw);
  }

  if (tryDecodeCurrentBuffer()) {
    return;
  }

  // quirc's Otsu threshold always treats the darker side as the foreground,
  // so it never finds a code rendered as light modules on a dark field (e.g.
  // a themed QR whose accent colour is lighter than the background). quirc
  // binarizes q->image in place during quirc_end() (its pixel buffer aliases
  // the image buffer), so the grayscale data is gone by now -- re-copy from
  // our own QImage, inverted this time, and retry once.
  buffer = quirc_begin(m_quirc, &qw, &qh);
  if (buffer == nullptr) {
    return;
  }
  for (int y = 0; y < qh; ++y) {
    const uint8_t* src = gray.constScanLine(y);
    uint8_t* dst = buffer + static_cast<qsizetype>(y) * qw;
    for (int x = 0; x < qw; ++x) {
      dst[x] = 255 - src[x];
    }
  }
  tryDecodeCurrentBuffer();
}

bool QrScanner::tryDecodeCurrentBuffer() {
  quirc_end(m_quirc);

  const int count = quirc_count(m_quirc);
  for (int i = 0; i < count; ++i) {
    struct quirc_code code;
    struct quirc_data data;
    quirc_extract(m_quirc, i, &code);
    if (quirc_decode(&code, &data) != QUIRC_SUCCESS) {
      // Retry mirrored: a front-facing camera produces flipped codes.
      quirc_flip(&code);
      if (quirc_decode(&code, &data) != QUIRC_SUCCESS) {
        continue;
      }
    }
    const QString payload = QString::fromUtf8(
        reinterpret_cast<const char*>(data.payload), data.payload_len);
    if (payload.isEmpty()) {
      continue;
    }
    m_haveResult = true;
    emit codeScanned(payload);
    return true;
  }
  return false;
}
