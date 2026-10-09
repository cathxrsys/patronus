#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVideoFrame>
#include <QVideoSink>

class QImage;
struct quirc;

// QrScanner taps the video frames rendered by a QML VideoOutput (via its
// videoSink) and decodes any QR Code found in them using the bundled quirc
// library. It throttles the work and stops after the first successful decode so
// the UI can pop the scan page. Exposed to QML as Qr.QrScanner.
class QrScanner : public QObject {
  Q_OBJECT
  Q_PROPERTY(
      QVideoSink* videoSink READ videoSink WRITE setVideoSink NOTIFY videoSinkChanged)
  Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

 public:
  explicit QrScanner(QObject* parent = nullptr);
  ~QrScanner() override;

  QVideoSink* videoSink() const { return m_videoSink; }
  void setVideoSink(QVideoSink* sink);

  bool active() const { return m_active; }
  void setActive(bool active);

  // Checks the camera permission and requests it when undetermined. On
  // platforms without a runtime permission model this reports granted
  // immediately.
  Q_INVOKABLE void requestCameraPermission();

 signals:
  void videoSinkChanged();
  void activeChanged();
  void codeScanned(const QString& text);
  void cameraPermissionGranted();
  void cameraPermissionDenied();

 private slots:
  void onVideoFrame(const QVideoFrame& frame);

 private:
  void decodeImage(const QImage& image);
  // Runs finder-pattern detection + decode on whatever is currently in
  // quirc's pixel buffer. Returns true and emits codeScanned() on success.
  bool tryDecodeCurrentBuffer();

  QPointer<QVideoSink> m_videoSink;
  bool m_active = true;
  bool m_haveResult = false;
  quirc* m_quirc = nullptr;
  int m_quircW = 0;
  int m_quircH = 0;
  QElapsedTimer m_throttle;
};
