cmake_minimum_required(VERSION 4.0)
project(certgen)

set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED libsodium)

find_package(Boost REQUIRED COMPONENTS system)

pkg_check_modules(SODIUM REQUIRED libsodium)


set(SOURCES
        main.cpp
)

set(HEADERS
        ../corecrypto/corecrypto.h
)


add_executable(certgen ${SOURCES})
target_include_directories(certgen PRIVATE ${SODIUM_INCLUDE_DIRS})
target_link_libraries(certgen PRIVATE ${SODIUM_LIBRARIES})

target_link_libraries(certgen PRIVATE ${CMAKE_SOURCE_DIR}/../corecrypto/build/libcorecrypto.a)

target_include_directories(certgen PRIVATE ../corecrypto)

target_include_directories(certgen PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(certgen PRIVATE ${Boost_LIBRARIES})

find_package(spdlog REQUIRED)
target_link_libraries(certgen PRIVATE spdlog::spdlog)


target_include_directories(certgen
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${SODIUM_INCLUDE_DIRS}
)

target_link_libraries(certgen
        PUBLIC
        ${SODIUM_LIBRARIES}
)