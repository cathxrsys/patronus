#include "opusrecorder.h"

#include <QAudioFormat>
#include <QDebug>
#include <cmath>

#include "audiodevicemanager.h"
#include "audiopermission.h"

OpusEncoderWorker::OpusEncoderWorker(QObject *parent) : QObject(parent) {}

OpusEncoderWorker::~OpusEncoderWorker() { cleanup(); }

void OpusEncoderWorker::init() {
  int error;
  m_encoder =
      opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_VOIP, &error);

  if (error != OPUS_OK) {
    emit errorOccurred(
        QString("Failed to create Opus encoder: %1").arg(opus_strerror(error)));
    return;
  }

  // Settings for VoIP
  opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(32000));
  opus_encoder_ctl(m_encoder, OPUS_SET_COMPLEXITY(7));
  opus_encoder_ctl(m_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  opus_encoder_ctl(m_encoder, OPUS_SET_DTX(1));
  opus_encoder_ctl(m_encoder, OPUS_SET_INBAND_FEC(1));
  opus_encoder_ctl(m_encoder, OPUS_SET_PACKET_LOSS_PERC(10));

  qDebug() << "[OpusRecorder] [init] Encoder initialized";
}

void OpusEncoderWorker::encodeFrame(const QByteArray &pcmData) {
  if (!m_encoder) return;

  const qint16 *pcm = reinterpret_cast<const qint16 *>(pcmData.constData());

  QByteArray opusData(4000, 0);

  int encodedBytes = opus_encode(
      m_encoder, pcm, FRAME_SIZE,
      reinterpret_cast<unsigned char *>(opusData.data()), opusData.size());

  if (encodedBytes < 0) {
    emit errorOccurred(
        QString("Opus encode error: %1").arg(opus_strerror(encodedBytes)));
    return;
  }

  opusData.resize(encodedBytes);
  emit encodedFrame(opusData);
}

void OpusEncoderWorker::cleanup() {
  if (m_encoder) {
    opus_encoder_destroy(m_encoder);
    m_encoder = nullptr;
  }
}

OpusRecorder::OpusRecorder(AudioDeviceManager *audioDeviceManager,
                           QObject *parent)
    : QObject(parent), m_audioDeviceManager(audioDeviceManager) {
  m_encoderWorker = new OpusEncoderWorker();
  m_encoderWorker->moveToThread(&m_encoderThread);

  connect(&m_encoderThread, &QThread::started, m_encoderWorker,
          &OpusEncoderWorker::init);
  connect(&m_encoderThread, &QThread::finished, m_encoderWorker,
          &QObject::deleteLater);
  connect(this, &OpusRecorder::encodeRequested, m_encoderWorker,
          &OpusEncoderWorker::encodeFrame);
  connect(m_encoderWorker, &OpusEncoderWorker::encodedFrame, this,
          &OpusRecorder::onFrameEncoded);
  connect(m_encoderWorker, &OpusEncoderWorker::errorOccurred, this,
          &OpusRecorder::errorOccurred);

  m_encoderThread.start();
}

OpusRecorder::~OpusRecorder() {
  stopRecording();
  m_encoderThread.quit();
  m_encoderThread.wait();
}

void OpusRecorder::setupAudio() {
  QAudioFormat format;
  format.setSampleRate(SAMPLE_RATE);
  format.setChannelCount(CHANNELS);
  format.setSampleFormat(QAudioFormat::Int16);

  const QAudioDevice inputDevice =
      m_audioDeviceManager != nullptr
          ? m_audioDeviceManager->selectedAudioInput()
          : QMediaDevices::defaultAudioInput();
  qDebug() << "[OpusRecorder] [setupAudio] Selected audio input device:"
           << inputDevice.description() << inputDevice.id();

  if (!inputDevice.isFormatSupported(format)) {
    emit errorOccurred("Audio format not supported");
    return;
  }

  m_audioSource = new QAudioSource(inputDevice, format, this);
  m_audioSource->setBufferSize(BYTES_PER_FRAME * 4);
  connect(m_audioSource, &QAudioSource::stateChanged, this,
          [this](QAudio::State state) {
            if (state == QAudio::StoppedState && m_audioSource &&
                m_audioSource->error() != QAudio::NoError) {
              qWarning() << "[OpusRecorder] Audio source stopped with error:"
                         << m_audioSource->error();
              emit errorOccurred(
                  QString("Audio input error (code %1)").arg(m_audioSource->error()));
            }
          });
}

void OpusRecorder::startRecording() {
  if (m_recording) return;

  // Latch the intent to record immediately so a stopRecording() that lands
  // while the permission prompt is still pending can cancel it (see the
  // m_recording check in the callback below).
  m_recording = true;

  audiopermission::ensureMicrophoneAccess(this, [this](bool granted) {
    if (!m_recording) {
      // stopRecording() already ran; the call ended before the user
      // responded to the permission prompt.
      return;
    }
    if (!granted) {
      m_recording = false;
      emit errorOccurred(
          tr("Microphone access is required for calls. Enable it in system "
             "settings."));
      return;
    }
    startCapture();
  });
}

void OpusRecorder::startCapture() {
  setupAudio();
  if (!m_audioSource) {
    m_recording = false;
    return;
  }

  m_pcmBuffer.clear();
  m_audioDevice = m_audioSource->start();

  connect(m_audioDevice, &QIODevice::readyRead, this,
          &OpusRecorder::onAudioDataReady);

  qDebug() << "[OpusRecorder] [startRecording] Recording started";
}

void OpusRecorder::stopRecording() {
  if (!m_recording) return;

  if (m_audioSource) {
    m_audioSource->stop();
    delete m_audioSource;
    m_audioSource = nullptr;
  }

  m_audioDevice = nullptr;
  m_pcmBuffer.clear();
  m_recording = false;

  qDebug() << "[OpusRecorder] [stopRecording] Recording stopped";
}

void OpusRecorder::onAudioDataReady() {
  if (!m_audioDevice) return;

  m_pcmBuffer.append(m_audioDevice->readAll());

  while (m_pcmBuffer.size() >= BYTES_PER_FRAME) {
    QByteArray frame = m_pcmBuffer.left(BYTES_PER_FRAME);
    m_pcmBuffer.remove(0, BYTES_PER_FRAME);
    emit frameLevelCaptured(calculateLevel(frame));
    emit encodeRequested(frame);
  }
}

void OpusRecorder::onFrameEncoded(const QByteArray &opusData) {
  emit frameEncoded(opusData);
}

qreal OpusRecorder::calculateLevel(const QByteArray &pcmData) const {
  if (pcmData.isEmpty()) {
    return 0.0;
  }

  const qint16 *samples = reinterpret_cast<const qint16 *>(pcmData.constData());
  const int sampleCount = pcmData.size() / static_cast<int>(sizeof(qint16));
  if (sampleCount <= 0) {
    return 0.0;
  }

  qint64 sumSquares = 0;
  for (int index = 0; index < sampleCount; ++index) {
    const qint64 sample = samples[index];
    sumSquares += sample * sample;
  }

  const qreal rms = std::sqrt(static_cast<qreal>(sumSquares) /
                              static_cast<qreal>(sampleCount));
  constexpr qreal kSpeechReference = 5000.0;
  return qBound<qreal>(0.0, rms / kSpeechReference, 1.0);
}