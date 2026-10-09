#!/usr/bin/env bash

set -euo pipefail

usage() {
    echo "Usage: $0 <Debug|Release> [--home_path /path/to/home]" >&2
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
HOME_PATH="$HOME"
BUILD_JOBS="${BUILD_JOBS:-1}"
ARTIFACTS_ROOT="${ARTIFACTS_ROOT:-$PROJECT_DIR/artifacts/appimage}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --home_path|--home-path)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --home_path" >&2
                exit 1
            fi
            HOME_PATH="$2"
            shift 2
            ;;
        --home_path=*|--home-path=*)
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

QT_ROOT="${QT_ROOT:-$HOME_PATH/Qt}"
QT_GCC_DIR="${QT_GCC_DIR:-$(find "$QT_ROOT" -mindepth 2 -maxdepth 2 -type d -name gcc_64 | sort -V | tail -n1)}"

if [[ -z "$QT_GCC_DIR" ]]; then
    echo "Qt kit gcc_64 not found under $QT_ROOT" >&2
    exit 1
fi

MODE_LOWER="$(printf '%s' "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')"
CLIENT_BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build-appimage-$MODE_LOWER}"
APP_DIR="${APP_DIR:-$PROJECT_DIR/AppDir-$MODE_LOWER}"
APPIMAGETOOL="$PROJECT_DIR/appimagetool-x86_64.AppImage"
ARTIFACTS_DIR="${ARTIFACTS_DIR:-$ARTIFACTS_ROOT/$MODE_LOWER}"

if [[ "$BUILD_TYPE" == "Release" ]]; then
    OUTPUT_NAME="patronus-x86_64.AppImage"
else
    OUTPUT_NAME="patronus-debug-x86_64.AppImage"
fi

RELEASE_CMAKE_ARGS=()
INSTALL_ARGS=()
if [[ "$BUILD_TYPE" == "Release" ]]; then
    SANITIZE_FLAGS="-O2 -DNDEBUG -g0 -ffile-prefix-map=$REPO_ROOT=. -fdebug-prefix-map=$REPO_ROOT=. -fmacro-prefix-map=$REPO_ROOT=."
    RELEASE_CMAKE_ARGS=(
        "-DCMAKE_C_FLAGS_RELEASE=$SANITIZE_FLAGS"
        "-DCMAKE_CXX_FLAGS_RELEASE=$SANITIZE_FLAGS"
    )
    INSTALL_ARGS=(--strip)
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

if [[ ! -f "$APPIMAGETOOL" ]]; then
    echo "Downloading appimagetool..."
    wget -q --show-progress https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage -O "$APPIMAGETOOL"
    chmod +x "$APPIMAGETOOL"
fi

configure_and_build_library() {
    local source_dir="$1"
    local build_dir="$2"

    mkdir -p "$build_dir"
    rm -rf "$build_dir/CMakeCache.txt" "$build_dir/CMakeFiles"

    cmake -S "$source_dir" -B "$build_dir" \
        -DBUILD_SHARED_LIBS=ON \
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

copy_shared_lib() {
    local source_lib="$1"

    if [[ -z "$source_lib" || ! -e "$source_lib" ]]; then
        return
    fi

    local resolved_lib
    resolved_lib="$(readlink -f "$source_lib")"
    local resolved_name
    resolved_name="$(basename "$resolved_lib")"
    local source_name
    source_name="$(basename "$source_lib")"

    cp -f "$resolved_lib" "$APP_DIR/usr/lib/$resolved_name"

    if [[ "$source_name" != "$resolved_name" ]]; then
        ln -sf "$resolved_name" "$APP_DIR/usr/lib/$source_name"
    fi
}

copy_selected_runtime_deps() {
    local target="$1"

    if [[ ! -f "$target" ]]; then
        return
    fi

    ldd "$target" 2>/dev/null | awk '/=> \/.* \(/ { print $3 }' | while read -r lib; do
        case "$(basename "$lib")" in
            libfmt.so*|libspdlog.so*|libsodium.so*|libopus.so*|libssl.so*|libcrypto.so*|liboqs.so*|libstdc++.so*|libgcc_s.so*)
                copy_shared_lib "$lib"
                ;;
        esac
    done
}

echo "Cleaning AppImage build directories..."
rm -rf "$REPO_ROOT/corecrypto/build"
rm -rf "$REPO_ROOT/doubleratchet/build"
rm -rf "$CLIENT_BUILD_DIR"
rm -rf "$APP_DIR"

echo "Building corecrypto ($BUILD_TYPE)..."
configure_and_build_library "$REPO_ROOT/corecrypto" "$REPO_ROOT/corecrypto/build"

echo "Building doubleratchet ($BUILD_TYPE)..."
configure_and_build_library "$REPO_ROOT/doubleratchet" "$REPO_ROOT/doubleratchet/build"

echo "Building AppImage client ($BUILD_TYPE)..."
mkdir -p "$CLIENT_BUILD_DIR"

cmake -S "$PROJECT_DIR" -B "$CLIENT_BUILD_DIR" \
    -DBUILD_APPIMAGE=ON \
    -DCMAKE_AUTOGEN_PARALLEL=1 \
    -DCMAKE_TOOLCHAIN_FILE="$QT_GCC_DIR/lib/cmake/Qt6/qt.toolchain.cmake" \
    -DCMAKE_PREFIX_PATH="$QT_GCC_DIR" \
    -DQt6_DIR="$QT_GCC_DIR/lib/cmake/Qt6" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    "${RELEASE_CMAKE_ARGS[@]}"

run_configured_build "$CLIENT_BUILD_DIR"

mkdir -p "$APP_DIR"
DESTDIR="$APP_DIR" cmake --install "$CLIENT_BUILD_DIR" --prefix /usr "${INSTALL_ARGS[@]}"

mkdir -p "$APP_DIR/usr/bin" "$APP_DIR/usr/lib" "$APP_DIR/usr/plugins" "$APP_DIR/usr/qml"

if [[ ! -f "$APP_DIR/usr/bin/patronus" ]]; then
    echo "AppImage binary not found after install" >&2
    exit 1
fi

cp "$REPO_ROOT/corecrypto/build/libcorecrypto.so" "$APP_DIR/usr/lib/"
cp "$REPO_ROOT/doubleratchet/build/libdoubleratchet.so" "$APP_DIR/usr/lib/"

echo "Copying Qt runtime libraries..."
cp -P $QT_GCC_DIR/lib/*.so* "$APP_DIR/usr/lib/" 2>/dev/null || true

echo "Copying selected runtime dependencies..."
copy_selected_runtime_deps "$CLIENT_BUILD_DIR/patronus"
copy_selected_runtime_deps "$REPO_ROOT/corecrypto/build/libcorecrypto.so"
copy_selected_runtime_deps "$REPO_ROOT/doubleratchet/build/libdoubleratchet.so"

echo "Copying Qt plugins and QML modules..."
cp -r $QT_GCC_DIR/plugins/* "$APP_DIR/usr/plugins/" 2>/dev/null || true
cp -r $QT_GCC_DIR/qml/* "$APP_DIR/usr/qml/" 2>/dev/null || true

# WebP (and TIFF) support ships in the qtimageformats add-on. If it is not
# installed in the Qt kit, the copy above silently leaves qwebp out and any
# WebP image (often received with a .png name) renders blank. Warn loudly.
if [[ ! -e "$APP_DIR/usr/plugins/imageformats/libqwebp.so" ]]; then
    echo "WARNING: imageformats/libqwebp.so is missing from the Qt kit at" \
         "$QT_GCC_DIR — install the qtimageformats add-on and rebuild, or WebP" \
         "images will not display." >&2
fi

while IFS= read -r link_path; do
    target_path="$(readlink "$link_path")"
    if [[ "$target_path" == /usr/* || "$target_path" == /lib/* ]]; then
        rm -f "$link_path"
        target_name="$(basename "$target_path")"
        if [[ -f "$APP_DIR/usr/lib/$target_name" ]]; then
            ln -sf "$target_name" "$link_path"
        fi
    fi
done < <(find "$APP_DIR/usr/lib" -type l)

if command -v patchelf >/dev/null 2>&1; then
    patchelf --set-rpath '$ORIGIN/../lib' "$APP_DIR/usr/bin/patronus"
    find "$APP_DIR/usr/lib" -name '*.so*' -type f -exec patchelf --set-rpath '$ORIGIN' {} \; 2>/dev/null || true
fi

cat > "$APP_DIR/usr/bin/qt.conf" << 'EOF'
[Paths]
Prefix = ..
Plugins = plugins
Imports = qml
Qml2Imports = qml
EOF

cat > "$APP_DIR/AppRun" << 'EOF'
#!/usr/bin/env bash
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib:$LD_LIBRARY_PATH"
export QT_PLUGIN_PATH="$HERE/usr/plugins"
export QML2_IMPORT_PATH="$HERE/usr/qml"
mkdir -p "$HERE/PatronusData"
exec "$HERE/usr/bin/patronus" "$@"
EOF
chmod +x "$APP_DIR/AppRun"

cat > "$APP_DIR/patronus.desktop" << 'EOF'
[Desktop Entry]
Name=patronus
Exec=patronus
Icon=patronus
Type=Application
Categories=Network;
EOF

if [[ -f "$REPO_ROOT/icons/main.png" ]]; then
    cp "$REPO_ROOT/icons/main.png" "$APP_DIR/patronus.png"
fi

strip_release_artifact "$APP_DIR/usr/bin/patronus"
strip_release_artifact "$APP_DIR/usr/lib/libcorecrypto.so"
strip_release_artifact "$APP_DIR/usr/lib/libdoubleratchet.so"

mkdir -p "$ARTIFACTS_DIR"
rm -f "$ARTIFACTS_DIR/$OUTPUT_NAME"
ARCH=x86_64 "$APPIMAGETOOL" "$APP_DIR" "$ARTIFACTS_DIR/$OUTPUT_NAME"

echo "AppImage $BUILD_TYPE build completed: $ARTIFACTS_DIR/$OUTPUT_NAME"