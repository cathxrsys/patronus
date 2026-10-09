#include "voicemessagemanager.h"

#include <QDateTime>

#include "apppaths.h"
#include "audiodevicemanager.h"
#include "filetransfermanager.h"
#include "voicecliputils.h"

VoiceMessageManager::VoiceMessageManager(
    FileTransferManager* fileTransferManager,
    AudioDeviceManager* audioDeviceManager, QObject* parent)
    : QObject(parent),
      m_fileTransferManager(fileTransferManager),
      m_recorder(audioDeviceManager, this),
      m_player(this) {
  connect(&m_recorder, &VoiceClipRecorder::frameEncoded, this,
          &VoiceMessageManager::onFrameEncoded);
  connect(&m_recorder, &VoiceClipRecorder::frameLevelCaptured, this,
          &VoiceMessageManager::onFrameLevelCaptured);
  connect(&m_recorder, &VoiceClipRecorder::errorOccurred, this,
          &VoiceMessageManager::onRecorderError);
  connect(&m_player, &VoiceClipPlayer::errorOccurred, this,
          &VoiceMessageManager::onPlayerError);
  connect(&m_player, &VoiceClipPlayer::positionChanged, this,
          &VoiceMessageManager::onPlayerPositionChanged);
  connect(&m_player, &VoiceClipPlayer::playbackFinished, this,
          &VoiceMessageManager::finishPlayback);
}

bool VoiceMessageManager::isRecording() const {
  return m_recorder.isRecording();
}

int VoiceMessageManager::recordingDurationMs() const {
  return m_recordingDurationMs;
}

QVariantList VoiceMessageManager::recordingLevelBars() const {
  QVariantList bars;
  bars.reserve(m_recordingLevelBars.size());
  for (int value : m_recordingLevelBars) {
    bars.append(value);
  }
  return bars;
}

bool VoiceMessageManager::isPlaying() const {
  return m_playingMessageId != 0 && !m_playbackPaused;
}

bool VoiceMessageManager::isPlaybackPaused() const {
  return m_playingMessageId != 0 && m_playbackPaused;
}

quint64 VoiceMessageManager::playingMessageId() const {
  return m_playingMessageId;
}

int VoiceMessageManager::playbackPositionMs() const {
  return m_playbackPositionMs;
}

int VoiceMessageManager::playbackDurationMs() const {
  return m_playbackDurationMs;
}

void VoiceMessageManager::startRecording() {
  if (m_fileTransferManager == nullptr) {
    emit errorOccurred(tr("Voice messages are unavailable"));
    return;
  }

  if (isRecording()) {
    return;
  }

  stopPlayback();
  m_recordedFrames.clear();
  m_recordedFrameLevels.clear();
  m_recentRecordingLevels.clear();
  m_recordingLevelBars.clear();
  m_recordingDurationMs = 0;
  m_recordingUiFrameCounter = 0;
  emit recordingDurationChanged();
  emit recordingLevelBarsChanged();
  m_recorder.startRecording();
  emit recordingChanged();
}

void VoiceMessageManager::cancelRecording() {
  if (!isRecording()) {
    return;
  }

  m_recorder.stopRecording(false);
  resetRecordingState();
  emit recordingChanged();
}

void VoiceMessageManager::stopRecordingAndSend(
    const QString& serverId, const QString& contactPubKey, quint64 replyTo,
    const QString& replyPreview, const QString& replyKind, bool replyIsOwn) {
  if (!isRecording()) {
    return;
  }

  m_recorder.stopRecording(true);
  emit recordingChanged();

  if (serverId.isEmpty() || contactPubKey.isEmpty()) {
    resetRecordingState();
    emit errorOccurred(tr("Choose a chat before sending a voice message"));
    return;
  }

  if (m_recordedFrames.isEmpty() || m_recordingDurationMs <= 0) {
    resetRecordingState();
    emit errorOccurred(tr("Voice message is too short"));
    return;
  }

  const QString recordingPath = buildRecordingPath();
  QString errorText;
  if (!voiceclip::writeClip(recordingPath, m_recordedFrames,
                            static_cast<quint32>(m_recordingDurationMs),
                            &errorText)) {
    resetRecordingState();
    emit errorOccurred(errorText);
    return;
  }

  const int durationMs = m_recordingDurationMs;
  const QList<int> waveform = buildWaveform();
  resetRecordingState();
  m_fileTransferManager->sendAudioMessage(
      serverId, contactPubKey, recordingPath, static_cast<quint64>(durationMs),
      waveform, ReplyInfo{replyTo, replyPreview, replyKind, replyIsOwn});
}

void VoiceMessageManager::togglePlayback(quint64 messageId,
                                         const QString& localPath) {
  if (messageId == 0 || localPath.isEmpty()) {
    emit errorOccurred(tr("Voice message file is unavailable"));
    return;
  }

  if (m_playingMessageId == messageId) {
    if (isPlaying()) {
      pausePlayback();
    } else if (isPlaybackPaused()) {
      resumePlayback();
    }
    return;
  }

  if (!loadPlaybackClip(messageId, localPath)) {
    return;
  }

  m_playbackPaused = true;
  resumePlayback();
}

void VoiceMessageManager::seekPlayback(quint64 messageId,
                                       const QString& localPath,
                                       qreal positionRatio) {
  if (messageId == 0 || localPath.isEmpty()) {
    emit errorOccurred(tr("Voice message file is unavailable"));
    return;
  }

  const qreal boundedRatio = qBound<qreal>(0.0, positionRatio, 1.0);
  const bool isCurrentMessage = (m_playingMessageId == messageId);
  bool shouldResume = false;

  if (!isCurrentMessage) {
    if (!loadPlaybackClip(messageId, localPath)) {
      return;
    }
    shouldResume = true;
    m_playbackPaused = true;
  } else {
    shouldResume = isPlaying();
    m_player.stop();
  }

  m_player.seek(boundedRatio);
  m_playbackPositionMs = m_player.positionMs();
  emit playingStateChanged();

  if (shouldResume) {
    resumePlayback();
  }
}

bool VoiceMessageManager::loadPlaybackClip(quint64 messageId,
                                           const QString& localPath) {
  stopPlayback();

  QString errorText;
  if (!m_player.loadClip(localPath, &errorText)) {
    emit errorOccurred(errorText);
    return false;
  }

  m_playbackDurationMs = m_player.durationMs();
  m_playbackPositionMs = 0;
  m_playingMessageId = messageId;
  return true;
}

void VoiceMessageManager::pausePlayback() {
  if (!isPlaying()) {
    return;
  }

  m_player.pause();
  m_playbackPaused = true;
  m_playbackPositionMs = m_player.positionMs();

  emit playingChanged();
  emit playingStateChanged();
}

void VoiceMessageManager::resumePlayback() {
  if (m_playingMessageId == 0 || !m_player.hasClip()) {
    return;
  }

  const bool wasPlaying = isPlaying();
  m_player.play();
  m_playbackPaused = false;
  m_playbackPositionMs = m_player.positionMs();

  if (!wasPlaying) {
    emit playingChanged();
  }
  emit playingStateChanged();
}

void VoiceMessageManager::stopPlayback() {
  if (m_playingMessageId == 0) {
    return;
  }

  finishPlayback();
}

void VoiceMessageManager::onFrameEncoded(const QByteArray& opusFrame) {
  if (opusFrame.isEmpty()) {
    return;
  }

  m_recordedFrames.append(opusFrame);
}

void VoiceMessageManager::onFrameLevelCaptured(qreal level) {
  if (!isRecording()) {
    return;
  }

  constexpr qreal kNoiseGate = 0.02;
  constexpr qreal kReferenceFloor = 0.12;
  constexpr qreal kReferenceDecay = 0.985;

  const qreal gatedLevel = level >= kNoiseGate ? level : 0.0;

  m_recordedFrameLevels.append(gatedLevel);
  const int nextDurationMs = static_cast<int>(
      voiceclip::durationForFrameCount(m_recordedFrameLevels.size()));
  if (m_recordingDurationMs != nextDurationMs) {
    m_recordingDurationMs = nextDurationMs;
    emit recordingDurationChanged();
  }

  m_recentRecordingLevels.append(gatedLevel);
  constexpr int kLiveBarCount = 48;
  while (m_recentRecordingLevels.size() > kLiveBarCount) {
    m_recentRecordingLevels.removeFirst();
  }

  if (gatedLevel > m_recordingLevelReference) {
    m_recordingLevelReference = gatedLevel;
  } else {
    m_recordingLevelReference =
        qMax(kReferenceFloor, m_recordingLevelReference * kReferenceDecay);
  }

  m_recordingUiFrameCounter += 1;
  if (m_recordingUiFrameCounter >= 2) {
    m_recordingUiFrameCounter = 0;
    updateRecordingLevelBars();
  }
}

void VoiceMessageManager::onRecorderError(const QString& errorText) {
  resetRecordingState();
  emit recordingChanged();
  emit errorOccurred(errorText);
}

void VoiceMessageManager::onPlayerError(const QString& errorText) {
  finishPlayback();
  emit errorOccurred(errorText);
}

void VoiceMessageManager::onPlayerPositionChanged(int positionMs) {
  const int boundedPosition = qBound(0, positionMs, m_playbackDurationMs);
  if (m_playbackPositionMs != boundedPosition) {
    m_playbackPositionMs = boundedPosition;
    if (m_playingMessageId != 0) {
      emit playingStateChanged();
    }
  }
}

QString VoiceMessageManager::buildRecordingPath() const {
  const QString directoryPath = apppaths::filesDirectory();

  return directoryPath + QStringLiteral("/voice_") +
         QString::number(QDateTime::currentMSecsSinceEpoch()) +
         QString::fromUtf8(voiceclip::kDefaultExtension);
}

void VoiceMessageManager::finishPlayback() {
  const bool hadPlayback = (m_playingMessageId != 0);
  const bool wasPlaying = isPlaying();
  m_player.stop();
  m_playingMessageId = 0;
  m_playbackPaused = false;
  m_playbackPositionMs = 0;
  m_playbackDurationMs = 0;

  if (wasPlaying) {
    emit playingChanged();
  }
  if (hadPlayback) {
    emit playingStateChanged();
  }
}

void VoiceMessageManager::resetRecordingState() {
  m_recordedFrames.clear();
  m_recordedFrameLevels.clear();
  m_recentRecordingLevels.clear();
  m_recordingLevelBars.clear();
  m_recordingLevelReference = 0.12;
  m_recordingDurationMs = 0;
  m_recordingUiFrameCounter = 0;
  emit recordingDurationChanged();
  emit recordingLevelBarsChanged();
}

QList<int> VoiceMessageManager::buildWaveform() const {
  constexpr int kBarCount = 90;
  QList<int> waveform;
  waveform.reserve(kBarCount);

  if (m_recordedFrameLevels.isEmpty()) {
    for (int index = 0; index < kBarCount; ++index) {
      waveform.append(8);
    }
    return waveform;
  }

  QList<qreal> bucketLevels;
  bucketLevels.reserve(kBarCount);
  const int frameCount = m_recordedFrameLevels.size();
  for (int barIndex = 0; barIndex < kBarCount; ++barIndex) {
    const int startIndex = (barIndex * frameCount) / kBarCount;
    const int endIndex =
        qMax(startIndex + 1, ((barIndex + 1) * frameCount) / kBarCount);

    qreal totalLevel = 0.0;
    int samples = 0;
    for (int frameIndex = startIndex;
         frameIndex < endIndex && frameIndex < frameCount; ++frameIndex) {
      totalLevel += m_recordedFrameLevels.at(frameIndex);
      ++samples;
    }

    bucketLevels.append(samples > 0 ? (totalLevel / samples) : 0.0);
  }

  qreal minLevel = bucketLevels.first();
  qreal peakLevel = bucketLevels.first();
  for (qreal level : bucketLevels) {
    minLevel = qMin(minLevel, level);
    peakLevel = qMax(peakLevel, level);
  }

  const qreal levelRange = peakLevel - minLevel;
  if (levelRange <= 0.0001) {
    for (int index = 0; index < kBarCount; ++index) {
      waveform.append(18);
    }
    return waveform;
  }

  for (qreal level : bucketLevels) {
    const qreal normalizedLevel = (level - minLevel) / levelRange;
    const int normalized =
        qBound(6, static_cast<int>(qRound(10.0 + normalizedLevel * 90.0)), 100);
    waveform.append(normalized);
  }

  return waveform;
}

void VoiceMessageManager::updateRecordingLevelBars() {
  constexpr int kLiveBarCount = 48;
  constexpr qreal kReferenceFloor = 0.12;

  QList<int> bars;
  bars.reserve(kLiveBarCount);

  const qreal safeReferenceLevel =
      qMax(kReferenceFloor, m_recordingLevelReference);
  const int leadingEmptyBars =
      qMax(0, kLiveBarCount - m_recentRecordingLevels.size());
  for (int index = 0; index < leadingEmptyBars; ++index) {
    bars.append(6);
  }

  for (qreal level : m_recentRecordingLevels) {
    const qreal normalizedLevel =
        qBound<qreal>(0.0, level / safeReferenceLevel, 1.0);
    bars.append(
        qBound(6, static_cast<int>(qRound(8.0 + normalizedLevel * 92.0)), 100));
  }

  if (m_recordingLevelBars == bars) {
    return;
  }

  m_recordingLevelBars = bars;
  emit recordingLevelBarsChanged();
}