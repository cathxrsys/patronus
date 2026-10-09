cmake_minimum_required(VERSION 3.16)
project(doubleratchet VERSION 1.0 LANGUAGES CXX)

if(WIN32)
    set(PATRONUS_WINDOWS_LIB_CONFIG "Release" CACHE STRING "Windows configuration used for internal prebuilt libraries")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Option for a shared library build (STATIC by default)
option(BUILD_SHARED_LIBS "Build shared library" OFF)

if(ANDROID)
    set(ANDROID_DEPS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../client/third_party/android/${ANDROID_ABI})
    set(ANDROID_INCLUDE_DIR ${ANDROID_DEPS_ROOT}/include)
    set(ANDROID_LIB_DIR ${ANDROID_DEPS_ROOT}/lib)

    if(NOT EXISTS ${ANDROID_INCLUDE_DIR}/sodium.h)
        message(FATAL_ERROR "Android sodium headers not found in ${ANDROID_INCLUDE_DIR}")
    endif()
    if(NOT EXISTS ${ANDROID_LIB_DIR}/libsodium.a)
        message(FATAL_ERROR "Android sodium library not found in ${ANDROID_LIB_DIR}")
    endif()
    if(NOT EXISTS ${ANDROID_INCLUDE_DIR}/spdlog/spdlog.h)
        message(FATAL_ERROR "Android spdlog headers not found in ${ANDROID_INCLUDE_DIR}")
    endif()
    if(NOT EXISTS ${ANDROID_LIB_DIR}/libspdlog.a)
        message(FATAL_ERROR "Android spdlog library not found in ${ANDROID_LIB_DIR}")
    endif()
    if(NOT EXISTS ${ANDROID_INCLUDE_DIR}/nlohmann/json.hpp)
        message(FATAL_ERROR "Android nlohmann headers not found in ${ANDROID_INCLUDE_DIR}")
    endif()
    if(NOT EXISTS ${ANDROID_INCLUDE_DIR}/openssl/ssl.h)
        message(FATAL_ERROR "Android OpenSSL headers not found in ${ANDROID_INCLUDE_DIR}")
    endif()
    if(NOT EXISTS ${ANDROID_LIB_DIR}/libcrypto.a)
        message(FATAL_ERROR "Android OpenSSL crypto library not found in ${ANDROID_LIB_DIR}")
    endif()
    if(NOT EXISTS ${ANDROID_INCLUDE_DIR}/oqs/oqs.h)
        message(FATAL_ERROR "Android liboqs headers not found in ${ANDROID_INCLUDE_DIR}")
    endif()
    if(NOT EXISTS ${ANDROID_LIB_DIR}/liboqs.a)
        message(FATAL_ERROR "Android liboqs library not found in ${ANDROID_LIB_DIR}")
    endif()

    set(SODIUM_INCLUDE_DIR ${ANDROID_INCLUDE_DIR})
    set(SODIUM_LIBRARY ${ANDROID_LIB_DIR}/libsodium.a)
    set(SPDLOG_INCLUDE_DIR ${ANDROID_INCLUDE_DIR})
    set(SPDLOG_LIBRARY ${ANDROID_LIB_DIR}/libspdlog.a)
    set(NLOHMANN_JSON_INCLUDE_DIR ${ANDROID_INCLUDE_DIR})
    set(OPENSSL_INCLUDE_DIR ${ANDROID_INCLUDE_DIR})
    set(OPENSSL_CRYPTO_LIBRARY ${ANDROID_LIB_DIR}/libcrypto.a)
    set(LIBOQS_INCLUDE_DIR ${ANDROID_INCLUDE_DIR})
    set(LIBOQS_LIBRARY ${ANDROID_LIB_DIR}/liboqs.a)
elseif(WIN32)
    # Windows: use vcpkg
    find_path(SODIUM_INCLUDE_DIR sodium.h PATHS ${VCPKG_INSTALLED_PATH}/x64-windows/include REQUIRED)
    find_library(SODIUM_LIBRARY NAMES sodium libsodium PATHS ${VCPKG_INSTALLED_PATH}/x64-windows/lib REQUIRED)
    
    find_package(spdlog CONFIG REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    
    find_path(LIBOQS_INCLUDE_DIR oqs/oqs.h PATHS ${VCPKG_INSTALLED_PATH}/x64-windows/include REQUIRED)
    find_library(LIBOQS_LIBRARY NAMES oqs PATHS ${VCPKG_INSTALLED_PATH}/x64-windows/lib REQUIRED)
else()
    # Linux/macOS: use pkg-config
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SODIUM REQUIRED libsodium)

    find_package(spdlog REQUIRED)
    find_package(nlohmann_json REQUIRED)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)

    find_path(LIBOQS_INCLUDE_DIR oqs/oqs.h)
    find_library(LIBOQS_LIBRARY NAMES oqs)

    if (NOT LIBOQS_INCLUDE_DIR OR NOT LIBOQS_LIBRARY)
        message(FATAL_ERROR "liboqs not found. Provide -DLIBOQS_INCLUDE_DIR and -DLIBOQS_LIBRARY.")
    endif()
endif()

# Source files only (no headers)
set(SOURCES
    doubleratchet.cpp
    crypto.cpp
    kdf.cpp
    pq.cpp
    pqdh.cpp
    prekeys.cpp
    ../coreutils/coreutils.cpp
)

# Create the library (headers are not needed in add_library)
add_library(doubleratchet ${SOURCES})

# Headers for installation only
set(HEADERS
    doubleratchet.h
    crypto.h
    kdf.h
    pq.h
    pqdh.h
    prekeys.h
    ../coreutils/coreutils.h
)

target_include_directories(doubleratchet
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../corecrypto
    ../coreutils
)

if(ANDROID)
    target_include_directories(doubleratchet
        PUBLIC
        ${SODIUM_INCLUDE_DIR}
        ${SPDLOG_INCLUDE_DIR}
        ${NLOHMANN_JSON_INCLUDE_DIR}
        ${OPENSSL_INCLUDE_DIR}
        ${LIBOQS_INCLUDE_DIR}
    )
elseif(WIN32)
    target_include_directories(doubleratchet
        PUBLIC
        ${SODIUM_INCLUDE_DIR}
        ${LIBOQS_INCLUDE_DIR}
    )
else()
    target_include_directories(doubleratchet
        PUBLIC
        ${SODIUM_INCLUDE_DIRS}
        ${LIBOQS_INCLUDE_DIR}
    )
endif()

# Link against the existing CMake target if the library was added via add_subdirectory.
if(TARGET corecrypto)
    set(CORECRYPTO_LIB corecrypto)
elseif(WIN32)
    find_library(CORECRYPTO_LIB corecrypto PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../corecrypto/build-windows/${PATRONUS_WINDOWS_LIB_CONFIG} REQUIRED)
else()
    find_library(CORECRYPTO_LIB corecrypto PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../corecrypto/build REQUIRED)
endif()

if(ANDROID)
    target_link_libraries(doubleratchet
        PUBLIC
        ${CORECRYPTO_LIB}
        ${SODIUM_LIBRARY}
        ${OPENSSL_CRYPTO_LIBRARY}
        ${LIBOQS_LIBRARY}
        PRIVATE
        ${SPDLOG_LIBRARY}
    )
elseif(WIN32)
    target_link_libraries(doubleratchet
        PUBLIC
        ${CORECRYPTO_LIB}
        ${SODIUM_LIBRARY}
        ${LIBOQS_LIBRARY}
        PRIVATE
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        OpenSSL::Crypto
    )
else()
    target_link_libraries(doubleratchet
        PUBLIC
        ${CORECRYPTO_LIB}
        ${SODIUM_LIBRARIES}
        ${LIBOQS_LIBRARY}
        PRIVATE
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        OpenSSL::Crypto
    )
endif()

# Installation
install(TARGETS doubleratchet
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES ${HEADERS} DESTINATION include/doubleratchet)