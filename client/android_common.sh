#!/usr/bin/env bash

set -euo pipefail

usage() {
    echo "Usage: $0 <Debug|Release> --home-path /path/to/home" >&2
}

BUILD_TYPE="${1:-}"
shift || true

case "$BUILD_TYPE" in
    Debug|Release)
        ;;
    *)
        usage
        exit 1
        ;;
esac

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$PROJECT_DIR")"
HOME_PATH=""
BUILD_JOBS="${BUILD_JOBS:-1}"
ARTIFACTS_ROOT="${ARTIFACTS_ROOT:-$PROJECT_DIR/artifacts/android}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --home-path)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --home-path" >&2
                exit 1
            fi
            HOME_PATH="$2"
            shift 2
            ;;
        --home-path=*)
            HOME_PATH="${1#*=}"
            shift
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage
            exit 1
            ;;
    esac
done

if [[ -z "$HOME_PATH" ]]; then
    usage
    exit 1
fi

QT_ROOT="${QT_ROOT:-$HOME_PATH/Qt}"

if [[ -z "${QT_VERSION:-}" ]]; then
    QT_VERSION="$(find "$QT_ROOT" -maxdepth 1 -mindepth 1 -type d -name '[0-9]*.[0-9]*.[0-9]*' 2>/dev/null | sort -V | tail -n 1 | xargs -r basename)"
    if [[ -z "$QT_VERSION" ]]; then
        echo "Could not auto-detect Qt version in $QT_ROOT. Install Qt or set QT_VERSION explicitly." >&2
        exit 1
    fi
    echo "Auto-detected Qt version: $QT_VERSION"
fi

QT_HOST_DIR="${QT_HOST_DIR:-$QT_ROOT/$QT_VERSION/gcc_64}"
QT_ANDROID_DIR="${QT_ANDROID_DIR:-$QT_ROOT/$QT_VERSION/android_arm64_v8a}"

ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-$ANDROID_SDK_ROOT/ndk/27.2.12479018}}"
ANDROID_ABI="${ANDROID_ABI:-arm64-v8a}"
ANDROID_PLATFORM="${ANDROID_PLATFORM:-android-35}"
PACKAGE_FORMAT="${PACKAGE_FORMAT:-apk}"

if [[ "$BUILD_TYPE" == "Release" ]]; then
    BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build-android-$ANDROID_ABI-release}"
    ARTIFACTS_DIR="${ARTIFACTS_DIR:-$ARTIFACTS_ROOT/release}"
else
    BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build-android-$ANDROID_ABI-debug}"
    ARTIFACTS_DIR="${ARTIFACTS_DIR:-$ARTIFACTS_ROOT/debug}"
fi

ANDROID_DEPS_LIB_DIR="$PROJECT_DIR/third_party/android/$ANDROID_ABI/lib"
ANDROID_OPENSSL_RUNTIME_DIR="$ANDROID_DEPS_LIB_DIR/runtime"

QT_TOOLCHAIN_FILE="$QT_ANDROID_DIR/lib/cmake/Qt6/qt.toolchain.cmake"
QT_ANDROID_CMAKE_DIR="$QT_ANDROID_DIR/lib/cmake"
QT_HOST_CMAKE_DIR="$QT_HOST_DIR/lib/cmake"

required_qt_components=(
    Qt6Core
    Qt6Gui
    Qt6Network
    Qt6Qml
    Qt6Quick
    Qt6Sql
    Qt6WebSockets
    Qt6Concurrent
    Qt6Multimedia
)

case "$PACKAGE_FORMAT" in
    none|apk|aab|both)
        ;;
    *)
        echo "Unsupported PACKAGE_FORMAT: $PACKAGE_FORMAT" >&2
        echo "Use one of: none, apk, aab, both" >&2
        exit 1
        ;;
esac

if [[ ! -d "$QT_HOST_DIR" ]]; then
    echo "Qt host kit not found: $QT_HOST_DIR" >&2
    exit 1
fi

if [[ ! -d "$QT_ANDROID_DIR" ]]; then
    echo "Qt Android kit not found: $QT_ANDROID_DIR" >&2
    exit 1
fi

if [[ ! -f "$QT_TOOLCHAIN_FILE" ]]; then
    echo "Qt Android toolchain file not found: $QT_TOOLCHAIN_FILE" >&2
    exit 1
fi

if [[ ! -f "$QT_HOST_CMAKE_DIR/Qt6LinguistTools/Qt6LinguistToolsConfig.cmake" ]]; then
    echo "Required host Qt component is missing: Qt6LinguistTools" >&2
    echo "Expected file: $QT_HOST_CMAKE_DIR/Qt6LinguistTools/Qt6LinguistToolsConfig.cmake" >&2
    exit 1
fi

for component in "${required_qt_components[@]}"; do
    if [[ ! -f "$QT_ANDROID_CMAKE_DIR/$component/${component}Config.cmake" ]]; then
        echo "Required Qt Android component is missing: $component" >&2
        echo "Expected file: $QT_ANDROID_CMAKE_DIR/$component/${component}Config.cmake" >&2
        exit 1
    fi
done

if [[ ! -d "$ANDROID_SDK_ROOT" ]]; then
    echo "Android SDK not found: $ANDROID_SDK_ROOT" >&2
    exit 1
fi

if [[ ! -d "$ANDROID_NDK_ROOT" ]]; then
    echo "Android NDK not found: $ANDROID_NDK_ROOT" >&2
    exit 1
fi

if [[ ! -f "$ANDROID_DEPS_LIB_DIR/libssl.a" ]]; then
    echo "Android OpenSSL SSL static library not found: $ANDROID_DEPS_LIB_DIR/libssl.a" >&2
    exit 1
fi

if [[ ! -f "$ANDROID_DEPS_LIB_DIR/libcrypto.a" ]]; then
    echo "Android OpenSSL crypto static library not found: $ANDROID_DEPS_LIB_DIR/libcrypto.a" >&2
    exit 1
fi

android_platform_api="${ANDROID_PLATFORM#android-}"
if [[ "$android_platform_api" =~ ^[0-9]+$ ]]; then
    ANDROID_CLANG_API="$android_platform_api"
else
    ANDROID_CLANG_API="35"
fi

ANDROID_CLANG="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android${ANDROID_CLANG_API}-clang"
ANDROID_STRIP="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"

mkdir -p "$ANDROID_OPENSSL_RUNTIME_DIR"

if [[ ! -f "$ANDROID_OPENSSL_RUNTIME_DIR/libcrypto_3.so" || "$ANDROID_DEPS_LIB_DIR/libcrypto.a" -nt "$ANDROID_OPENSSL_RUNTIME_DIR/libcrypto_3.so" ]]; then
    "$ANDROID_CLANG" \
        -shared -fPIC \
        -Wl,-soname,libcrypto_3.so \
        -o "$ANDROID_OPENSSL_RUNTIME_DIR/libcrypto_3.so" \
        -Wl,--whole-archive "$ANDROID_DEPS_LIB_DIR/libcrypto.a" -Wl,--no-whole-archive \
        -ldl -llog -lz
fi

if [[ ! -f "$ANDROID_OPENSSL_RUNTIME_DIR/libssl_3.so" || "$ANDROID_DEPS_LIB_DIR/libssl.a" -nt "$ANDROID_OPENSSL_RUNTIME_DIR/libssl_3.so" || "$ANDROID_OPENSSL_RUNTIME_DIR/libcrypto_3.so" -nt "$ANDROID_OPENSSL_RUNTIME_DIR/libssl_3.so" ]]; then
    "$ANDROID_CLANG" \
        -shared -fPIC \
        -Wl,-soname,libssl_3.so \
        -o "$ANDROID_OPENSSL_RUNTIME_DIR/libssl_3.so" \
        -Wl,--whole-archive "$ANDROID_DEPS_LIB_DIR/libssl.a" -Wl,--no-whole-archive \
        "$ANDROID_OPENSSL_RUNTIME_DIR/libcrypto_3.so" \
        -ldl -llog -lz
fi

if [[ ! -d "$ANDROID_SDK_ROOT/platforms/$ANDROID_PLATFORM" ]]; then
    echo "Requested Android platform is not installed: $ANDROID_SDK_ROOT/platforms/$ANDROID_PLATFORM" >&2
    exit 1
fi

if [[ "$PACKAGE_FORMAT" != "none" && "$android_platform_api" =~ ^[0-9]+$ ]] && (( android_platform_api < 35 )); then
    echo "Packaging requires compileSdk android-35 or newer because the generated Gradle build depends on AndroidX core 1.16.0." >&2
    exit 1
fi

RELEASE_CMAKE_ARGS=()
if [[ "$BUILD_TYPE" == "Release" ]]; then
    SANITIZE_FLAGS="-O2 -DNDEBUG -g0 -ffile-prefix-map=$REPO_ROOT=. -fdebug-prefix-map=$REPO_ROOT=. -fmacro-prefix-map=$REPO_ROOT=."
    RELEASE_CMAKE_ARGS=(
        "-DCMAKE_C_FLAGS_RELEASE=$SANITIZE_FLAGS"
        "-DCMAKE_CXX_FLAGS_RELEASE=$SANITIZE_FLAGS"
    )
fi

run_configured_build() {
    local build_dir="$1"
    shift || true

    if [[ -f "$build_dir/build.ninja" ]]; then
        ninja -C "$build_dir" -j"$BUILD_JOBS" "$@"
        return
    fi

    if [[ -f "$build_dir/Makefile" ]]; then
        make -C "$build_dir" -j"$BUILD_JOBS" "$@"
        return
    fi

    local cmake_args=(--build "$build_dir" --parallel "$BUILD_JOBS")
    if [[ $# -gt 0 ]]; then
        cmake_args+=(--target)
        cmake_args+=("$@")
    fi
    CMAKE_BUILD_PARALLEL_LEVEL="$BUILD_JOBS" cmake "${cmake_args[@]}"
}

build_package_target() {
    local target="$1"
    local extension="$2"

    run_configured_build "$BUILD_DIR" "$target" >&2

    local artifact_path
    artifact_path="$(find "$BUILD_DIR" -type f -name "*.$extension" | grep -v unsigned | sort | tail -n 1)"
    if [[ -z "$artifact_path" ]]; then
        artifact_path="$(find "$BUILD_DIR" -type f -name "*.$extension" | sort | tail -n 1)"
    fi
    if [[ -z "$artifact_path" ]]; then
        echo "Target '$target' completed, but no .$extension artifact was found under $BUILD_DIR" >&2
        return 1
    fi

    echo "$artifact_path"
}

publish_android_artifact() {
    local source_path="$1"
    local artifact_name="$2"

    mkdir -p "$ARTIFACTS_DIR"
    local destination_path="$ARTIFACTS_DIR/$artifact_name"
    cp -f "$source_path" "$destination_path"
    echo "$destination_path"
}

build_internal_android_libs() {
    echo "Building Android internal libraries: corecrypto, doubleratchet"
    run_configured_build "$BUILD_DIR" corecrypto doubleratchet
}

strip_release_native_artifacts() {
    if [[ "$BUILD_TYPE" != "Release" ]]; then
        return
    fi

    if [[ ! -x "$ANDROID_STRIP" ]]; then
        return
    fi

    while IFS= read -r -d '' file_path; do
        "$ANDROID_STRIP" --strip-unneeded "$file_path" 2>/dev/null || true
    done < <(find "$BUILD_DIR" -type f -name '*.so' -print0)
}

echo "Configuring Android $BUILD_TYPE build..."
mkdir -p "$BUILD_DIR"
rm -rf "$BUILD_DIR/CMakeCache.txt" "$BUILD_DIR/CMakeFiles"

cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_AUTOGEN_PARALLEL=1 \
    -DCMAKE_TOOLCHAIN_FILE="$QT_TOOLCHAIN_FILE" \
    -DQT_HOST_PATH="$QT_HOST_DIR" \
    -DCMAKE_PREFIX_PATH="$QT_ANDROID_DIR" \
    -DQT_NO_GLOBAL_APK_TARGET_PART_OF_ALL=ON \
    -DANDROID_SDK_ROOT="$ANDROID_SDK_ROOT" \
    -DANDROID_NDK_ROOT="$ANDROID_NDK_ROOT" \
    -DANDROID_ABI="$ANDROID_ABI" \
    -DANDROID_PLATFORM="$ANDROID_PLATFORM" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    "${RELEASE_CMAKE_ARGS[@]}"

build_internal_android_libs

echo "Building Android client ($BUILD_TYPE)..."
run_configured_build "$BUILD_DIR"

strip_release_native_artifacts

case "$PACKAGE_FORMAT" in
    none)
        echo "Native Android $BUILD_TYPE build completed: $BUILD_DIR"
        ;;
    apk)
        apk_path="$(build_package_target apk apk)"
        if [[ "$BUILD_TYPE" == "Release" ]]; then
            apk_artifact="$(publish_android_artifact "$apk_path" "patronus_unsigned.apk")"
            echo "Unsigned APK: $apk_artifact"

            sign_script="$PROJECT_DIR/sign.sh"
            if [[ ! -x "$sign_script" ]]; then
                chmod +x "$sign_script"
            fi
            "$sign_script"
        else
            apk_artifact="$(publish_android_artifact "$apk_path" "patronus-debug.apk")"
            echo "APK: $apk_artifact"
        fi
        ;;
    aab)
        aab_path="$(build_package_target aab aab)"
        aab_artifact="$(publish_android_artifact "$aab_path" "$( [[ "$BUILD_TYPE" == "Release" ]] && echo patronus.aab || echo patronus-debug.aab )")"
        echo "AAB: $aab_artifact"
        ;;
    both)
        apk_path="$(build_package_target apk apk)"
        aab_path="$(build_package_target aab aab)"
        if [[ "$BUILD_TYPE" == "Release" ]]; then
            apk_artifact="$(publish_android_artifact "$apk_path" "patronus_unsigned.apk")"
            echo "Unsigned APK: $apk_artifact"

            sign_script="$PROJECT_DIR/sign.sh"
            if [[ ! -x "$sign_script" ]]; then
                chmod +x "$sign_script"
            fi
            "$sign_script"

            aab_artifact="$(publish_android_artifact "$aab_path" "patronus.aab")"
        else
            apk_artifact="$(publish_android_artifact "$apk_path" "patronus-debug.apk")"
            aab_artifact="$(publish_android_artifact "$aab_path" "patronus-debug.aab")"
            echo "APK: $apk_artifact"
        fi
        echo "AAB: $aab_artifact"
        ;;
esac