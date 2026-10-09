#include "voicecliputils.h"

#include <QFile>
#include <QObject>
#include <QtAlgorithms>
#include <QtEndian>

namespace voiceclip {
namespace {

constexpr char kMagic[] = "ARCVOIC1";
constexpr quint8 kVersion = 1;
constexpr int kHeaderSize = 8 + 1 + 4 + 4;

}  // namespace

quint32 durationForFrameCount(int frameCount) {
  if (frameCount <= 0) {
    return 0;
  }

  return static_cast<quint32>(frameCount) * kFrameDurationMs;
}

bool writeClip(const QString& filePath, const QList<QByteArray>& frames,
               quint32 durationMs, QString* errorText) {
  QFile file(filePath);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    if (errorText != nullptr) {
      *errorText = QObject::tr("Failed to create voice message file");
    }
    return false;
  }

  QByteArray header;
  header.reserve(kHeaderSize);
  header.append(kMagic, 8);
  header.append(static_cast<char>(kVersion));

  const quint32 beDuration = qToBigEndian(durationMs);
  const quint32 beFrameCount =
      qToBigEndian(static_cast<quint32>(frames.size()));
  header.append(reinterpret_cast<const char*>(&beDuration), sizeof(beDuration));
  header.append(reinterpret_cast<const char*>(&beFrameCount),
                sizeof(beFrameCount));

  if (file.write(header) != header.size()) {
    if (errorText != nullptr) {
      *errorText = QObject::tr("Failed to write voice message header");
    }
    return false;
  }

  for (const QByteArray& frame : frames) {
    const quint32 beFrameSize =
        qToBigEndian(static_cast<quint32>(frame.size()));
    if (file.write(reinterpret_cast<const char*>(&beFrameSize),
                   sizeof(beFrameSize)) != sizeof(beFrameSize) ||
        file.write(frame) != frame.size()) {
      if (errorText != nullptr) {
        *errorText = QObject::tr("Failed to write voice message data");
      }
      return false;
    }
  }

  return true;
}

bool readClip(const QString& filePath, ClipData* out, QString* errorText) {
  if (out == nullptr) {
    if (errorText != nullptr) {
      *errorText = QObject::tr("Voice message buffer is invalid");
    }
    return false;
  }

  QFile file(filePath);
  if (!file.open(QIODevice::ReadOnly)) {
    if (errorText != nullptr) {
      *errorText = QObject::tr("Failed to open voice message file");
    }
    return false;
  }

  const QByteArray header = file.read(kHeaderSize);
  if (header.size() != kHeaderSize || header.left(8) != QByteArray(kMagic, 8)) {
    if (errorText != nullptr) {
      *errorText = QObject::tr("Voice message file is corrupted");
    }
    return false;
  }

  const quint8 version = static_cast<quint8>(header.at(8));
  if (version != kVersion) {
    if (errorText != nullptr) {
      *errorText = QObject::tr("Voice message format is not supported");
    }
    return false;
  }

  quint32 beDuration = 0;
  quint32 beFrameCount = 0;
  memcpy(&beDuration, header.constData() + 9, sizeof(beDuration));
  memcpy(&beFrameCount, header.constData() + 13, sizeof(beFrameCount));

  out->durationMs = qFromBigEndian(beDuration);
  const quint32 frameCount = qFromBigEndian(beFrameCount);
  out->frames.clear();

  for (quint32 index = 0; index < frameCount; ++index) {
    quint32 beFrameSize = 0;
    if (file.read(reinterpret_cast<char*>(&beFrameSize), sizeof(beFrameSize)) !=
        sizeof(beFrameSize)) {
      if (errorText != nullptr) {
        *errorText = QObject::tr("Voice message data is incomplete");
      }
      out->frames.clear();
      return false;
    }

    const quint32 frameSize = qFromBigEndian(beFrameSize);
    const QByteArray frame = file.read(frameSize);
    if (frame.size() != static_cast<qsizetype>(frameSize)) {
      if (errorText != nullptr) {
        *errorText = QObject::tr("Voice message data is incomplete");
      }
      out->frames.clear();
      return false;
    }

    out->frames.append(frame);
  }

  if (out->durationMs == 0) {
    out->durationMs = durationForFrameCount(out->frames.size());
  }

  return true;
}

QList<int> buildWaveform(const QList<QByteArray>& frames, int barCount) {
  const int resolvedBarCount = qMax(1, barCount);
  QList<int> waveform;
  waveform.reserve(resolvedBarCount);

  if (frames.isEmpty()) {
    for (int index = 0; index < resolvedBarCount; ++index) {
      waveform.append(8);
    }
    return waveform;
  }

  QList<qreal> bucketLevels;
  bucketLevels.reserve(resolvedBarCount);

  const int frameCount = frames.size();
  for (int barIndex = 0; barIndex < resolvedBarCount; ++barIndex) {
    const int startIndex = (barIndex * frameCount) / resolvedBarCount;
    const int endIndex =
        qMax(startIndex + 1, ((barIndex + 1) * frameCount) / resolvedBarCount);

    qreal totalSize = 0.0;
    int samples = 0;
    for (int frameIndex = startIndex;
         frameIndex < endIndex && frameIndex < frameCount; ++frameIndex) {
      totalSize += frames.at(frameIndex).size();
      ++samples;
    }

    bucketLevels.append(samples > 0 ? (totalSize / samples) : 0.0);
  }

  qreal minLevel = bucketLevels.first();
  qreal maxLevel = bucketLevels.first();
  for (qreal level : bucketLevels) {
    minLevel = qMin(minLevel, level);
    maxLevel = qMax(maxLevel, level);
  }

  const qreal levelRange = maxLevel - minLevel;
  if (levelRange <= 0.001) {
    for (int index = 0; index < resolvedBarCount; ++index) {
      waveform.append(18);
    }
    return waveform;
  }

  for (qreal level : bucketLevels) {
    const qreal normalized = (level - minLevel) / levelRange;
    waveform.append(
        qBound(6, static_cast<int>(qRound(10.0 + normalized * 90.0)), 100));
  }

  return waveform;
}

QList<int> buildWaveformFromFile(const QString& filePath, int barCount) {
  ClipData clipData;
  if (!readClip(filePath, &clipData, nullptr)) {
    return {};
  }

  return buildWaveform(clipData.frames, barCount);
}

}  // namespace voiceclip