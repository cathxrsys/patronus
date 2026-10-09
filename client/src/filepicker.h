#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>

namespace filepicker {

using SelectionCallback = std::function<void(const QString& filePath)>;
using MultiSelectionCallback =
    std::function<void(const QStringList& filePaths)>;

void pickOpenFile(QObject* context, const QString& title,
                  const QString& initialDirectory, const QString& filter,
                  SelectionCallback callback);

// Multi-file variant. Desktop uses QFileDialog::getOpenFileNames; Android uses
// ACTION_OPEN_DOCUMENT with EXTRA_ALLOW_MULTIPLE. The callback receives the
// (possibly empty) list of local file paths; on Android the picked content is
// copied into the temp directory first, mirroring pickOpenFile.
void pickOpenFiles(QObject* context, const QString& title,
                   const QString& initialDirectory, const QString& filter,
                   MultiSelectionCallback callback);

}  // namespace filepicker