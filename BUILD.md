# Concise Build Checklist

## Linux Build

1. Install Qt 6.9.3 via Qt Online Installer into `~/Qt`.
2. Install system dependencies:

```bash
sudo apt install cmake pkg-config libopus-dev libsodium-dev libspdlog-dev libssl-dev nlohmann-json3-dev ffmpeg
```

3. Install `liboqs`.

### Clone the repository

```bash
cd /tmp
git clone https://github.com/open-quantum-safe/liboqs.git
cd liboqs
```

### Build and install

```bash
mkdir build && cd build
cmake -DBUILD_SHARED_LIBS=ON ..
make -j$(nproc)
sudo make install
```

### Update library cache

```bash
sudo ldconfig
```

4. Build the `corecrypto/` and `doubleratchet/` libraries.
5. Run `client/build_debug.sh` for a debug build or `client/build_release.sh` for a release build.

## Linux Portable AppImage Build

1. Install packages:

```bash
sudo apt install desktop-file-utils libfuse2 patchelf ffmpeg
```

2. Run:

```bash
./build_appimage_release.sh
```

For a debug AppImage, run:

```bash
./build_appimage_debug.sh
```

If the AppImage does not start normally, you can launch the extracted bundle manually:

```bash
./patronus-x86_64.AppImage --appimage-extract
cd squashfs-root
./AppRun
```

## Windows `.exe` Build

1. Install Microsoft Visual Studio and MSVC toolsets `v145` and `v141`.
2. Install CMake, Git, Python, and Strawberry Perl if needed.
3. Download `qt-everywhere-src-6.9.3`, then build and install Qt.

```bat
C:\qt-everywhere-src-6.9.3\configure.bat -release -static -static-runtime -opensource -confirm-license -platform win32-msvc -qt-zlib -qt-libpng -qt-libjpeg -nomake examples -nomake tests -prefix "C:\Qt\6.9.3-static"
ninja
ninja install
```

4. Install dependencies via `vcpkg`:

```bat
.\vcpkg install libsodium:x64-windows-static opus:x64-windows-static spdlog:x64-windows-static fmt:x64-windows-static openssl:x64-windows-static nlohmann-json:x64-windows-static
```

5. Install `liboqs`:

```bat
git clone https://github.com/open-quantum-safe/liboqs.git C:\liboqs

cd C:\liboqs
mkdir build && cd build
cmake -G "Visual Studio 18 2026" -A x64 -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_PREFIX=C:/liboqs_static ..

cmake --build . --config Release --parallel
cmake --install .
```

6. Run `build_windows_release.bat` for release or `build_windows_debug.bat` for debug.

# Сжатый чеклист по сборке

## Сборка на Linux

1. Установите Qt 6.9.3 через Qt Online Installer в `~/Qt`.
2. Установите системные зависимости:

```bash
sudo apt install cmake pkg-config libopus-dev libsodium-dev libspdlog-dev libssl-dev nlohmann-json3-dev ffmpeg
```

3. Установите `liboqs`.

### Клонирование репозитория

```bash
cd /tmp
git clone https://github.com/open-quantum-safe/liboqs.git
cd liboqs
```

### Сборка и установка

```bash
mkdir build && cd build
cmake -DBUILD_SHARED_LIBS=ON ..
make -j$(nproc)
sudo make install
```

### Обновление кэша библиотек

```bash
sudo ldconfig
```

4. Соберите библиотеки `corecrypto/` и `doubleratchet/`.
5. Запустите `client/build_debug.sh` для debug-сборки или `client/build_release.sh` для release-сборки.

## Сборка переносимого AppImage для Linux

1. Установите пакеты:

```bash
sudo apt install desktop-file-utils libfuse2 patchelf ffmpeg
```

2. Запустите:

```bash
./build_appimage_release.sh
```

Для debug AppImage используйте:

```bash
./build_appimage_debug.sh
```

Если AppImage не запускается обычным способом, можно вручную запустить распакованный bundle:

```bash
./patronus-x86_64.AppImage --appimage-extract
cd squashfs-root
./AppRun
```

## Сборка Windows `.exe`

1. Установите Microsoft Visual Studio и MSVC toolsets `v145` и `v141`.
2. Установите CMake, Git, Python и Strawberry Perl при необходимости.
3. Скачайте `qt-everywhere-src-6.9.3`, затем соберите и установите Qt.

```bat
C:\qt-everywhere-src-6.9.3\configure.bat -release -static -static-runtime -opensource -confirm-license -platform win32-msvc -qt-zlib -qt-libpng -qt-libjpeg -nomake examples -nomake tests -prefix "C:\Qt\6.9.3-static"
ninja
ninja install
```

4. Установите зависимости через `vcpkg`:

```bat
.\vcpkg install libsodium:x64-windows-static opus:x64-windows-static spdlog:x64-windows-static fmt:x64-windows-static openssl:x64-windows-static nlohmann-json:x64-windows-static
```

5. Установите `liboqs`:

```bat
git clone https://github.com/open-quantum-safe/liboqs.git C:\liboqs

cd C:\liboqs
mkdir build && cd build
cmake -G "Visual Studio 18 2026" -A x64 -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_PREFIX=C:/liboqs_static ..

cmake --build . --config Release --parallel
cmake --install .
```

6. Запустите `build_windows_release.bat` для release или `build_windows_debug.bat` для debug.