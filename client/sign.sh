#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARTIFACTS_DIR="${ARTIFACTS_DIR:-$SCRIPT_DIR/artifacts/android/release}"
APK_UNSIGNED="${APK_UNSIGNED:-$ARTIFACTS_DIR/patronus_unsigned.apk}"
APK_SIGNED="${APK_SIGNED:-$ARTIFACTS_DIR/patronus_signed.apk}"
KEYSTORE="${KEYSTORE:-$SCRIPT_DIR/keystores/patronus_key_2026_2.jks}"
APKSIGNER="${APKSIGNER:-$HOME/Android/Sdk/build-tools/35.0.0/apksigner}"
KEY_ALIAS="${KEY_ALIAS:-patronus_key_2026_2}"
KEYSTORE_PASS_FILE="${KEYSTORE_PASS_FILE:-$SCRIPT_DIR/keystores/.patronus_key_2026_2.pass}"

if [[ ! -f "$APK_UNSIGNED" ]]; then
    echo "ERROR: unsigned APK not found: $APK_UNSIGNED" >&2
    exit 1
fi

if [[ ! -f "$KEYSTORE" ]]; then
    echo "ERROR: keystore not found: $KEYSTORE" >&2
    exit 1
fi

if [[ ! -x "$APKSIGNER" ]]; then
    echo "ERROR: apksigner not found: $APKSIGNER" >&2
    exit 1
fi

mkdir -p "$ARTIFACTS_DIR"
rm -f "$APK_SIGNED"

PASS_ARGS=()
if [[ -f "$KEYSTORE_PASS_FILE" ]]; then
    export PATRONUS_KEYSTORE_PASS
    PATRONUS_KEYSTORE_PASS="$(cat "$KEYSTORE_PASS_FILE")"
    PASS_ARGS=(--ks-pass env:PATRONUS_KEYSTORE_PASS --key-pass env:PATRONUS_KEYSTORE_PASS)
fi

echo "Signing APK: $APK_UNSIGNED"
"$APKSIGNER" sign \
    --ks "$KEYSTORE" \
    --ks-key-alias "$KEY_ALIAS" \
    "${PASS_ARGS[@]}" \
    --out "$APK_SIGNED" \
    "$APK_UNSIGNED"

echo "Signed APK: $APK_SIGNED"
ls -lh "$APK_SIGNED"

echo "Verifying signature..."
"$APKSIGNER" verify --verbose "$APK_SIGNED"