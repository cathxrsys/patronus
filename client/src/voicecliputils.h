#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace voiceclip {

constexpr quint32 kFrameDurationMs = 20;
constexpr const char* kDefaultMimeType = "application/x-core-voice-opus";
constexpr const char* kDefaultExtension = ".corevoice";

struct ClipData {
  QList<QByteArray> frames;
  quint32 durationMs = 0;
};

quint32 durationForFrameCount(int frameCount);
bool writeClip(const QString& filePath, const QList<QByteArray>& frames,
               quint32 durationMs, QString* errorText = nullptr);
bool readClip(const QString& filePath, ClipData* out,
              QString* errorText = nullptr);
QList<int> buildWaveform(const QList<QByteArray>& frames, int barCount = 90);
QList<int> buildWaveformFromFile(const QString& filePath, int barCount = 90);

}  // namespace voiceclip