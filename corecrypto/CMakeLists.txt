cmake_minimum_required(VERSION 3.16)
project(corecrypto VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Option for a shared library build (STATIC by default)
option(BUILD_SHARED_LIBS "Build shared library" OFF)

if(ANDROID)
    set(ANDROID_DEPS_ROOT ${CMAKE_SOURCE_DIR}/../client/third_party/android/${ANDROID_ABI})
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
    if(NOT EXISTS ${ANDROID_INCLUDE_DIR}/fmt/format.h)
        message(FATAL_ERROR "Android fmt headers not found in ${ANDROID_INCLUDE_DIR}")
    endif()
    if(NOT EXISTS ${ANDROID_LIB_DIR}/libfmt.a)
        message(FATAL_ERROR "Android fmt library not found in ${ANDROID_LIB_DIR}")
    endif()

    set(SODIUM_INCLUDE_DIR ${ANDROID_INCLUDE_DIR})
    set(SODIUM_LIBRARY ${ANDROID_LIB_DIR}/libsodium.a)
    set(SPDLOG_INCLUDE_DIR ${ANDROID_INCLUDE_DIR})
    set(SPDLOG_LIBRARY ${ANDROID_LIB_DIR}/libspdlog.a)
    set(FMT_INCLUDE_DIR ${ANDROID_INCLUDE_DIR})
    set(FMT_LIBRARY ${ANDROID_LIB_DIR}/libfmt.a)
elseif(WIN32)
    # Windows: use variables passed on the command line
    message(STATUS "SODIUM_INCLUDE_DIR = ${SODIUM_INCLUDE_DIR}")
    message(STATUS "SODIUM_LIBRARY = ${SODIUM_LIBRARY}")

    set(SODIUM_INCLUDE_DIR_NORMALIZED "${SODIUM_INCLUDE_DIR}")
    set(SODIUM_LIBRARY_NORMALIZED "${SODIUM_LIBRARY}")
    file(TO_CMAKE_PATH "${SODIUM_INCLUDE_DIR_NORMALIZED}" SODIUM_INCLUDE_DIR_NORMALIZED)
    file(TO_CMAKE_PATH "${SODIUM_LIBRARY_NORMALIZED}" SODIUM_LIBRARY_NORMALIZED)
    
    if(NOT DEFINED SODIUM_INCLUDE_DIR OR SODIUM_INCLUDE_DIR STREQUAL "")
        message(FATAL_ERROR "SODIUM_INCLUDE_DIR not set or invalid: ${SODIUM_INCLUDE_DIR}")
    endif()
    if(NOT IS_DIRECTORY "${SODIUM_INCLUDE_DIR_NORMALIZED}")
        message(FATAL_ERROR "SODIUM_INCLUDE_DIR not set or invalid: ${SODIUM_INCLUDE_DIR_NORMALIZED}")
    endif()
    if(NOT DEFINED SODIUM_LIBRARY OR SODIUM_LIBRARY STREQUAL "")
        message(FATAL_ERROR "SODIUM_LIBRARY not set or invalid: ${SODIUM_LIBRARY}")
    endif()
    if(NOT EXISTS "${SODIUM_LIBRARY_NORMALIZED}")
        message(FATAL_ERROR "SODIUM_LIBRARY not set or invalid: ${SODIUM_LIBRARY_NORMALIZED}")
    endif()

    set(SODIUM_INCLUDE_DIR "${SODIUM_INCLUDE_DIR_NORMALIZED}")
    set(SODIUM_LIBRARY "${SODIUM_LIBRARY_NORMALIZED}")
    
    find_package(spdlog CONFIG REQUIRED)
    find_package(fmt CONFIG REQUIRED)
else()
    # Linux/macOS: use pkg-config
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SODIUM REQUIRED libsodium)
    find_package(spdlog REQUIRED)
endif()

set(SOURCES
    corecrypto.cpp
    bip39.c
    bip39_english.c
    pbkdf2.c
    sha2.c
    hmac.c
    memzero.c
)

set(HEADERS
    corecrypto.h
    bip39.h
    pbkdf2.h
    sha2.h
    hmac.h
    memzero.h
    byte_order.h
)

# Create the library with shared/static support
add_library(corecrypto ${SOURCES} ${HEADERS})

target_include_directories(corecrypto
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(ANDROID)
    target_include_directories(corecrypto
        PUBLIC
        ${SODIUM_INCLUDE_DIR}
        ${SPDLOG_INCLUDE_DIR}
        ${FMT_INCLUDE_DIR}
    )
elseif(WIN32)
    target_include_directories(corecrypto
        PUBLIC
        ${SODIUM_INCLUDE_DIR}
    )
else()
    target_include_directories(corecrypto
        PUBLIC
        ${SODIUM_INCLUDE_DIRS}
    )
endif()

if(ANDROID)
    target_link_libraries(corecrypto
        PUBLIC
        ${SODIUM_LIBRARY}
        ${SPDLOG_LIBRARY}
        ${FMT_LIBRARY}
    )
elseif(WIN32)
    target_link_libraries(corecrypto
        PUBLIC
        ${SODIUM_LIBRARY}
        spdlog::spdlog
        fmt::fmt
    )
else()
    target_link_libraries(corecrypto
        PUBLIC
        ${SODIUM_LIBRARIES}
        spdlog::spdlog
    )
endif()

# Installation
install(TARGETS corecrypto
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES ${HEADERS} DESTINATION include/corecrypto)