#pragma once

#include <opus/opus.h>

#include <QAudioSource>
#include <QIODevice>
#include <QMediaDevices>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QThread>

class AudioDeviceManager;

class OpusEncoderWorker : public QObject {
  Q_OBJECT
 public:
  explicit OpusEncoderWorker(QObject *parent = nullptr);
  ~OpusEncoderWorker();

 public slots:
  void init();
  void encodeFrame(const QByteArray &pcmData);
  void cleanup();

 signals:
  void encodedFrame(const QByteArray &opusData);
  void errorOccurred(const QString &error);

 private:
  OpusEncoder *m_encoder = nullptr;
  static constexpr int SAMPLE_RATE = 48000;
  static constexpr int CHANNELS = 1;
  static constexpr int FRAME_SIZE = 960;  // 20ms @ 48kHz
};

class OpusRecorder : public QObject {
  Q_OBJECT
 public:
  explicit OpusRecorder(AudioDeviceManager *audioDeviceManager = nullptr,
                        QObject *parent = nullptr);
  ~OpusRecorder();

  void startRecording();
  void stopRecording();

  bool isRecording() const { return m_recording; }

 signals:
  void frameEncoded(const QByteArray &opusData);
  void frameLevelCaptured(qreal level);
  void errorOccurred(const QString &error);
  void encodeRequested(const QByteArray &pcmData);

 private slots:
  void onAudioDataReady();
  void onFrameEncoded(const QByteArray &opusData);

 private:
  qreal calculateLevel(const QByteArray &pcmData) const;
  void setupAudio();
  // Actually opens the audio input once microphone access is confirmed.
  void startCapture();

  AudioDeviceManager *m_audioDeviceManager = nullptr;
  QAudioSource *m_audioSource = nullptr;
  QIODevice *m_audioDevice = nullptr;

  QThread m_encoderThread;
  OpusEncoderWorker *m_encoderWorker = nullptr;

  QByteArray m_pcmBuffer;
  bool m_recording = false;

  static constexpr int SAMPLE_RATE = 48000;
  static constexpr int CHANNELS = 1;
  static constexpr int FRAME_SIZE = 960;
  static constexpr int BYTES_PER_FRAME = FRAME_SIZE * CHANNELS * sizeof(qint16);
};