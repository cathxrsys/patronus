#pragma once

#include <functional>

class QObject;

// Shared microphone-permission gate for the call and voice-message recorders
// (mirrors the QCameraPermission flow in QrScanner::requestCameraPermission).
// Requests the permission if it has never been decided, invoking onResult
// once the user responds; calls onResult synchronously otherwise. Never
// re-prompts once denied, since the OS itself won't show its dialog again --
// callers should point the user to system settings in that case.
namespace audiopermission {
void ensureMicrophoneAccess(QObject* context,
                            std::function<void(bool granted)> onResult);
}
