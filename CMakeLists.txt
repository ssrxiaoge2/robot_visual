cmake_minimum_required(VERSION 3.16)

project(wh-robot-visual VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(PROJECT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

find_package(Qt6 REQUIRED COMPONENTS Widgets Network SerialBus)

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
        mainwindow.ui
        workflowengine.cpp
        workflowengine.h
        devicemanager.cpp
        devicemanager.h
        agvcontroller.cpp
        agvcontroller.h
        themeswitch.cpp
        themeswitch.h
        deviceindicator.cpp
        deviceindicator.h
        workflowwidget.cpp
        workflowwidget.h
        workflowstep.h
        camerawindow.cpp
        camerawindow.h
        robotcontroller.cpp
        robotcontroller.h
)

qt_add_executable(wh-robot-visual
    MANUAL_FINALIZATION
    ${PROJECT_SOURCES}
)

target_link_libraries(wh-robot-visual PRIVATE Qt6::Widgets Qt6::Network Qt6::SerialBus)

# ── 奥比中光 SDK（仅 Linux 部署时链接，Windows 开发环境跳过）──
if(UNIX)
    set(ORBBEC_SDK_ROOT "${CMAKE_SOURCE_DIR}/../helper/Project11/OrbbecSDK_C_C++_v1.10.35_20260128_fa838f5_linux_x64_release/OrbbecSDK_v1.10.35/SDK")
    target_include_directories(wh-robot-visual PRIVATE "${ORBBEC_SDK_ROOT}/include")
    target_link_directories(wh-robot-visual PRIVATE "${ORBBEC_SDK_ROOT}/lib")
    target_link_libraries(wh-robot-visual PRIVATE
        "${ORBBEC_SDK_ROOT}/lib/libOrbbecSDK.so.1.10.35"
    )
    set_target_properties(wh-robot-visual PROPERTIES
        BUILD_RPATH "${ORBBEC_SDK_ROOT}/lib"
        INSTALL_RPATH "${ORBBEC_SDK_ROOT}/lib"
    )
endif()

# Windows：隐藏控制台窗口；Linux/macOS 不需要此属性
if(WIN32)
    set_target_properties(wh-robot-visual PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
endif()
if(APPLE)
    set_target_properties(wh-robot-visual PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    )
endif()

include(GNUInstallDirs)
install(TARGETS wh-robot-visual
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

qt_finalize_executable(wh-robot-visual)
