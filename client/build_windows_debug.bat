@echo off
setlocal enabledelayedexpansion

echo ========================================
echo Building Patronus for Windows x64 Debug
echo ========================================
echo.

for %%I in ("%~dp0.") do set "CLIENT_DIR=%%~fI"
for %%I in ("%CLIENT_DIR%\..") do set "REPO_ROOT=%%~fI"

if not defined VCPKG_ROOT set "VCPKG_ROOT=C:\vcpkg"
if not defined QT_ROOT set "QT_ROOT=C:\Qt\6.9.3-static"
if not defined VCPKG_TRIPLET set "VCPKG_TRIPLET=x64-windows-static"
if not defined LIBOQS_ROOT set "LIBOQS_ROOT=C:\liboqs_static"
if not defined VS_PATH set "VS_PATH=C:\PROGRA~1\Microsoft Visual Studio\18\Community"
if not defined BUILD_JOBS set "BUILD_JOBS=1"
if not defined CMAKE_GENERATOR set "CMAKE_GENERATOR=Ninja Multi-Config"

set "PATRONUS_VCPKG_ROOT=%VCPKG_ROOT%"
set "PATRONUS_QT_ROOT=%QT_ROOT%"
set "PATRONUS_LIBOQS_ROOT=%LIBOQS_ROOT%"

set "BUILD_TYPE=Debug"
set "WINDOWS_LIB_CONFIG=Debug"
set "OUTPUT_NAME=patronus-debug.exe"
set "CORECRYPTO_DIR=%REPO_ROOT%\corecrypto"
set "DOUBLERATCHET_DIR=%REPO_ROOT%\doubleratchet"
set "CORECRYPTO_BUILD_DIR=%CORECRYPTO_DIR%\build-windows"
set "DOUBLERATCHET_BUILD_DIR=%DOUBLERATCHET_DIR%\build-windows"
set "CLIENT_BUILD_DIR=%CLIENT_DIR%\build-windows"
set "OUTPUT_DIR=%CLIENT_DIR%\artifacts\windows\debug"

if not exist "%VCPKG_ROOT%\vcpkg.exe" (
    echo ERROR: vcpkg not found
    exit /b 1
)

if not exist "%QT_ROOT%\lib\cmake" (
    echo ERROR: Qt not found
    exit /b 1
)

call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (
    echo ERROR: VS env failed
    exit /b 1
)

set "VCPKG_ROOT=%PATRONUS_VCPKG_ROOT%"
set "QT_ROOT=%PATRONUS_QT_ROOT%"
set "LIBOQS_ROOT=%PATRONUS_LIBOQS_ROOT%"

set "VCPKG_INSTALLED_PATH=%VCPKG_ROOT%\installed"
set "VCPKG_PREFIX_PATH=%VCPKG_INSTALLED_PATH%\%VCPKG_TRIPLET%"
set "VCPKG_ROOT_CMAKE=%VCPKG_ROOT:\=/%"
set "VCPKG_INSTALLED_PATH_CMAKE=%VCPKG_INSTALLED_PATH:\=/%"
set "VCPKG_PREFIX_PATH_CMAKE=%VCPKG_PREFIX_PATH:\=/%"
set "QT_ROOT_CMAKE=%QT_ROOT:\=/%"
set "LIBOQS_ROOT_CMAKE=%LIBOQS_ROOT:\=/%"

if not exist "%VCPKG_PREFIX_PATH%\include\sodium.h" (
    echo ERROR: libsodium headers not found at %VCPKG_PREFIX_PATH%\include\sodium.h
    exit /b 1
)

if not exist "%VCPKG_PREFIX_PATH%\lib\libsodium.lib" (
    echo ERROR: libsodium library not found at %VCPKG_PREFIX_PATH%\lib\libsodium.lib
    exit /b 1
)

where ninja >nul 2>nul
if errorlevel 1 (
    echo ERROR: ninja not found in PATH
    exit /b 1
)

echo.
echo [1/3] Building corecrypto...
if exist "%CORECRYPTO_BUILD_DIR%" rmdir /s /q "%CORECRYPTO_BUILD_DIR%"
mkdir "%CORECRYPTO_BUILD_DIR%"
pushd "%CORECRYPTO_BUILD_DIR%"

cmake .. ^
    -G "%CMAKE_GENERATOR%" ^
    -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT_CMAKE%/scripts/buildsystems/vcpkg.cmake" ^
    -DVCPKG_TARGET_TRIPLET=%VCPKG_TRIPLET% ^
    -DVCPKG_INSTALLED_PATH="%VCPKG_INSTALLED_PATH_CMAKE%" ^
    -DCMAKE_PREFIX_PATH="%VCPKG_PREFIX_PATH_CMAKE%" ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DCMAKE_C_FLAGS_DEBUG="/MTd /Zi /Od /RTC1" ^
    -DCMAKE_CXX_FLAGS_DEBUG="/MTd /Zi /Od /RTC1" ^
    -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreadedDebug" ^
    -DSODIUM_INCLUDE_DIR="%VCPKG_PREFIX_PATH_CMAKE%/include" ^
    -DSODIUM_LIBRARY="%VCPKG_PREFIX_PATH_CMAKE%/lib/libsodium.lib"
if errorlevel 1 exit /b 1

cmake --build . --config %BUILD_TYPE% --parallel %BUILD_JOBS%
if errorlevel 1 exit /b 1
popd

echo.
echo [2/3] Building doubleratchet...
if exist "%DOUBLERATCHET_BUILD_DIR%" rmdir /s /q "%DOUBLERATCHET_BUILD_DIR%"
mkdir "%DOUBLERATCHET_BUILD_DIR%"
pushd "%DOUBLERATCHET_BUILD_DIR%"

cmake .. ^
    -G "%CMAKE_GENERATOR%" ^
    -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT_CMAKE%/scripts/buildsystems/vcpkg.cmake" ^
    -DVCPKG_TARGET_TRIPLET=%VCPKG_TRIPLET% ^
    -DVCPKG_INSTALLED_PATH="%VCPKG_INSTALLED_PATH_CMAKE%" ^
    -DCMAKE_PREFIX_PATH="%VCPKG_PREFIX_PATH_CMAKE%" ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DCMAKE_C_FLAGS_DEBUG="/MTd /Zi /Od /RTC1" ^
    -DCMAKE_CXX_FLAGS_DEBUG="/MTd /Zi /Od /RTC1" ^
    -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreadedDebug" ^
    -DPATRONUS_WINDOWS_LIB_CONFIG=%WINDOWS_LIB_CONFIG% ^
    -DSODIUM_INCLUDE_DIR="%VCPKG_PREFIX_PATH_CMAKE%/include" ^
    -DSODIUM_LIBRARY="%VCPKG_PREFIX_PATH_CMAKE%/lib/libsodium.lib" ^
    -DOPUS_INCLUDE_DIR="%VCPKG_PREFIX_PATH_CMAKE%/include" ^
    -DOPUS_LIBRARY="%VCPKG_PREFIX_PATH_CMAKE%/lib/opus.lib" ^
    -DSPDLOG_INCLUDE_DIR="%VCPKG_PREFIX_PATH_CMAKE%/include" ^
    -DSPDLOG_LIBRARY="%VCPKG_PREFIX_PATH_CMAKE%/lib/spdlog.lib" ^
    -DFMT_INCLUDE_DIR="%VCPKG_PREFIX_PATH_CMAKE%/include" ^
    -DFMT_LIBRARY="%VCPKG_PREFIX_PATH_CMAKE%/lib/fmt.lib" ^
    -DLIBOQS_INCLUDE_DIR="%LIBOQS_ROOT_CMAKE%/include" ^
    -DLIBOQS_LIBRARY="%LIBOQS_ROOT_CMAKE%/lib/oqs.lib"
if errorlevel 1 exit /b 1

cmake --build . --config %BUILD_TYPE% --parallel %BUILD_JOBS%
if errorlevel 1 exit /b 1
popd

echo.
echo [3/3] Building client...
if exist "%CLIENT_BUILD_DIR%" rmdir /s /q "%CLIENT_BUILD_DIR%"
mkdir "%CLIENT_BUILD_DIR%"
pushd "%CLIENT_BUILD_DIR%"

cmake .. ^
    -G "%CMAKE_GENERATOR%" ^
    -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT_CMAKE%/scripts/buildsystems/vcpkg.cmake" ^
    -DVCPKG_TARGET_TRIPLET=%VCPKG_TRIPLET% ^
    -DVCPKG_INSTALLED_PATH="%VCPKG_INSTALLED_PATH_CMAKE%" ^
    -DCMAKE_PREFIX_PATH="%VCPKG_PREFIX_PATH_CMAKE%;%QT_ROOT_CMAKE%/lib/cmake" ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DCMAKE_C_FLAGS_DEBUG="/MTd /Zi /Od /RTC1" ^
    -DCMAKE_CXX_FLAGS_DEBUG="/MTd /Zi /Od /RTC1" ^
    -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreadedDebug" ^
    -DPATRONUS_WINDOWS_LIB_CONFIG=%WINDOWS_LIB_CONFIG% ^
    -DSODIUM_INCLUDE_DIR="%VCPKG_PREFIX_PATH_CMAKE%/include" ^
    -DSODIUM_LIBRARY="%VCPKG_PREFIX_PATH_CMAKE%/lib/libsodium.lib" ^
    -DOPUS_INCLUDE_DIR="%VCPKG_PREFIX_PATH_CMAKE%/include" ^
    -DOPUS_LIBRARY="%VCPKG_PREFIX_PATH_CMAKE%/lib/opus.lib" ^
    -DSPDLOG_INCLUDE_DIR="%VCPKG_PREFIX_PATH_CMAKE%/include" ^
    -DSPDLOG_LIBRARY="%VCPKG_PREFIX_PATH_CMAKE%/lib/spdlog.lib" ^
    -DFMT_INCLUDE_DIR="%VCPKG_PREFIX_PATH_CMAKE%/include" ^
    -DFMT_LIBRARY="%VCPKG_PREFIX_PATH_CMAKE%/lib/fmt.lib" ^
    -DLIBOQS_INCLUDE_DIR="%LIBOQS_ROOT_CMAKE%/include" ^
    -DLIBOQS_LIBRARY="%LIBOQS_ROOT_CMAKE%/lib/oqs.lib"
if errorlevel 1 exit /b 1

cmake --build . --config %BUILD_TYPE% --parallel %BUILD_JOBS%
if errorlevel 1 exit /b 1
popd

echo.
echo [4/4] Copying result...
if exist "%OUTPUT_DIR%" rmdir /s /q "%OUTPUT_DIR%"
mkdir "%OUTPUT_DIR%"

copy /Y "%CLIENT_BUILD_DIR%\%BUILD_TYPE%\patronus.exe" "%OUTPUT_DIR%\%OUTPUT_NAME%" >nul

if exist "%OUTPUT_DIR%\%OUTPUT_NAME%" (
    echo.
    echo ========================================
    echo BUILD SUCCESSFUL!
    echo ========================================
    echo Executable: %OUTPUT_DIR%\%OUTPUT_NAME%
    dir "%OUTPUT_DIR%\%OUTPUT_NAME%"
) else (
    echo ERROR: Failed to copy
    exit /b 1
)

echo.
exit /b 0