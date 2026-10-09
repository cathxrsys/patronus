#pragma once

#include <opus/opus.h>

#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QThread>
#include <QTimer>

#ifdef Q_OS_ANDROID
#include <QJniObject>
#endif

class OpusDecoderWorker : public QObject {
  Q_OBJECT
 public:
  explicit OpusDecoderWorker(QObject *parent = nullptr);
  ~OpusDecoderWorker();

 public slots:
  void init();
  void decodeFrame(const QByteArray &opusData);
  void cleanup();

 signals:
  void decodedFrame(const QByteArray &pcmData);
  void errorOccurred(const QString &error);

 private:
  OpusDecoder *m_decoder = nullptr;
  static constexpr int SAMPLE_RATE = 48000;
  static constexpr int CHANNELS = 1;
  static constexpr int MAX_FRAME_SIZE = 5760;
};

class AudioOutputBuffer : public QIODevice {
  Q_OBJECT
 public:
  explicit AudioOutputBuffer(QObject *parent = nullptr);

  void addPcmData(const QByteArray &data);
  void clear();
  int bufferedSize() const;

  qint64 bytesAvailable() const override;
  bool isSequential() const override { return true; }

 protected:
  qint64 readData(char *data, qint64 maxlen) override;
  qint64 writeData(const char *data, qint64 len) override;

 private:
  QByteArray m_buffer;
  mutable QMutex m_mutex;
};

class JitterBuffer : public QObject {
  Q_OBJECT
 public:
  explicit JitterBuffer(int targetBufferMs = 150, QObject *parent = nullptr);

  void push(const QByteArray &pcmData);
  QByteArray pop();
  void clear();

  void setBufferSize(int ms);
  int bufferedMs() const;
  bool isReady() const;

 private:
  QQueue<QByteArray> m_frames;
  mutable QMutex m_mutex;
  int m_targetBufferMs;  // Target buffer size
  int m_minBufferMs = 60;  // Minimum size before playback starts
  int m_maxBufferMs = 300;  // Maximum size before reset
  int m_frameMs = 20;       // 20ms per frame
  bool m_bufferingComplete = false;

  int m_consecutiveUnderruns = 0;  // Underrun counter
  int m_consecutiveOverruns = 0;  // Overflow counter
};

class OpusPlayer : public QObject {
  Q_OBJECT
 public:
  explicit OpusPlayer(int jitterBufferMs, QObject *parent);
  explicit OpusPlayer(int jitterBufferMs = 150, int outputBufferMs = 100,
                      QObject *parent = nullptr);
  ~OpusPlayer();

  void start();
  void stop();
  void playFrame(const QByteArray &opusData);

  void setJitterBufferSize(int ms);
  bool isPlaying() const { return m_playing; }

  qreal voiceLevel() const { return m_voiceLevel; }

 signals:
  void errorOccurred(const QString &error);
  void decodeRequested(const QByteArray &opusData);
  void voiceLevelChanged(qreal level);

 private slots:
  void onFrameDecoded(const QByteArray &pcmData);
  void onPlaybackTimer();

 private:
  void setupAudio();
  qreal calculateVoiceLevel(const QByteArray &pcmData);  // New method

  QAudioSink *m_audioSink = nullptr;
  AudioOutputBuffer *m_audioBuffer = nullptr;
  JitterBuffer *m_jitterBuffer = nullptr;

#ifdef Q_OS_ANDROID
  QJniObject m_voicePlayer;
#endif

  QThread m_decoderThread;
  OpusDecoderWorker *m_decoderWorker = nullptr;

  QTimer *m_playbackTimer = nullptr;

  bool m_playing = false;

  static constexpr int SAMPLE_RATE = 48000;
  static constexpr int CHANNELS = 1;
  int m_outputBufferMs = 100;

  qreal m_voiceLevel = 0.0;

  static constexpr qreal SMOOTHING_FACTOR = 0.3;  // unused
};