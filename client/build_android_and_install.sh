#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

RED='\033[0;31m'
NC='\033[0m'

tmpfile=$(mktemp)
trap 'rm -f "$tmpfile"' EXIT

if ! "$SCRIPT_DIR/android_common.sh" Debug "$@" | tee "$tmpfile"; then
    echo -e "${RED}Build failed — installation skipped.${NC}" >&2
    exit 1
fi

apk_path=$(grep '^APK: ' "$tmpfile" | sed 's/^APK: //' | tail -n 1)

if [[ -n "$apk_path" ]]; then
    echo "Installing: $apk_path"
    adb install -r "$apk_path"
fi
