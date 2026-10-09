#include "voiceclipplayer.h"

#include <QAudioFormat>
#include <QMediaDevices>
#include <vector>

#include "voicecliputils.h"

VoiceClipPlayer::VoiceClipPlayer(QObject* parent) : QObject(parent) {
  m_pcmBufferDevice.setBuffer(&m_pcmData);
  m_pcmBufferDevice.open(QIODevice::ReadOnly);

  m_positionTimer.setInterval(80);
  connect(&m_positionTimer, &QTimer::timeout, this,
          &VoiceClipPlayer::onPositionTimer);
}

VoiceClipPlayer::~VoiceClipPlayer() {
  stop();
  cleanupDecoder();
}

bool VoiceClipPlayer::loadClip(const QString& localPath, QString* errorText) {
  voiceclip::ClipData clipData;
  if (!voiceclip::readClip(localPath, &clipData, errorText)) {
    return false;
  }

  stop();
  cleanupDecoder();
  m_pcmData.clear();
  m_durationMs = static_cast<int>(clipData.durationMs);

  int error = OPUS_OK;
  m_decoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &error);
  if (error != OPUS_OK || !m_decoder) {
    const QString message = QStringLiteral("Failed to create Opus decoder: %1")
                                .arg(QString::fromUtf8(opus_strerror(error)));
    if (errorText) {
      *errorText = message;
    }
    emit errorOccurred(message);
    cleanupDecoder();
    return false;
  }

  std::vector<qint16> pcmFrame(FRAME_SIZE * CHANNELS * 6);
  for (const QByteArray& opusFrame : clipData.frames) {
    const int samplesDecoded = opus_decode(
        m_decoder,
        reinterpret_cast<const unsigned char*>(opusFrame.constData()),
        opusFrame.size(), pcmFrame.data(), FRAME_SIZE * 6, 0);

    if (samplesDecoded < 0) {
      const QString message =
          QStringLiteral("Opus decode error: %1")
              .arg(QString::fromUtf8(opus_strerror(samplesDecoded)));
      if (errorText) {
        *errorText = message;
      }
      emit errorOccurred(message);
      stop();
      cleanupDecoder();
      m_pcmData.clear();
      m_durationMs = 0;
      return false;
    }

    m_pcmData.append(reinterpret_cast<const char*>(pcmFrame.data()),
                     samplesDecoded * CHANNELS * BYTES_PER_SAMPLE);
  }

  if (m_durationMs <= 0) {
    m_durationMs = static_cast<int>(
        voiceclip::durationForFrameCount(clipData.frames.size()));
  }

  resetPlaybackBuffer();
  m_started = false;
  emit positionChanged(0);
  return true;
}

void VoiceClipPlayer::play() {
  if (!hasClip()) {
    return;
  }

  if (!ensureAudioSink()) {
    return;
  }

  if (!m_started) {
    m_playbackStartByte = m_pcmBufferDevice.pos();
    m_audioSink->start(&m_pcmBufferDevice);
    m_started = true;
  } else if (m_audioSink->state() == QAudio::SuspendedState) {
    m_audioSink->resume();
  } else if (m_audioSink->state() == QAudio::StoppedState ||
             m_audioSink->state() == QAudio::IdleState) {
    restartSinkAtCurrentPosition(true);
  }

  m_positionTimer.start();
  emit positionChanged(positionMs());
}

void VoiceClipPlayer::pause() {
  if (!m_audioSink || m_audioSink->state() != QAudio::ActiveState) {
    return;
  }

  m_audioSink->suspend();
  m_positionTimer.stop();
  emit positionChanged(positionMs());
}

void VoiceClipPlayer::stop() {
  m_positionTimer.stop();
  m_idlePolls = 0;

  if (m_audioSink) {
    m_audioSink->stop();
    delete m_audioSink;
    m_audioSink = nullptr;
  }

  resetPlaybackBuffer();
  m_started = false;
  emit positionChanged(0);
}

void VoiceClipPlayer::seek(qreal positionRatio) {
  if (!hasClip()) {
    return;
  }

  const qreal boundedRatio = qBound<qreal>(0.0, positionRatio, 1.0);
  // alignedBytePosition works in clip-relative bytes; offset past the leading
  // silence to address the actual playback buffer.
  const int targetBytePosition = alignedBytePosition(
      static_cast<int>(qRound(boundedRatio * m_pcmData.size())));
  const bool resumeAfterSeek = isPlaying();

  m_pcmBufferDevice.seek(LEAD_IN_BYTES + targetBytePosition);
  restartSinkAtCurrentPosition(resumeAfterSeek);
  emit positionChanged(positionMs());
}

bool VoiceClipPlayer::isPlaying() const {
  return m_audioSink && m_audioSink->state() == QAudio::ActiveState;
}

bool VoiceClipPlayer::hasClip() const {
  return !m_pcmData.isEmpty() && m_durationMs > 0;
}

int VoiceClipPlayer::durationMs() const { return m_durationMs; }

int VoiceClipPlayer::positionMs() const {
  if (!hasClip()) {
    return 0;
  }

  const int bytesPerMs = bytesPerMillisecond();
  if (bytesPerMs <= 0) {
    return 0;
  }

  // While the sink is running, derive the position from the audio actually
  // processed (played) rather than from how far ahead the pull-mode buffer has
  // been read. Otherwise the reported position — and the end-of-clip check —
  // would lead real playback by the whole sink buffer, which on Android is
  // large (see ensureAudioSink) and would race ahead and clip the tail. When
  // not actively playing (paused/seeked/stopped) the buffer position is exact.
  qint64 playedBytes;
  if (m_audioSink != nullptr && m_started) {
    const qint64 bytesPerSec =
        static_cast<qint64>(SAMPLE_RATE) * CHANNELS * BYTES_PER_SAMPLE;
    const qint64 processedBytes =
        m_audioSink->processedUSecs() * bytesPerSec / 1000000;
    playedBytes = m_playbackStartByte + processedBytes;
  } else {
    playedBytes = currentBufferPosition();
  }

  // playedBytes is in playback-buffer coordinates (which start with the leading
  // silence); the clip-relative position drops that lead-in. It is negative
  // while the silence is still playing, which qBound clamps back to 0.
  const qint64 clipBytes = playedBytes - LEAD_IN_BYTES;
  return qBound(0, static_cast<int>(clipBytes / bytesPerMs), m_durationMs);
}

void VoiceClipPlayer::onPositionTimer() {
  emit positionChanged(positionMs());

  if (m_audioSink == nullptr || !m_started || !hasClip()) {
    return;
  }

  // The source has been fully pulled into the sink once the read cursor reaches
  // the end (equivalently, the sink reports IdleState). At that point up to one
  // sink buffer of audio is still draining to the speaker, so we must NOT finish
  // immediately — finishPlayback stops the sink and would clip the tail (the
  // larger Android buffer made this audible). Finish once the *played* position
  // reaches the end; as a defensive fallback (should processedUSecs plateau on
  // some backend), also finish after enough polls for the buffer to have drained.
  const bool sourceDrained =
      currentBufferPosition() >= totalPlaybackBytes() ||
      m_audioSink->state() == QAudio::IdleState;
  if (!sourceDrained) {
    m_idlePolls = 0;
    return;
  }

  ++m_idlePolls;
  const int bytesPerMs = qMax(1, bytesPerMillisecond());
  const int intervalMs = qMax(1, m_positionTimer.interval());
  const int drainPolls = m_audioSink->bufferSize() / bytesPerMs / intervalMs + 3;
  if (positionMs() >= m_durationMs || m_idlePolls >= drainPolls) {
    m_positionTimer.stop();
    m_idlePolls = 0;
    emit positionChanged(m_durationMs);
    emit playbackFinished();
  }
}

void VoiceClipPlayer::onAudioStateChanged(QtAudio::State state) {
  // Backstop for when the source is exhausted and the audio has already fully
  // played (e.g. a tiny buffer): the per-tick check in onPositionTimer handles
  // the normal case where the buffer still has a tail to drain.
  if (state == QAudio::IdleState && hasClip() && positionMs() >= m_durationMs) {
    m_positionTimer.stop();
    emit positionChanged(m_durationMs);
    emit playbackFinished();
  }
}

void VoiceClipPlayer::cleanupDecoder() {
  if (m_decoder) {
    opus_decoder_destroy(m_decoder);
    m_decoder = nullptr;
  }
}

bool VoiceClipPlayer::ensureAudioSink() {
  if (m_audioSink) {
    return true;
  }

  QAudioFormat format;
  format.setSampleRate(SAMPLE_RATE);
  format.setChannelCount(CHANNELS);
  format.setSampleFormat(QAudioFormat::Int16);

  const QAudioDevice outputDevice = QMediaDevices::defaultAudioOutput();
  if (!outputDevice.isFormatSupported(format)) {
    const QString message = tr("Audio output format not supported");
    emit errorOccurred(message);
    return false;
  }

  m_audioSink = new QAudioSink(outputDevice, format, this);
  // A small pull-mode buffer is fine on desktop, but on Android the first
  // playback after the sink is created cold-starts the underlying AudioTrack:
  // with only ~100 ms buffered the pipeline under-runs during warm-up and the
  // opening of the clip comes out silent (seeking back later plays it because
  // the AudioTrack is warm by then). A larger buffer gives the cold start
  // enough runway that the first samples are not dropped.
#if defined(Q_OS_ANDROID)
  m_audioSink->setBufferSize(BYTES_PER_FRAME * 25);  // ~500 ms
#else
  m_audioSink->setBufferSize(BYTES_PER_FRAME * 5);  // ~100 ms
#endif
  connect(m_audioSink, &QAudioSink::stateChanged, this,
          &VoiceClipPlayer::onAudioStateChanged);
  return true;
}

void VoiceClipPlayer::restartSinkAtCurrentPosition(bool startPlayback) {
  if (!ensureAudioSink()) {
    return;
  }

  m_positionTimer.stop();
  m_audioSink->stop();
  m_started = false;
  m_idlePolls = 0;

  if (startPlayback) {
    m_playbackStartByte = m_pcmBufferDevice.pos();
    m_audioSink->start(&m_pcmBufferDevice);
    m_started = true;
    m_positionTimer.start();
  }
}

int VoiceClipPlayer::bytesPerMillisecond() const {
  return (SAMPLE_RATE * CHANNELS * BYTES_PER_SAMPLE) / 1000;
}

int VoiceClipPlayer::alignedBytePosition(int bytePosition) const {
  const int frameAlignment = CHANNELS * BYTES_PER_SAMPLE;
  const int boundedPosition = qBound(0, bytePosition, m_pcmData.size());
  return boundedPosition - (boundedPosition % frameAlignment);
}

int VoiceClipPlayer::currentBufferPosition() const {
  return qBound(0, static_cast<int>(m_pcmBufferDevice.pos()),
                totalPlaybackBytes());
}

int VoiceClipPlayer::totalPlaybackBytes() const {
  return LEAD_IN_BYTES + m_pcmData.size();
}

void VoiceClipPlayer::resetPlaybackBuffer() {
  // The device serves LEAD_IN_BYTES of leading silence followed by the decoded
  // PCM. A fresh play() starts at offset 0 so the sink's cold-start under-run
  // (Android) consumes the silence rather than the opening of the clip; see
  // LEAD_IN_BYTES. On desktop the lead-in is 0 and this is a plain reset.
  QByteArray playback;
  playback.reserve(LEAD_IN_BYTES + m_pcmData.size());
  if (LEAD_IN_BYTES > 0) {
    playback.append(LEAD_IN_BYTES, '\0');
  }
  playback.append(m_pcmData);

  m_pcmBufferDevice.close();
  m_pcmBufferDevice.setData(playback);
  m_pcmBufferDevice.open(QIODevice::ReadOnly);
  m_pcmBufferDevice.seek(0);
}