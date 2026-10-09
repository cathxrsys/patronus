#!/usr/bin/env bash

set -euo pipefail

RED='\033[0;31m'
NC='\033[0m'

BUILD_DIR=build-android-arm64-v8a

echo "Removing build directory: $BUILD_DIR"
rm -rf "$BUILD_DIR"

QT_ROOT="${QT_ROOT:-$HOME/Qt}"
QT_VERSION="${QT_VERSION:-6.9.3}"
QT_ANDROID_DIR="${QT_ANDROID_DIR:-$QT_ROOT/$QT_VERSION/android_arm64_v8a}"
QT_HOST_PATH="${QT_HOST_PATH:-$QT_ROOT/$QT_VERSION/gcc_64}"
QT_TOOLCHAIN_FILE="$QT_ANDROID_DIR/lib/cmake/Qt6/qt.toolchain.cmake"
ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-$ANDROID_SDK_ROOT/ndk/27.2.12479018}}"
ANDROID_ABI="${ANDROID_ABI:-arm64-v8a}"
ANDROID_PLATFORM="${ANDROID_PLATFORM:-android-35}"

if [ ! -f "$QT_TOOLCHAIN_FILE" ]; then
    echo -e "${RED}Qt Android toolchain file not found: $QT_TOOLCHAIN_FILE${NC}" >&2
    exit 1
fi

mkdir -p "$BUILD_DIR"
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_TOOLCHAIN_FILE="$QT_TOOLCHAIN_FILE" \
    -DQT_HOST_PATH="$QT_HOST_PATH" \
    -DCMAKE_PREFIX_PATH="$QT_ANDROID_DIR" \
    -DANDROID_SDK_ROOT="$ANDROID_SDK_ROOT" \
    -DANDROID_NDK_ROOT="$ANDROID_NDK_ROOT" \
    -DANDROID_ABI="$ANDROID_ABI" \
    -DANDROID_PLATFORM="$ANDROID_PLATFORM"

if ! cmake --build "$BUILD_DIR" --parallel "$(nproc)"; then
    echo -e "${RED}Build failed — installation skipped.${NC}" >&2
    exit 1
fi

APK_PATH=$(find "$BUILD_DIR" -name "*.apk" | head -n 1)
if [ -n "$APK_PATH" ]; then
    echo "APK built: $APK_PATH"
    if command -v adb >/dev/null 2>&1; then
        echo "Installing APK via adb..."
        adb install -r "$APK_PATH"
        echo "APK installed successfully."
    else
        echo "adb not found in PATH. Skipping APK installation."
    fi
else
    echo "Build finished, but APK not found."
fi
