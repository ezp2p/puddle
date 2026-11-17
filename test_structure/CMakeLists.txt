# Mock: shows the targets and which may link which. Not wired up to build.
# Each layer links only what is below it, so a forbidden include fails to compile.
cmake_minimum_required(VERSION 3.24)
project(puddle LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

# Every include names its folder ("objects/user.hpp"), so one include root serves all.
include_directories(${PROJECT_SOURCE_DIR})

# platform: one folder per OS, chosen here and nowhere else
add_library(puddle_platform STATIC)
if(WIN32)
  target_sources(puddle_platform PRIVATE platform/windows/service.cpp platform/windows/paths.cpp
                                         platform/windows/net.cpp platform/windows/fs.cpp platform/windows/lock.cpp)
  target_link_libraries(puddle_platform PRIVATE ws2_32 advapi32)
else()
  target_sources(puddle_platform PRIVATE platform/posix/net.cpp platform/posix/fs.cpp platform/posix/lock.cpp)
  if(APPLE)
    target_sources(puddle_platform PRIVATE platform/macos/service.cpp platform/macos/paths.cpp)
  else()
    target_sources(puddle_platform PRIVATE platform/linux/service.cpp platform/linux/paths.cpp)
  endif()
endif()

# storage: one interface, a driver behind it. Nothing above core links it.
add_library(puddle_storage STATIC storage/storage.cpp
            storage/files/files.cpp storage/files/log.cpp storage/files/records.cpp storage/files/blobs.cpp storage/files/secrets.cpp
            storage/memory/memory.cpp)
target_link_libraries(puddle_storage PUBLIC puddle_platform OpenSSL::Crypto)

add_library(puddle_core STATIC)        # core/*.cpp
target_link_libraries(puddle_core PUBLIC puddle_platform PRIVATE puddle_storage OpenSSL::Crypto yaml)

add_library(puddle_wire STATIC)        # wire/*.cpp
target_link_libraries(puddle_wire PUBLIC puddle_core)

add_library(puddle_events STATIC)      # events/*.cpp
target_link_libraries(puddle_events PUBLIC puddle_wire)

add_library(puddle_objects STATIC)     # objects/*.cpp
target_link_libraries(puddle_objects PUBLIC puddle_events)

add_library(puddle_http STATIC)        # http/*.cpp
target_link_libraries(puddle_http PUBLIC puddle_platform OpenSSL::SSL)

# The two APIs: siblings, neither links the other.
add_library(puddle_api STATIC)         # api/*.cpp, api/routes/*.cpp, api/json/*.cpp
target_link_libraries(puddle_api PUBLIC puddle_objects puddle_http)

add_library(puddle_federation STATIC)  # federation/*.cpp
target_link_libraries(puddle_federation PUBLIC puddle_events puddle_http)

add_executable(puddled service/main.cpp)
target_link_libraries(puddled PRIVATE puddle_api puddle_federation puddle_storage)  # main() picks the driver

# The client side knows a Puddle only over HTTPS: none of the server layers here.
add_library(puddle_client STATIC)      # client/*.cpp
target_link_libraries(puddle_client PUBLIC Qt6::Core Qt6::Network)

qt_add_executable(puddle app/main.cpp app/launcher.cpp)  # plus app/qml
target_link_libraries(puddle PRIVATE puddle_client puddle_platform Qt6::Quick)

# Tests: tests/<layer>/, each linking only the layer it tests.
