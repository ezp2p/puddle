cmake_minimum_required(VERSION 3.21)
project(puddle VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Homebrew's OpenSSL is keg-only on macOS; the system one is LibreSSL.
if(APPLE AND NOT OPENSSL_ROOT_DIR AND EXISTS /opt/homebrew/opt/openssl@3)
  set(OPENSSL_ROOT_DIR /opt/homebrew/opt/openssl@3)
endif()

# Release builds link OpenSSL statically so the binaries don't depend on the
# host's libssl version.
option(PUDDLE_STATIC_OPENSSL "Link OpenSSL statically" OFF)
if(PUDDLE_STATIC_OPENSSL)
  set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()

# libyaml, for the config and profile files (pkg-config isn't assumed to exist).
find_path(YAML_INCLUDE_DIR yaml.h)
if(PUDDLE_STATIC_OPENSSL)
  find_library(YAML_LIBRARY NAMES libyaml.a yaml)
else()
  find_library(YAML_LIBRARY NAMES yaml)
endif()
if(NOT YAML_INCLUDE_DIR OR NOT YAML_LIBRARY)
  message(FATAL_ERROR "libyaml not found (brew install libyaml / apt install libyaml-dev)")
endif()

find_package(OpenSSL 3.0 REQUIRED)
find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra)
# Paths are built as "<data_dir>/<name>" in PUDDLE_PATH_MAX buffers; data_dir is capped at
# PUDDLE_DATA_DIR_MAX when the config is read, so gcc's worst-case truncation warnings are noise.
add_compile_options(-Wno-format-truncation)

# glibc hides nftw()/FTW_*/memmem() unless asked; macOS exposes them by default.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_compile_definitions(_GNU_SOURCE)
endif()

# --- the Puddle, in layers (C++20, C-callable headers). Each layer links only the ones below it, so a file that
# includes a layer it may not use does not build. Every include names its folder: "core/identity.h".
# api/ and federation/ are siblings: neither links the other. The objects link neither http nor federation.

# platform: what differs per OS: clocks, files and directories
add_library(puddle_platform STATIC
  src/platform/clock.cpp
  src/platform/fsutil.cpp)
# The one place the OS is chosen: each file in src/platform/ ends in _posix or _windows.
if(WIN32)
  target_sources(puddle_platform PRIVATE src/platform/fs_windows.cpp src/platform/net_windows.cpp
                 src/platform/paths_windows.cpp src/platform/process_windows.cpp)
  target_link_libraries(puddle_platform PUBLIC ws2_32)
else()
  target_sources(puddle_platform PRIVATE src/platform/fs_posix.cpp src/platform/net_posix.cpp
                 src/platform/paths_posix.cpp src/platform/process_posix.cpp)
endif()
target_include_directories(puddle_platform PUBLIC src)

# base: what every layer uses: hashing, JSON, encoding, logging, cleaning text
add_library(puddle_base STATIC
  src/base/encoding.cpp
  src/base/hash.cpp
  src/base/json.cpp
  src/base/log.cpp
  src/base/text.cpp)
target_link_libraries(puddle_base PUBLIC puddle_platform OpenSSL::Crypto Threads::Threads)

# storage: the chat logs and media on disk
add_library(puddle_storage STATIC
  src/storage/storage.cpp
  src/storage/media.cpp
  src/storage/store.cpp)
target_link_libraries(puddle_storage PUBLIC puddle_base)

# core: config, keys, and the records kept through storage/ (streams, people, conversations, groups, the outbox's queue, profile assignments)
add_library(puddle_core STATIC
  src/core/auth.cpp
  src/core/config.cpp
  src/core/context.cpp
  src/core/conversations.cpp
  src/core/delivery.cpp
  src/core/group.cpp
  src/core/identity.cpp
  src/core/link.cpp
  src/core/outbox.cpp
  src/core/people.cpp
  src/core/phrase.cpp
  src/core/profile.cpp
  src/core/ratelimit.cpp
  src/core/streams.cpp
  src/core/yamltree.cpp)
target_link_libraries(puddle_core PUBLIC puddle_storage)
# Only yamltree.cpp sees libyaml's headers: a shared include dir like /opt/homebrew/include would
# also expose a different OpenSSL's headers.
set_source_files_properties(src/core/yamltree.cpp PROPERTIES INCLUDE_DIRECTORIES "${YAML_INCLUDE_DIR}")
target_link_libraries(puddle_core PUBLIC OpenSSL::Crypto Threads::Threads ${YAML_LIBRARY})

# wire: the shapes both APIs send
add_library(puddle_wire STATIC
  src/wire/wireprofile.cpp)
target_link_libraries(puddle_wire PUBLIC puddle_core)

# events: signed events: build, parse, sign, fold
add_library(puddle_events STATIC
  src/events/group_events.cpp
  src/events/message.cpp)
target_link_libraries(puddle_events PUBLIC puddle_core)

# objects: the owner API's objects: User, Stream, Conversation, Message, Group
add_library(puddle_objects STATIC
  src/objects/conversation.cpp
  src/objects/group.cpp
  src/objects/message.cpp
  src/objects/stream.cpp
  src/objects/profile.cpp
  src/objects/user.cpp)
target_link_libraries(puddle_objects PUBLIC puddle_events)

# http: TLS, the listener, the router, the client
add_library(puddle_http STATIC
  src/http/client.cpp
  src/http/http.cpp
  src/http/reqsig.cpp
  src/http/router.cpp
  src/http/serve.cpp
  src/http/tls.cpp)
target_link_libraries(puddle_http PUBLIC puddle_core)
target_link_libraries(puddle_http PUBLIC OpenSSL::SSL)

# api: the owner API: routes that use the objects
add_library(puddle_api STATIC
  src/api/conversations.cpp
  src/api/groups.cpp
  src/api/profiles.cpp
  src/api/request.cpp
  src/api/routes.cpp
  src/api/streams.cpp
  src/api/users.cpp)
target_link_libraries(puddle_api PUBLIC puddle_objects puddle_http)

# federation: the stream API: routes other Puddles call, and how our news reaches them
add_library(puddle_federation STATIC
  src/federation/access.cpp
  src/federation/courier.cpp
  src/federation/flow.cpp
  src/federation/groupapi.cpp
  src/federation/groupflow.cpp
  src/federation/inbound.cpp
  src/federation/keysync.cpp
  src/federation/messages.cpp
  src/federation/profiles_api.cpp
  src/federation/sender.cpp)
target_link_libraries(puddle_federation PUBLIC puddle_wire puddle_events puddle_http)

# service: the assembled Puddle (server.cpp) and every route together
add_library(puddle_service STATIC
  src/service/main.cpp
  src/service/routes.cpp
  src/service/server.cpp)
target_link_libraries(puddle_service PUBLIC puddle_api puddle_federation)

# --- puddled: headless service (systemd --user, launchd, ...) -----------------
add_executable(puddled src/service/main.cpp)
target_link_libraries(puddled PRIVATE puddle_service)
install(TARGETS puddled RUNTIME DESTINATION bin)

# --- Puddle: the desktop app (Qt). Embeds the server, so nothing else is needed.
option(PUDDLE_BUILD_GUI "Build the Qt desktop app" ON)
if(PUDDLE_BUILD_GUI)
  find_package(Qt6 COMPONENTS Quick QuickControls2 Network Svg Test QUIET) # Svg: the icons are SVG files
  if(Qt6_FOUND)
    qt_standard_project_setup()
    qt_policy(SET QTP0001 NEW)
    qt_policy(SET QTP0004 NEW)
    qt_add_executable(puddle WIN32 MACOSX_BUNDLE
      src/gui/main.cpp
      src/gui/app/backend.cpp
      src/gui/app/backend.h
      src/gui/app/chats.cpp
      src/gui/app/chats.h
      src/gui/app/apiclient.cpp
      src/gui/app/apiclient.h
      src/gui/app/jsonlistmodel.cpp
      src/gui/app/jsonlistmodel.h)
    file(GLOB PUDDLE_ICONS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS src/gui/assets/icons/tabler/*.svg)
    # Theme is a singleton: the colours and sizes everything else reads
    set_source_files_properties(src/gui/qml/theme/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
    qt_add_qml_module(puddle
      URI Puddle
      VERSION 1.0
      QML_FILES
        src/gui/qml/Main.qml
        src/gui/qml/setup/Setup.qml
        src/gui/qml/shell/Shell.qml
        src/gui/qml/shell/Sidebar.qml
        src/gui/qml/shell/SidebarRow.qml
        src/gui/qml/shell/HomePage.qml
        src/gui/qml/shell/ContextBar.qml
        src/gui/qml/dialogs/NewConversationDialog.qml
        src/gui/qml/chat/ChatPage.qml
        src/gui/qml/settings/PersonRow.qml
        src/gui/qml/settings/LogPage.qml
        src/gui/qml/components/Avatar.qml
        src/gui/qml/components/Button.qml
        src/gui/qml/components/Card.qml
        src/gui/qml/components/Dialog.qml
        src/gui/qml/components/DialogButtonBox.qml
        src/gui/qml/components/PrimaryButton.qml
        src/gui/qml/components/PanelToggle.qml
        src/gui/qml/components/TextField.qml
        src/gui/qml/components/ToolButton.qml
        src/gui/qml/components/ContextTrigger.qml
        src/gui/qml/components/FieldMenu.qml
        src/gui/qml/components/ConfirmDialog.qml
        src/gui/qml/components/ConversationMenu.qml
        src/gui/qml/components/Icon.qml
        src/gui/qml/components/ScrollArea.qml
        src/gui/qml/components/ScrollList.qml
        src/gui/qml/components/WheelScroll.qml
        src/gui/qml/theme/Theme.qml
      RESOURCES src/gui/assets/puddle.png ${PUDDLE_ICONS})
    target_link_libraries(puddle PRIVATE puddle_service Qt6::Quick Qt6::QuickControls2 Qt6::Network Qt6::Svg)
    if(APPLE)
      enable_language(OBJCXX)
      target_sources(puddle PRIVATE src/gui/app/dock_mac.mm)
      target_link_libraries(puddle PRIVATE "-framework AppKit")
    endif()
    set_target_properties(puddle PROPERTIES
      OUTPUT_NAME Puddle
      MACOSX_BUNDLE_BUNDLE_NAME Puddle
      MACOSX_BUNDLE_GUI_IDENTIFIER io.github.sloporation.puddle
      MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
    install(TARGETS puddle BUNDLE DESTINATION . RUNTIME DESTINATION bin)

    # The first-run state machine, against the real core, with no window (needs the offscreen platform).
    add_executable(backend_test tests/backend_test.cpp src/gui/app/backend.cpp src/gui/app/backend.h src/gui/app/chats.cpp src/gui/app/chats.h
      src/gui/app/apiclient.cpp src/gui/app/apiclient.h src/gui/app/jsonlistmodel.cpp src/gui/app/jsonlistmodel.h)
    target_include_directories(backend_test PRIVATE src/gui/app)
    target_link_libraries(backend_test PRIVATE puddle_service Qt6::Gui Qt6::Network)
    if(APPLE)
      target_sources(backend_test PRIVATE src/gui/app/dock_mac.mm)
      target_link_libraries(backend_test PRIVATE "-framework AppKit")
    endif()
    add_test(NAME backend COMMAND backend_test)

    # The app's view of a Puddle (owner API over pinned TLS), against two real Puddles talking to each other.
    add_executable(chats_test tests/chats_test.cpp src/gui/app/chats.cpp src/gui/app/chats.h src/gui/app/apiclient.cpp src/gui/app/apiclient.h
      src/gui/app/jsonlistmodel.cpp src/gui/app/jsonlistmodel.h)
    set_target_properties(chats_test PROPERTIES AUTOMOC ON)
    target_link_libraries(chats_test PRIVATE puddle_service Qt6::Core Qt6::Gui Qt6::Network Qt6::Test)
    add_test(NAME chats COMMAND chats_test)

    # The window, driven with real mouse and key events against two real Puddles.
    add_executable(ui_test tests/ui_test.cpp src/gui/app/backend.cpp src/gui/app/backend.h src/gui/app/chats.cpp src/gui/app/chats.h
      src/gui/app/apiclient.cpp src/gui/app/apiclient.h src/gui/app/jsonlistmodel.cpp src/gui/app/jsonlistmodel.h)
    set_target_properties(ui_test PROPERTIES AUTOMOC ON)
    target_include_directories(ui_test PRIVATE src/gui/app)
    target_compile_definitions(ui_test PRIVATE PUDDLE_GUI_DIR="${CMAKE_SOURCE_DIR}/src/gui/qml")
    target_link_libraries(ui_test PRIVATE puddle_service Qt6::Quick Qt6::QuickControls2 Qt6::Network Qt6::Test)
    if(APPLE)
      target_sources(ui_test PRIVATE src/gui/app/dock_mac.mm)
      target_link_libraries(ui_test PRIVATE "-framework AppKit")
    endif()
    add_test(NAME ui COMMAND ui_test)
    set_tests_properties(ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 180)
    set_tests_properties(chats PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
    set_tests_properties(backend PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  else()
    message(WARNING "Qt6 Quick not found; skipping the Puddle app (set -DPUDDLE_BUILD_GUI=OFF to silence)")
  endif()
endif()

# --- tests --------------------------------------------------------------------
enable_testing()
# Each layer of src/ includes only the layers it may (tests/layers.cmake).
add_test(NAME layers COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/src -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/layers.cmake)

foreach(t http json store storage people conversations config yaml profile encoding identity auth reqsig phrase serve router routes link group groupapi groupowner streams ratelimit access owner_streams client outbox inbound open_request flow message messages keysync wireprofile profiles e2e group_e2e)
  add_executable(${t}_test tests/${t}_test.c)
  target_link_libraries(${t}_test PRIVATE puddle_service)
  add_test(NAME ${t} COMMAND ${t}_test)
  # fill new and freed heap memory with garbage, so reading memory nobody wrote shows up as a failure
  set_tests_properties(${t} PROPERTIES ENVIRONMENT "MallocScribble=1;MALLOC_PERTURB_=165")
endforeach()

install(FILES dist/puddled.service DESTINATION share/puddle)
