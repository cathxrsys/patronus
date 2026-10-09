#pragma once

#include <QString>

namespace fileopener {

bool openLocalFile(const QString& localPath);
bool openContainingFolder(const QString& localPath);

}  // namespace fileopener