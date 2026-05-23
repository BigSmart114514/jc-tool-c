cmake_minimum_required(VERSION 3.16)

project(qtermwidget LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(QTERMWIDGET_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../qtermwidget-2.4.0/lib")

set(QTERMWIDGET_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/Emulation.cpp
    ${QTERMWIDGET_SRC_DIR}/Vt102Emulation.cpp
    ${QTERMWIDGET_SRC_DIR}/Screen.cpp
    ${QTERMWIDGET_SRC_DIR}/ScreenWindow.cpp
    ${QTERMWIDGET_SRC_DIR}/TerminalDisplay.cpp
    ${QTERMWIDGET_SRC_DIR}/ColorScheme.cpp
    ${QTERMWIDGET_SRC_DIR}/KeyboardTranslator.cpp
    ${QTERMWIDGET_SRC_DIR}/Filter.cpp
    ${QTERMWIDGET_SRC_DIR}/TerminalCharacterDecoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/konsole_wcwidth.cpp
    ${QTERMWIDGET_SRC_DIR}/SearchBar.cpp
    ${QTERMWIDGET_SRC_DIR}/tools.cpp
    ${QTERMWIDGET_SRC_DIR}/HistorySearch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/History.cpp
)

set(QTERMWIDGET_HDRS
    ${QTERMWIDGET_SRC_DIR}/Emulation.h
    ${QTERMWIDGET_SRC_DIR}/Vt102Emulation.h
    ${QTERMWIDGET_SRC_DIR}/Screen.h
    ${QTERMWIDGET_SRC_DIR}/ScreenWindow.h
    ${QTERMWIDGET_SRC_DIR}/TerminalDisplay.h
    ${QTERMWIDGET_SRC_DIR}/ColorScheme.h
    ${QTERMWIDGET_SRC_DIR}/KeyboardTranslator.h
    ${QTERMWIDGET_SRC_DIR}/Filter.h
    ${QTERMWIDGET_SRC_DIR}/TerminalCharacterDecoder.h
    ${QTERMWIDGET_SRC_DIR}/konsole_wcwidth.h
    ${QTERMWIDGET_SRC_DIR}/SearchBar.h
    ${QTERMWIDGET_SRC_DIR}/tools.h
    ${QTERMWIDGET_SRC_DIR}/HistorySearch.h
    ${QTERMWIDGET_SRC_DIR}/Character.h
    ${QTERMWIDGET_SRC_DIR}/CharacterColor.h
    ${QTERMWIDGET_SRC_DIR}/ColorTables.h
    ${QTERMWIDGET_SRC_DIR}/LineFont.h
    ${QTERMWIDGET_SRC_DIR}/DefaultTranslatorText.h
    ${QTERMWIDGET_SRC_DIR}/ExtendedDefaultTranslator.h
    ${QTERMWIDGET_SRC_DIR}/qtermwidget_interface.h
    ${CMAKE_CURRENT_SOURCE_DIR}/History.h
    ${CMAKE_CURRENT_SOURCE_DIR}/qtermwidget_export.h
    ${CMAKE_CURRENT_SOURCE_DIR}/qtermwidget_version.h
)

set(QTERMWIDGET_UI
    ${QTERMWIDGET_SRC_DIR}/SearchBar.ui
)

add_library(qtermwidget STATIC ${QTERMWIDGET_SRCS} ${QTERMWIDGET_HDRS} ${QTERMWIDGET_UI})

target_include_directories(qtermwidget
    PUBLIC
        ${QTERMWIDGET_SRC_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${QTERMWIDGET_SRC_DIR}
)

target_link_libraries(qtermwidget PUBLIC Qt6::Widgets)

# These paths are only used to find keytab/color-scheme files at runtime; as a fallback
# we set them to the source directory so the files can be found.
target_compile_definitions(qtermwidget PRIVATE
    KB_LAYOUT_DIR="${QTERMWIDGET_SRC_DIR}/kb-layouts"
    COLORSCHEMES_DIR="${QTERMWIDGET_SRC_DIR}/color-schemes"
    TRANSLATIONS_DIR="${QTERMWIDGET_SRC_DIR}/translations"
)

# Fix: Emulation.cpp has commented-out include of Session.h/unistd.h — don't need
# Fix: Screen.cpp includes unistd.h but doesn't use it on Windows

# Suppress MSVC warnings from qtermwidget source
target_compile_options(qtermwidget PRIVATE /wd4100 /wd4701 /wd4702)
