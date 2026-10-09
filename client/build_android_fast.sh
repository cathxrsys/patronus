#!/usr/bin/env bash

set -euo pipefail

RED='\033[0;31m'
NC='\033[0m'

BUILD_DIR=build-android-arm64-v8a

if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${RED}Build directory $BUILD_DIR does not exist. Please configure the project first.${NC}" >&2
    exit 1
fi

if ! cmake --build "$BUILD_DIR" --config Debug -j"$(nproc)"; then
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
