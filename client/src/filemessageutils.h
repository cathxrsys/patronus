#pragma once

#include <QByteArray>
#include <QFileInfo>
#include <QList>
#include <QString>
#include <QStringList>

#include "nlohmann/json.hpp"

struct FileMessageData {
  QString fileId;
  QString fileName;
  quint64 fileSize = 0;
  QString fileSha256;
  QString fileKey;
  QString localPath;
  QString encryption;
  quint64 blobSize = 0;
  QString mediaType;
  QString mimeType;
  quint64 durationMs = 0;
  QList<int> waveform;
};

// An album groups several files/media under one message with an optional
// caption. It is persisted in the message `content` BLOB as
// {"caption": "...", "items": [ <file-json>, ... ]}.
struct AlbumMessageData {
  QString caption;
  QList<FileMessageData> items;
};

namespace filemessage {

inline quint64 readUInt64(const nlohmann::json& json, const char* key) {
  if (!json.contains(key)) {
    return 0;
  }

  const auto& value = json.at(key);
  if (value.is_number_unsigned()) {
    return value.get<quint64>();
  }
  if (value.is_number_integer()) {
    return static_cast<quint64>(value.get<qint64>());
  }
  if (value.is_string()) {
    return QString::fromStdString(value.get<std::string>()).toULongLong();
  }

  return 0;
}

inline QList<int> readIntList(const nlohmann::json& json, const char* key) {
  QList<int> result;
  if (!json.contains(key) || !json.at(key).is_array()) {
    return result;
  }

  for (const auto& value : json.at(key)) {
    if (value.is_number_integer()) {
      result.append(value.get<int>());
    } else if (value.is_number_unsigned()) {
      result.append(static_cast<int>(value.get<unsigned int>()));
    }
  }

  return result;
}

inline nlohmann::json toJson(const FileMessageData& data) {
  nlohmann::json json;
  json["file_id"] = data.fileId.toStdString();
  json["file_name"] = data.fileName.toStdString();
  json["file_size"] = static_cast<uint64_t>(data.fileSize);
  json["file_sha256"] = data.fileSha256.toStdString();
  json["file_key"] = data.fileKey.toStdString();
  json["local_path"] = data.localPath.toStdString();
  json["encryption"] = data.encryption.toStdString();
  json["blob_size"] = static_cast<uint64_t>(data.blobSize);
  json["media_type"] = data.mediaType.toStdString();
  json["mime_type"] = data.mimeType.toStdString();
  json["duration_ms"] = static_cast<uint64_t>(data.durationMs);
  json["waveform"] = nlohmann::json::array();
  for (int value : data.waveform) {
    json["waveform"].push_back(value);
  }
  return json;
}

inline void fromJson(const nlohmann::json& json, FileMessageData* out) {
  if (out == nullptr) {
    return;
  }
  out->fileId = QString::fromStdString(json.value("file_id", std::string()));
  out->fileName =
      QString::fromStdString(json.value("file_name", std::string()));
  out->fileSize = readUInt64(json, "file_size");
  out->fileSha256 =
      QString::fromStdString(json.value("file_sha256", std::string()));
  out->fileKey = QString::fromStdString(json.value("file_key", std::string()));
  out->localPath =
      QString::fromStdString(json.value("local_path", std::string()));
  out->encryption =
      QString::fromStdString(json.value("encryption", std::string()));
  out->blobSize = readUInt64(json, "blob_size");
  out->mediaType =
      QString::fromStdString(json.value("media_type", std::string()));
  out->mimeType =
      QString::fromStdString(json.value("mime_type", std::string()));
  out->durationMs = readUInt64(json, "duration_ms");
  out->waveform = readIntList(json, "waveform");
}

inline QByteArray serialize(const FileMessageData& data) {
  return QByteArray::fromStdString(toJson(data).dump());
}

inline bool deserialize(const QByteArray& raw, FileMessageData* out) {
  if (out == nullptr || raw.isEmpty()) {
    return false;
  }

  const auto json = nlohmann::json::parse(raw.constData(), nullptr, false);
  if (json.is_discarded() || !json.is_object()) {
    return false;
  }

  fromJson(json, out);
  return true;
}

inline QByteArray serializeAlbum(const AlbumMessageData& album) {
  nlohmann::json json;
  json["caption"] = album.caption.toStdString();
  json["items"] = nlohmann::json::array();
  for (const FileMessageData& item : album.items) {
    json["items"].push_back(toJson(item));
  }
  return QByteArray::fromStdString(json.dump());
}

inline bool deserializeAlbum(const QByteArray& raw, AlbumMessageData* out) {
  if (out == nullptr || raw.isEmpty()) {
    return false;
  }

  const auto json = nlohmann::json::parse(raw.constData(), nullptr, false);
  if (json.is_discarded() || !json.is_object()) {
    return false;
  }

  out->caption = QString::fromStdString(json.value("caption", std::string()));
  out->items.clear();
  if (json.contains("items") && json.at("items").is_array()) {
    for (const auto& itemJson : json.at("items")) {
      if (!itemJson.is_object()) {
        continue;
      }
      FileMessageData item;
      fromJson(itemJson, &item);
      out->items.append(item);
    }
  }
  return true;
}

// File-name extension fallback. Some messages arrive with no media_type/
// mime_type (older messages, other clients, or a file sent "as document"), so
// classifying purely on those fields leaves a plain .png/.jpg/.mp4 stuck as a
// generic file — no thumbnail, and it won't open in the media viewer. Trusting
// the extension recovers those. The lists mirror detectMediaType() in
// filetransfermanager.cpp; keep them in sync.
inline QString fileNameSuffix(const FileMessageData& data) {
  return QFileInfo(data.fileName).suffix().toLower();
}

inline bool suffixIsImage(const QString& suffix) {
  static const QStringList kImage = {
      QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
      QStringLiteral("bmp"), QStringLiteral("gif"), QStringLiteral("webp")};
  return kImage.contains(suffix);
}

inline bool suffixIsVideo(const QString& suffix) {
  static const QStringList kVideo = {
      QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("webm"),
      QStringLiteral("avi"), QStringLiteral("mov"), QStringLiteral("m4v")};
  return kVideo.contains(suffix);
}

inline bool isAudioMessage(const FileMessageData& data) {
  return data.mediaType == QStringLiteral("audio") ||
         data.mimeType.startsWith(QStringLiteral("audio/"));
}

inline bool isImageMessage(const FileMessageData& data) {
  if (data.mediaType == QStringLiteral("image") ||
      data.mimeType.startsWith(QStringLiteral("image/"))) {
    return true;
  }
  // Don't override an explicit audio/video classification with the extension.
  if (isAudioMessage(data) || data.mediaType == QStringLiteral("video") ||
      data.mimeType.startsWith(QStringLiteral("video/"))) {
    return false;
  }
  return suffixIsImage(fileNameSuffix(data));
}

inline bool isVideoMessage(const FileMessageData& data) {
  if (data.mediaType == QStringLiteral("video") ||
      data.mimeType.startsWith(QStringLiteral("video/"))) {
    return true;
  }
  if (isAudioMessage(data) || data.mediaType == QStringLiteral("image") ||
      data.mimeType.startsWith(QStringLiteral("image/"))) {
    return false;
  }
  return suffixIsVideo(fileNameSuffix(data));
}

// Coarse classification used for grid/list layout and reply previews.
inline QString mediaKind(const FileMessageData& data) {
  if (isAudioMessage(data)) {
    return QStringLiteral("audio");
  }
  if (isImageMessage(data)) {
    return QStringLiteral("image");
  }
  if (isVideoMessage(data)) {
    return QStringLiteral("video");
  }
  return QStringLiteral("file");
}

inline QString previewText(const FileMessageData& data) {
  if (isAudioMessage(data)) {
    return QStringLiteral("Voice message");
  }

  if (isImageMessage(data)) {
    return QStringLiteral("Photo");
  }

  if (isVideoMessage(data)) {
    return QStringLiteral("Video");
  }

  return data.fileName.isEmpty() ? QStringLiteral("File") : data.fileName;
}

inline bool hasLocalFile(const FileMessageData& data) {
  return !data.localPath.isEmpty() && QFileInfo::exists(data.localPath);
}

}  // namespace filemessage