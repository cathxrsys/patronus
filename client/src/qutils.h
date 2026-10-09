#pragma once

#include <QByteArray>
#include <QObject>
#include <cstdint>
#include <vector>

namespace qutils {
inline std::vector<uint8_t> to_vector(const QByteArray &array) {
  return std::vector<uint8_t>(
      reinterpret_cast<const uint8_t *>(array.data()),
      reinterpret_cast<const uint8_t *>(array.data() + array.size()));
}

inline std::vector<uint8_t> to_vector(const QString &hexString) {
  QByteArray byteArray = QByteArray::fromHex(hexString.toUtf8());
  return to_vector(byteArray);
}

inline QByteArray from_vector(const std::vector<uint8_t> &vec) {
  return QByteArray(reinterpret_cast<const char *>(vec.data()),
                    static_cast<int>(vec.size()));
}

inline QString from_vector_to_hex(const std::vector<uint8_t> &vec) {
  QByteArray byteArray = from_vector(vec);
  return QString::fromUtf8(byteArray.toHex());
}
}  // namespace qutils