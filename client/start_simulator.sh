#!/usr/bin/env bash

set -euo pipefail

export PATH="$PATH:/opt/android-sdk/emulator:/opt/android-sdk/platform-tools"

AVD_NAME="${1:-api31_phone_x86}"

emulator -avd "$AVD_NAME" -gpu host -no-snapshot
