#include "opusplayer.h"

#include <QAudioFormat>
#include <QDebug>

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#endif

namespace {

int clampTargetBufferMs(int targetBufferMs) { return qMax(20, targetBufferMs); }

int minimumBufferedMsForTarget(int targetBufferMs) {
  return qMin(60, qMax(20, targetBufferMs / 2));
}

int maximumBufferedMsForTarget(int targetBufferMs) {
  return qMax(targetBufferMs + 100, targetBufferMs * 2);
}

}  // namespace

// ============ OpusDecoderWorker ============

OpusDecoderWorker::OpusDecoderWorker(QObject *parent) : QObject(parent) {}

OpusDecoderWorker::~OpusDecoderWorker() { cleanup(); }

void OpusDecoderWorker::init() {
  int error;
  m_decoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &error);

  if (error != OPUS_OK) {
    emit errorOccurred(QString("Failed to create Opus decoder:  %1")
                           .arg(opus_strerror(error)));
    return;
  }

  qDebug() << "[OpusPlayer] [init] Decoder initialized";
}

void OpusDecoderWorker::decodeFrame(const QByteArray &opusData) {
  if (!m_decoder) return;

  std::vector<qint16> pcmBuffer(MAX_FRAME_SIZE * CHANNELS);

  int samplesDecoded = opus_decode(
      m_decoder, reinterpret_cast<const unsigned char *>(opusData.constData()),
      opusData.size(), pcmBuffer.data(), MAX_FRAME_SIZE, 0);

  if (samplesDecoded < 0) {
    emit errorOccurred(
        QString("Opus decode error: %1").arg(opus_strerror(samplesDecoded)));
    return;
  }

  QByteArray pcmData(reinterpret_cast<const char *>(pcmBuffer.data()),
                     samplesDecoded * CHANNELS * sizeof(qint16));

  emit decodedFrame(pcmData);
}

void OpusDecoderWorker::cleanup() {
  if (m_decoder) {
    opus_decoder_destroy(m_decoder);
    m_decoder = nullptr;
  }
}

AudioOutputBuffer::AudioOutputBuffer(QObject *parent) : QIODevice(parent) {
  open(QIODevice::ReadOnly);
}

void AudioOutputBuffer::addPcmData(const QByteArray &data) {
  QMutexLocker locker(&m_mutex);
  m_buffer.append(data);
}

void AudioOutputBuffer::clear() {
  QMutexLocker locker(&m_mutex);
  m_buffer.clear();
}

int AudioOutputBuffer::bufferedSize() const {
  QMutexLocker locker(&m_mutex);
  return m_buffer.size();
}

qint64 AudioOutputBuffer::bytesAvailable() const {
  QMutexLocker locker(&m_mutex);
  // On Android, QAudioSink checks bytesAvailable() before calling readData().
  // If it returns 0 the sink enters IdleState and stops reading entirely.
  // Always return a non-zero value so the sink stays Active; readData() pads
  // with silence when the actual buffer is empty.
  return qMax(m_buffer.size() + QIODevice::bytesAvailable(), (qint64)65536);
}

qint64 AudioOutputBuffer::readData(char *data, qint64 maxlen) {
  QMutexLocker locker(&m_mutex);

  static int s_readCount = 0;
  if (++s_readCount % 200 == 1) {
    qDebug() << "[AudioOutputBuffer] readData #" << s_readCount
             << "maxlen:" << maxlen << "bufSize:" << m_buffer.size();
  }

  if (m_buffer.isEmpty()) {
    memset(data, 0, maxlen);
    return maxlen;
  }

  qint64 bytesToRead = qMin(maxlen, static_cast<qint64>(m_buffer.size()));
  memcpy(data, m_buffer.constData(), bytesToRead);
  m_buffer.remove(0, bytesToRead);

  if (bytesToRead < maxlen) {
    memset(data + bytesToRead, 0, maxlen - bytesToRead);
  }

  return maxlen;
}

qint64 AudioOutputBuffer::writeData(const char *data, qint64 len) {
  Q_UNUSED(data);
  Q_UNUSED(len);
  return -1;
}

JitterBuffer::JitterBuffer(int targetBufferMs, QObject *parent)
    : QObject(parent),
      m_targetBufferMs(clampTargetBufferMs(targetBufferMs)),
      m_minBufferMs(minimumBufferedMsForTarget(m_targetBufferMs)),
      m_maxBufferMs(maximumBufferedMsForTarget(m_targetBufferMs)) {}

void JitterBuffer::push(const QByteArray &pcmData) {
  QMutexLocker locker(&m_mutex);

  int currentMs = m_frames.size() * m_frameMs;

  // Check for overflow: if the buffer is too large, drop old frames
  if (currentMs >= m_maxBufferMs) {
    // Drop old frames, keeping only the target amount
    int framesToKeep = m_targetBufferMs / m_frameMs;
    int framesToDrop = m_frames.size() - framesToKeep;

    qDebug() << "[JitterBuffer] Buffer overflow! Dropping" << framesToDrop
             << "old frames (" << (framesToDrop * m_frameMs) << "ms)";

    for (int i = 0; i < framesToDrop && !m_frames.isEmpty(); ++i) {
      m_frames.dequeue();
    }

    m_consecutiveOverruns++;
    m_consecutiveUnderruns = 0;
  } else {
    m_consecutiveOverruns = 0;
  }

  m_frames.enqueue(pcmData);
  currentMs = m_frames.size() * m_frameMs;

  // Start playback once the minimum buffer is reached
  if (currentMs >= m_minBufferMs && !m_bufferingComplete) {
    m_bufferingComplete = true;
    qDebug() << "[JitterBuffer] Buffering complete at" << currentMs << "ms";
  }

  // // Periodic log
  // if (m_frames.size() % 25 == 0) {
  //     qDebug() << "[JitterBuffer] Buffer:" << currentMs << "ms, target:" <<
  //     m_targetBufferMs << "ms";
  // }
}

QByteArray JitterBuffer::pop() {
  QMutexLocker locker(&m_mutex);

  if (!m_bufferingComplete) {
    return QByteArray();
  }

  if (m_frames.isEmpty()) {
    m_consecutiveUnderruns++;

    // If there are many underruns in a row, increase the minimum buffer
    if (m_consecutiveUnderruns > 3) {
      m_minBufferMs = qMin(m_minBufferMs + 20, 200);
      qDebug() << "[JitterBuffer] Underrun! Increasing min buffer to"
               << m_minBufferMs << "ms";
      m_bufferingComplete = false;  // Wait for the buffer to fill again
      m_consecutiveUnderruns = 0;
    }

    return QByteArray();
  }

  m_consecutiveUnderruns = 0;

  int currentMs = m_frames.size() * m_frameMs;

  // Adaptive catch-up: if the buffer exceeds the target, skip frames
  if (currentMs > m_targetBufferMs + 100) {
    // Skip 1 frame when needed to catch up
    int excessMs = currentMs - m_targetBufferMs;
    if (excessMs > 60 && m_frames.size() > 2) {
      m_frames.dequeue();  // Skip an old frame
      qDebug() << "[JitterBuffer] Catching up, skipped 1 frame.  Buffer:"
               << (m_frames.size() * m_frameMs) << "ms";
    }
  }

  // If the buffer stays large, the minimum threshold can be reduced
  if (currentMs > m_targetBufferMs && m_consecutiveOverruns == 0) {
    m_minBufferMs = qMax(m_minBufferMs - 5, 40);
  }

  return m_frames.dequeue();
}

void JitterBuffer::clear() {
  QMutexLocker locker(&m_mutex);
  m_frames.clear();
  m_bufferingComplete = false;
  m_consecutiveUnderruns = 0;
  m_consecutiveOverruns = 0;
  m_minBufferMs = 60;  // Reset to the initial value
}

void JitterBuffer::setBufferSize(int ms) {
  QMutexLocker locker(&m_mutex);
  m_targetBufferMs = clampTargetBufferMs(ms);
  m_minBufferMs = minimumBufferedMsForTarget(m_targetBufferMs);
  m_maxBufferMs = maximumBufferedMsForTarget(m_targetBufferMs);
}

int JitterBuffer::bufferedMs() const {
  QMutexLocker locker(&m_mutex);
  return m_frames.size() * m_frameMs;
}

bool JitterBuffer::isReady() const {
  QMutexLocker locker(&m_mutex);
  return m_bufferingComplete;
}

OpusPlayer::OpusPlayer(int jitterBufferMs, QObject *parent)
    : OpusPlayer(jitterBufferMs, 100, parent) {}

OpusPlayer::OpusPlayer(int jitterBufferMs, int outputBufferMs, QObject *parent)
    : QObject(parent),
      m_audioBuffer(new AudioOutputBuffer(this)),
      m_jitterBuffer(new JitterBuffer(jitterBufferMs, this)),
      m_outputBufferMs(qMax(20, outputBufferMs)) {
  m_decoderWorker = new OpusDecoderWorker();
  m_decoderWorker->moveToThread(&m_decoderThread);

  connect(&m_decoderThread, &QThread::started, m_decoderWorker,
          &OpusDecoderWorker::init);
  connect(&m_decoderThread, &QThread::finished, m_decoderWorker,
          &QObject::deleteLater);
  connect(this, &OpusPlayer::decodeRequested, m_decoderWorker,
          &OpusDecoderWorker::decodeFrame);
  connect(m_decoderWorker, &OpusDecoderWorker::decodedFrame, this,
          &OpusPlayer::onFrameDecoded);
  connect(m_decoderWorker, &OpusDecoderWorker::errorOccurred, this,
          &OpusPlayer::errorOccurred);

  m_decoderThread.start();

  m_playbackTimer = new QTimer(this);
  m_playbackTimer->setTimerType(Qt::PreciseTimer);
  connect(m_playbackTimer, &QTimer::timeout, this,
          &OpusPlayer::onPlaybackTimer);
}

OpusPlayer::~OpusPlayer() {
  stop();
  m_decoderThread.quit();
  m_decoderThread.wait();
}

void OpusPlayer::setupAudio() {
#ifdef Q_OS_ANDROID
  m_voicePlayer = QJniObject("org/patronus/client/VoiceCallAudioPlayer");
  if (!m_voicePlayer.isValid()) {
    qWarning() << "[OpusPlayer] Failed to create VoiceCallAudioPlayer";
    return;
  }
  bool ok = m_voicePlayer.callMethod<jboolean>("init");
  qDebug() << "[OpusPlayer] VoiceCallAudioPlayer init:" << ok;
  if (!ok) {
    m_voicePlayer = QJniObject();
  }
#else
  const auto devices = QMediaDevices::audioOutputs();
  qDebug() << "[OpusPlayer] Available audio output devices:";
  for (const QAudioDevice &dev : devices) {
    qDebug() << " -" << dev.description() << dev.id();
  }

  QAudioFormat format;
  format.setSampleRate(SAMPLE_RATE);
  format.setChannelCount(CHANNELS);
  format.setSampleFormat(QAudioFormat::Int16);

  QAudioDevice outputDevice = QMediaDevices::defaultAudioOutput();
  qDebug() << "[OpusPlayer] Default audio output device:"
           << outputDevice.description() << outputDevice.id();

  if (!outputDevice.isFormatSupported(format)) {
    qWarning() << "[OpusPlayer] Default device does not support format, trying others";
    for (const QAudioDevice &dev : devices) {
      if (dev.isFormatSupported(format)) {
        qDebug() << "[OpusPlayer] Fallback audio output device:" << dev.description();
        outputDevice = dev;
        break;
      }
    }
  }

  if (!outputDevice.isFormatSupported(format)) {
    emit errorOccurred("Audio output format not supported");
    return;
  }

  m_audioSink = new QAudioSink(outputDevice, format, this);
  m_audioSink->setBufferSize(
      (SAMPLE_RATE * CHANNELS * sizeof(qint16) * m_outputBufferMs) / 1000);
  qDebug() << "[OpusPlayer] QAudioSink created for device:" << outputDevice.description()
           << "id:" << outputDevice.id()
           << "state:" << m_audioSink->state()
           << "error:" << m_audioSink->error();
#endif
}

void OpusPlayer::start() {
  if (m_playing) return;

  setupAudio();

#ifdef Q_OS_ANDROID
  if (!m_voicePlayer.isValid()) return;
  m_jitterBuffer->clear();
  m_voicePlayer.callMethod<void>("start");
  m_playbackTimer->start(20);
  m_playing = true;
  qDebug() << "[OpusPlayer] [start] VoiceCallAudioPlayer started";
#else
  if (!m_audioSink) return;
  m_jitterBuffer->clear();
  m_audioBuffer->clear();
  m_audioSink->start(m_audioBuffer);
  qDebug() << "[OpusPlayer] [start] AudioSink state after start:"
           << m_audioSink->state() << "error:" << m_audioSink->error()
           << "bufferSize:" << m_audioSink->bufferSize()
           << "bytesFree:" << m_audioSink->bytesFree();
  m_playbackTimer->start(20);
  m_playing = true;
  qDebug() << "[OpusPlayer] [start] Playback started";
#endif
}

void OpusPlayer::stop() {
  if (!m_playing) return;

  m_playbackTimer->stop();

#ifdef Q_OS_ANDROID
  if (m_voicePlayer.isValid()) {
    m_voicePlayer.callMethod<void>("release");
    m_voicePlayer = QJniObject();
  }
#else
  if (m_audioSink) {
    m_audioSink->stop();
    delete m_audioSink;
    m_audioSink = nullptr;
  }
  m_audioBuffer->clear();
#endif

  m_jitterBuffer->clear();
  m_playing = false;
  qDebug() << "[OpusPlayer] [stop] Playback stopped";
}

void OpusPlayer::playFrame(const QByteArray &opusData) {
  emit decodeRequested(opusData);
}

void OpusPlayer::setJitterBufferSize(int ms) {
  m_jitterBuffer->setBufferSize(ms);
}

void OpusPlayer::onFrameDecoded(const QByteArray &pcmData) {
  static int s_decodedCount = 0;
  ++s_decodedCount;

  qreal rawLevel = calculateVoiceLevel(pcmData);
  emit voiceLevelChanged(rawLevel);

  m_jitterBuffer->push(pcmData);

  if (s_decodedCount % 50 == 1) {
    qDebug() << "[OpusPlayer] Decoded frame #" << s_decodedCount
             << "pcmSize:" << pcmData.size()
             << "jitterMs:" << m_jitterBuffer->bufferedMs()
             << "jitterReady:" << m_jitterBuffer->isReady();
  }
}

void OpusPlayer::onPlaybackTimer() {
  static int s_timerCount = 0;
  if (++s_timerCount % 100 == 1) {
    qDebug() << "[OpusPlayer] Timer #" << s_timerCount
             << "sinkState:" << (m_audioSink ? (int)m_audioSink->state() : -1)
             << "sinkError:" << (m_audioSink ? (int)m_audioSink->error() : -1)
             << "jitterMs:" << m_jitterBuffer->bufferedMs()
             << "jitterReady:" << m_jitterBuffer->isReady()
             << "audioBufBytes:" << m_audioBuffer->bufferedSize();
  }

  if (!m_jitterBuffer->isReady()) {
    return;
  }

  QByteArray pcmData = m_jitterBuffer->pop();
  if (pcmData.isEmpty()) return;

#ifdef Q_OS_ANDROID
  if (m_voicePlayer.isValid()) {
    QJniEnvironment env;
    jbyteArray jArray = env->NewByteArray(pcmData.size());
    env->SetByteArrayRegion(jArray, 0, pcmData.size(),
        reinterpret_cast<const jbyte*>(pcmData.constData()));
    m_voicePlayer.callMethod<void>("write", "([B)V", jArray);
    env->DeleteLocalRef(jArray);
    env.checkAndClearExceptions();
  }
#else
  m_audioBuffer->addPcmData(pcmData);
#endif
}

qreal OpusPlayer::calculateVoiceLevel(const QByteArray &pcmData) {
  if (pcmData.isEmpty()) return 0.0;

  const qint16 *samples = reinterpret_cast<const qint16 *>(pcmData.constData());
  int sampleCount = pcmData.size() / sizeof(qint16);

  if (sampleCount == 0) return 0.0;

  // Compute RMS (Root Mean Square), a standard audio measurement
  qint64 sumSquares = 0;
  for (int i = 0; i < sampleCount; ++i) {
    qint64 sample = samples[i];
    sumSquares += sample * sample;
  }

  qreal rms = std::sqrt(static_cast<qreal>(sumSquares) / sampleCount);

  // Normalize to the 0.0 - 1.0 range
  // INT16_MAX = 32767, but real speech rarely reaches the maximum
  // Use ~20% of the maximum as the "loud speech" reference
  constexpr qreal SPEECH_REFERENCE = 5000.0;  // ~20%

  qreal normalizedLevel = qMin(1.0, rms / SPEECH_REFERENCE);

  return normalizedLevel;
}