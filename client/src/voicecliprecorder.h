#pragma once

#include <opus/opus.h>

#include <QAudioSource>
#include <QIODevice>
#include <QObject>

class AudioDeviceManager;

class VoiceClipRecorder : public QObject {
  Q_OBJECT

 public:
  explicit VoiceClipRecorder(AudioDeviceManager* audioDeviceManager = nullptr,
                             QObject* parent = nullptr);
  ~VoiceClipRecorder() override;

  void startRecording();
  void stopRecording(bool finalize = true);
  bool isRecording() const { return m_recording; }

 signals:
  void frameEncoded(const QByteArray& opusData);
  void frameLevelCaptured(qreal level);
  void errorOccurred(const QString& error);

 private slots:
  void onAudioDataReady();

 private:
  // Actually opens the encoder and audio input once microphone access is
  // confirmed.
  void startCapture();
  void setupAudio();
  bool setupEncoder();
  void cleanupEncoder();
  void finalizeEncoding();
  qreal calculateLevel(const QByteArray& pcmData) const;

  AudioDeviceManager* m_audioDeviceManager = nullptr;
  QAudioSource* m_audioSource = nullptr;
  QIODevice* m_audioDevice = nullptr;
  OpusEncoder* m_encoder = nullptr;
  QByteArray m_pcmBuffer;
  QList<QByteArray> m_pcmFrames;
  bool m_recording = false;
  int m_capturedFrameCount = 0;

  static constexpr int SAMPLE_RATE = 48000;
  static constexpr int CHANNELS = 1;
  static constexpr int FRAME_SIZE = 960;
  static constexpr int BYTES_PER_FRAME = FRAME_SIZE * CHANNELS * sizeof(qint16);
};