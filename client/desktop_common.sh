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
QT_GCC_DIR="${QT_GCC_DIR:-$(find "$QT_ROOT" -mindepth 2 -maxdepth 2 -type d -name gcc_64 | sort -V | tail -n1)}"

if [[ -z "$QT_GCC_DIR" ]]; then
    echo "Qt kit gcc_64 not found under $QT_ROOT" >&2
    exit 1
fi

if [[ "$BUILD_TYPE" == "Release" ]]; then
    BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build-release}"
else
    BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build-debug}"
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

configure_and_build_library() {
    local source_dir="$1"
    local build_dir="$2"
    local shared_libs="$3"

    mkdir -p "$build_dir"
    rm -rf "$build_dir/CMakeCache.txt" "$build_dir/CMakeFiles"

    cmake -S "$source_dir" -B "$build_dir" \
        -DBUILD_SHARED_LIBS="$shared_libs" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        "${RELEASE_CMAKE_ARGS[@]}"

    run_configured_build "$build_dir"
}

strip_release_artifact() {
    local artifact="$1"

    if [[ "$BUILD_TYPE" != "Release" ]]; then
        return
    fi

    if ! command -v strip >/dev/null 2>&1; then
        return
    fi

    if [[ -f "$artifact" ]]; then
        strip --strip-unneeded "$artifact" 2>/dev/null || true
    fi
}

echo "Building corecrypto ($BUILD_TYPE)..."
configure_and_build_library "$REPO_ROOT/corecrypto" "$REPO_ROOT/corecrypto/build" OFF

echo "Building doubleratchet ($BUILD_TYPE)..."
configure_and_build_library "$REPO_ROOT/doubleratchet" "$REPO_ROOT/doubleratchet/build" OFF

echo "Building desktop client ($BUILD_TYPE)..."
mkdir -p "$BUILD_DIR"
rm -rf "$BUILD_DIR/CMakeCache.txt" "$BUILD_DIR/CMakeFiles"

cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_AUTOGEN_PARALLEL=1 \
    -DCMAKE_TOOLCHAIN_FILE="$QT_GCC_DIR/lib/cmake/Qt6/qt.toolchain.cmake" \
    -DCMAKE_PREFIX_PATH="$QT_GCC_DIR" \
    -DQt6_DIR="$QT_GCC_DIR/lib/cmake/Qt6" \
    "${RELEASE_CMAKE_ARGS[@]}"

run_configured_build "$BUILD_DIR" patronus

strip_release_artifact "$BUILD_DIR/patronus"

echo "Desktop $BUILD_TYPE build completed: $BUILD_DIR/patronus"