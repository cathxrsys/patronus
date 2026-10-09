#include "voicecliprecorder.h"

#include <QAudioFormat>
#include <QMediaDevices>
#include <cmath>

#include "audiodevicemanager.h"
#include "audiopermission.h"

VoiceClipRecorder::VoiceClipRecorder(AudioDeviceManager* audioDeviceManager,
                                     QObject* parent)
    : QObject(parent), m_audioDeviceManager(audioDeviceManager) {}

VoiceClipRecorder::~VoiceClipRecorder() {
  stopRecording(false);
  cleanupEncoder();
}

void VoiceClipRecorder::startRecording() {
  if (m_recording) {
    return;
  }

  // Latch the intent to record immediately so a stopRecording() that lands
  // while the permission prompt is still pending can cancel it (see the
  // m_recording check in the callback below).
  m_recording = true;

  audiopermission::ensureMicrophoneAccess(this, [this](bool granted) {
    if (!m_recording) {
      // stopRecording()/cancelRecording() already ran; the user backed out
      // before responding to the permission prompt.
      return;
    }
    if (!granted) {
      m_recording = false;
      emit errorOccurred(
          tr("Microphone access is required to record voice messages. "
             "Enable it in system settings."));
      return;
    }
    startCapture();
  });
}

void VoiceClipRecorder::startCapture() {
  if (!setupEncoder()) {
    m_recording = false;
    return;
  }

  setupAudio();
  if (!m_audioSource) {
    cleanupEncoder();
    m_recording = false;
    return;
  }

  m_pcmBuffer.clear();
  m_pcmFrames.clear();
  m_capturedFrameCount = 0;
  m_audioDevice = m_audioSource->start();
  if (!m_audioDevice) {
    emit errorOccurred(tr("Failed to start audio recording"));
    delete m_audioSource;
    m_audioSource = nullptr;
    cleanupEncoder();
    m_recording = false;
    return;
  }

  connect(m_audioDevice, &QIODevice::readyRead, this,
          &VoiceClipRecorder::onAudioDataReady);
}

void VoiceClipRecorder::stopRecording(bool finalize) {
  if (!m_recording) {
    return;
  }

  if (m_audioSource) {
    m_audioSource->stop();
    delete m_audioSource;
    m_audioSource = nullptr;
  }

  m_audioDevice = nullptr;

  if (finalize) {
    finalizeEncoding();
  } else {
    m_pcmBuffer.clear();
    m_pcmFrames.clear();
    m_capturedFrameCount = 0;
  }

  m_recording = false;

  cleanupEncoder();
}

void VoiceClipRecorder::onAudioDataReady() {
  if (!m_audioDevice || !m_encoder) {
    return;
  }

  m_pcmBuffer.append(m_audioDevice->readAll());

  while (m_pcmBuffer.size() >= BYTES_PER_FRAME) {
    const QByteArray frame = m_pcmBuffer.first(BYTES_PER_FRAME);
    m_pcmBuffer.remove(0, BYTES_PER_FRAME);
    m_pcmFrames.append(frame);
    ++m_capturedFrameCount;

    emit frameLevelCaptured(calculateLevel(frame));
  }
}

void VoiceClipRecorder::setupAudio() {
  QAudioFormat format;
  format.setSampleRate(SAMPLE_RATE);
  format.setChannelCount(CHANNELS);
  format.setSampleFormat(QAudioFormat::Int16);

  const QAudioDevice inputDevice =
      m_audioDeviceManager != nullptr
          ? m_audioDeviceManager->selectedAudioInput()
          : QMediaDevices::defaultAudioInput();

  if (!inputDevice.isFormatSupported(format)) {
    emit errorOccurred(tr("Audio format not supported"));
    return;
  }

  m_audioSource = new QAudioSource(inputDevice, format, this);
  m_audioSource->setBufferSize(BYTES_PER_FRAME * 12);
  connect(m_audioSource, &QAudioSource::stateChanged, this,
          [this](QAudio::State state) {
            if (state == QAudio::StoppedState && m_audioSource &&
                m_audioSource->error() != QAudio::NoError) {
              qWarning() << "[VoiceClipRecorder] Audio source stopped with error:"
                         << m_audioSource->error();
              emit errorOccurred(
                  tr("Audio input error (code %1)").arg(m_audioSource->error()));
            }
          });
}

bool VoiceClipRecorder::setupEncoder() {
  cleanupEncoder();

  int error = OPUS_OK;
  m_encoder = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_AUDIO,
                                  &error);
  if (error != OPUS_OK || !m_encoder) {
    emit errorOccurred(QStringLiteral("Failed to create Opus encoder: %1")
                           .arg(QString::fromUtf8(opus_strerror(error))));
    m_encoder = nullptr;
    return false;
  }

  opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(24000));
  opus_encoder_ctl(m_encoder, OPUS_SET_COMPLEXITY(4));
  opus_encoder_ctl(m_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  opus_encoder_ctl(m_encoder, OPUS_SET_DTX(1));
  opus_encoder_ctl(m_encoder, OPUS_SET_INBAND_FEC(0));
  opus_encoder_ctl(m_encoder, OPUS_SET_PACKET_LOSS_PERC(0));
  return true;
}

void VoiceClipRecorder::cleanupEncoder() {
  if (m_encoder) {
    opus_encoder_destroy(m_encoder);
    m_encoder = nullptr;
  }
}

void VoiceClipRecorder::finalizeEncoding() {
  if (!m_encoder) {
    return;
  }

  if (!m_pcmBuffer.isEmpty()) {
    QByteArray paddedFrame = m_pcmBuffer;
    if (paddedFrame.size() < BYTES_PER_FRAME) {
      paddedFrame.append(
          QByteArray(BYTES_PER_FRAME - paddedFrame.size(), '\0'));
    }
    m_pcmFrames.append(paddedFrame);
    m_pcmBuffer.clear();
  }

  for (const QByteArray& frame : std::as_const(m_pcmFrames)) {
    QByteArray opusData(4000, 0);
    const auto* pcm = reinterpret_cast<const qint16*>(frame.constData());
    const int encodedBytes = opus_encode(
        m_encoder, pcm, FRAME_SIZE,
        reinterpret_cast<unsigned char*>(opusData.data()), opusData.size());

    if (encodedBytes < 0) {
      emit errorOccurred(
          QStringLiteral("Opus encode error: %1")
              .arg(QString::fromUtf8(opus_strerror(encodedBytes))));
      m_pcmFrames.clear();
      return;
    }

    opusData.resize(encodedBytes);
    emit frameEncoded(opusData);
  }

  m_pcmFrames.clear();
  m_capturedFrameCount = 0;
}

qreal VoiceClipRecorder::calculateLevel(const QByteArray& pcmData) const {
  if (pcmData.isEmpty()) {
    return 0.0;
  }

  const auto* samples = reinterpret_cast<const qint16*>(pcmData.constData());
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