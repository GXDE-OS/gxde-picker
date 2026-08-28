cmake_minimum_required(VERSION 3.16)
project(gxde-picker LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 自动处理 Qt 的 MOC、UIC、RCC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# ---------- 查找 Qt6 组件 ----------
find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Network
    DBus
    Svg
    LinguistTools
)

# ---------- DTK 依赖（dtk2widget + Dtk6）----------
find_package(PkgConfig REQUIRED)
pkg_check_modules(DTK2Widget REQUIRED dtk2widget)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(EGL REQUIRED egl)

# 使用 Dtk6 的核心和 GUI 模块（即使暂时未直接调用，保证兼容）
find_package(Dtk6Core REQUIRED)
find_package(Dtk6Gui REQUIRED)

# ---------- X11 / XCB 依赖（替代已移除的 Qt5::X11Extras）----------
pkg_check_modules(XCB REQUIRED xcb xcb-util)
find_package(X11 REQUIRED)          # 提供 X11::X11、Xext、Xtst 等

# ---------- 源文件与资源 ----------
set(SOURCES
    src/main.cpp
    src/picker.cpp
    src/eventmonitor.cpp
    src/colormenu.cpp
    src/clipboard.cpp
    src/animation.cpp
    src/dbusnotify.cpp
    src/settings.cpp
    src/utils.cpp
    src/waylandcapture.cpp
    src/protocols/kywc-capture-v1-protocol.c
    src/protocols/kywc-output-v1-protocol.c
)

set(HEADERS
    src/picker.h
    src/eventmonitor.h
    src/colormenu.h
    src/clipboard.h
    src/animation.h
    src/dbusnotify.h
    src/settings.h
    src/utils.h
    src/waylandcapture.h
    src/protocols/kywc-capture-v1-client-protocol.h
    src/protocols/kywc-output-v1-client-protocol.h
)

set(RESOURCES
    gxde-picker.qrc
)

# ---------- 可执行文件 ----------
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS} ${RESOURCES})

# ---------- 包含路径 ----------
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src          # 源码目录
    /usr/include/dtk2/DWidget                # dtk2widget 头文件
    ${Dtk6Core_INCLUDE_DIRS}
    ${Dtk6Gui_INCLUDE_DIRS}
    ${XCB_INCLUDE_DIRS}
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
)

# ---------- 链接库 ----------
target_link_libraries(${PROJECT_NAME}
    Qt6::Core
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Widgets
    Qt6::Network
    Qt6::DBus
    Qt6::Svg
    dtk2widget                              # 旧版 DTK 小部件（已支持 Qt6）
    Dtk6::Core                              # DTK6 核心
    Dtk6::Gui                               # DTK6 GUI
    ${XCB_LIBRARIES}                        # xcb 和 xcb-util
    X11::X11 X11::Xext X11::Xtst            # X11 扩展
    ${WAYLAND_CLIENT_LIBRARIES}
    ${EGL_LIBRARIES}
)

# ---------- 编译宏 ----------
target_compile_definitions(${PROJECT_NAME} PRIVATE
    DSR_LANG_PATH="${CMAKE_INSTALL_PREFIX}/share/${PROJECT_NAME}/translations"
)

# ---------- 翻译处理 ----------
add_custom_target(generate_translations ALL
    COMMAND sh -c "/usr/lib/qt6/bin/lupdate ./src/* -ts ./translations/*.ts -no-obsolete"
    COMMAND sh -c "/usr/lib/qt6/bin/lrelease ./translations/*.ts -qm ./translations/*.qm"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating translation files"
    VERBATIM
)

install(FILES ${QM_FILES}
    DESTINATION share/${PROJECT_NAME}/translations
)

# ---------- 安装 ----------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

install(FILES gxde-picker.desktop
    DESTINATION share/applications
)

install(FILES com.gxde.Picker.service
    DESTINATION share/dbus-1/services
)
