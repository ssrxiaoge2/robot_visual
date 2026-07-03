cmake_minimum_required(VERSION 3.16)

project(wh-robot-visual VERSION 0.2.6 LANGUAGES CXX)

# 让支持 CMake target 分组的 IDE（如 Qt Creator）把主程序和测试目标分开展示，
# 避免大量测试 target 挤占主工程列表空间。
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(PROJECT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

string(TIMESTAMP APP_BUILD_DATE "%Y-%m-%d")

add_compile_definitions(
    APP_VERSION="${PROJECT_VERSION}"
    APP_BUILD_DATE="${APP_BUILD_DATE}"
)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network SerialBus Test)
include(GNUInstallDirs)
enable_testing()

# 调度文件职责：lineconfig/taskqueue 定义模型，taskexecutor 执行单任务，
# linemanager 管理整线状态和 FIFO；它们与设备控制器编译进同一 Qt 程序。
set(PROJECT_SOURCES
        src/main.cpp
        src/mainwindow.cpp
        src/mainwindow.h
        src/mainwindow.ui
        src/workflowstep.h
        src/huayanScheduler.cpp
        src/huayanScheduler.h
        src/lineorchestrator.cpp
        src/lineorchestrator.h
        src/devicemanager.cpp
        src/devicemanager.h
        src/agvcontroller.cpp
        src/agvcontroller.h
        src/visionclient.cpp
        src/visionclient.h
        src/camerawindow.cpp
        src/camerawindow.h
        src/handeyedialog.cpp
        src/handeyedialog.h
        src/deviceindicator.cpp
        src/deviceindicator.h
        src/workflowwidget.cpp
        src/workflowwidget.h
        src/themeswitch.cpp
        src/themeswitch.h
        src/lineconfig.h
        src/taskqueue.h
        src/taskexecutor.cpp
        src/taskexecutor.h
        src/linemanager.cpp
        src/linemanager.h
        src/nscanscheduler.cpp
        src/nscanscheduler.h
        src/customSysScheduler.cpp
        src/customSysScheduler.h
        src/shortageconfig.h
        src/shortagecalculator.cpp
        src/shortagecalculator.h
        src/shortagetestpanel.cpp
        src/shortagetestpanel.h
        src/shortagetestsession.cpp
        src/shortagetestsession.h
        src/shortagemonitor.cpp
        src/shortagemonitor.h
        src/palletscheduler.cpp
        src/palletscheduler.h
        src/palletparamdialog.cpp
        src/palletparamdialog.h
)

qt_add_executable(wh-robot-visual
    MANUAL_FINALIZATION
    ${PROJECT_SOURCES}
)
set_target_properties(wh-robot-visual PROPERTIES FOLDER "robot_visual")

target_link_libraries(wh-robot-visual PRIVATE Qt6::Widgets Qt6::Network Qt6::SerialBus)
target_include_directories(wh-robot-visual PRIVATE src/)

function(assign_test_folder target_name)
    # 所有测试 target 在 IDE 中统一归入 tests 分组，
    # 物理文件位置仍保持现有 tests/ 目录，不影响编译与代码结构。
    set_target_properties(${target_name} PROPERTIES FOLDER "tests")
endfunction()

add_executable(shortage-calculator-test
    tests/test_shortagecalculator.cpp
    src/shortagecalculator.cpp
    src/shortagecalculator.h
    src/shortageconfig.h
)
assign_test_folder(shortage-calculator-test)
target_link_libraries(shortage-calculator-test PRIVATE Qt6::Test Qt6::Core)
target_include_directories(shortage-calculator-test PRIVATE src/)
add_test(NAME shortage-calculator COMMAND shortage-calculator-test)

add_executable(custom-system-parser-test
    tests/test_customsysscheduler.cpp
    src/customSysScheduler.cpp
    src/customSysScheduler.h
)
assign_test_folder(custom-system-parser-test)
target_link_libraries(custom-system-parser-test PRIVATE Qt6::Test Qt6::Core Qt6::Network)
target_include_directories(custom-system-parser-test PRIVATE src/)
add_test(NAME custom-system-parser COMMAND custom-system-parser-test)

add_executable(shortage-monitor-test
    tests/test_shortagemonitor.cpp
    src/shortagemonitor.cpp
    src/shortagemonitor.h
    src/shortagecalculator.cpp
    src/shortagecalculator.h
    src/shortageconfig.h
    src/customSysScheduler.cpp
    src/customSysScheduler.h
)
assign_test_folder(shortage-monitor-test)
target_link_libraries(shortage-monitor-test PRIVATE Qt6::Test Qt6::Core Qt6::Network)
target_include_directories(shortage-monitor-test PRIVATE src/)
add_test(NAME shortage-monitor COMMAND shortage-monitor-test)

add_executable(live-shortage-baseline-test
    tests/test_live_shortage_baseline.cpp
)
assign_test_folder(live-shortage-baseline-test)
target_link_libraries(live-shortage-baseline-test PRIVATE Qt6::Test Qt6::Core)
target_include_directories(live-shortage-baseline-test PRIVATE src/)
add_test(NAME live-shortage-baseline COMMAND live-shortage-baseline-test)

set(HUAYAN_SDK_ROOT "${CMAKE_SOURCE_DIR}/3rd/HuaYansdk/HuayanRobotLibrary-C++-V1.0.15.0")

add_executable(live-shortage-dispatch-test
    tests/test_live_shortage_dispatch.cpp
    src/agvcontroller.cpp
    src/agvcontroller.h
    src/huayanScheduler.cpp
    src/huayanScheduler.h
    src/linemanager.cpp
    src/linemanager.h
    src/taskexecutor.cpp
    src/taskexecutor.h
    src/taskqueue.h
    src/nscanscheduler.cpp
    src/nscanscheduler.h
    src/palletscheduler.cpp
    src/palletscheduler.h
)
assign_test_folder(live-shortage-dispatch-test)
target_link_libraries(live-shortage-dispatch-test PRIVATE Qt6::Test Qt6::Core Qt6::Network Qt6::SerialBus)
target_include_directories(live-shortage-dispatch-test PRIVATE src/)
add_test(NAME live-shortage-dispatch COMMAND live-shortage-dispatch-test)

target_include_directories(live-shortage-dispatch-test PRIVATE "${HUAYAN_SDK_ROOT}/include")
if(UNIX)
    target_link_directories(live-shortage-dispatch-test PRIVATE "${HUAYAN_SDK_ROOT}/Linux")
    target_link_libraries(live-shortage-dispatch-test PRIVATE "${HUAYAN_SDK_ROOT}/Linux/libHR_Pro.so")
elseif(WIN32)
    if(MSVC)
        target_link_directories(live-shortage-dispatch-test PRIVATE "${HUAYAN_SDK_ROOT}/MSVC")
        target_link_libraries(live-shortage-dispatch-test PRIVATE "${HUAYAN_SDK_ROOT}/MSVC/HR_Pro.lib")
    elseif(MINGW)
        target_link_directories(live-shortage-dispatch-test PRIVATE "${HUAYAN_SDK_ROOT}/MinGW")
        target_link_libraries(live-shortage-dispatch-test PRIVATE "${HUAYAN_SDK_ROOT}/MinGW/libHR_Pro.dll.a")
    endif()
endif()

add_executable(shortage-test-session-test
    tests/test_shortagetestsession.cpp
    src/shortagetestsession.cpp
    src/shortagetestsession.h
    src/shortagecalculator.cpp
    src/shortagecalculator.h
    src/shortageconfig.h
    src/customSysScheduler.cpp
    src/customSysScheduler.h
)
assign_test_folder(shortage-test-session-test)
target_link_libraries(shortage-test-session-test PRIVATE Qt6::Test Qt6::Core Qt6::Network)
target_include_directories(shortage-test-session-test PRIVATE src/)
add_test(NAME shortage-test-session COMMAND shortage-test-session-test)

add_executable(shortage-test-wiring-test
    tests/test_shortagetestwiring.cpp
    src/devicemanager.cpp
    src/devicemanager.h
    src/agvcontroller.cpp
    src/agvcontroller.h
    src/visionclient.cpp
    src/visionclient.h
    src/huayanScheduler.cpp
    src/huayanScheduler.h
    src/lineorchestrator.cpp
    src/lineorchestrator.h
    src/linemanager.cpp
    src/linemanager.h
    src/taskexecutor.cpp
    src/taskexecutor.h
    src/taskqueue.h
    src/nscanscheduler.cpp
    src/nscanscheduler.h
    src/customSysScheduler.cpp
    src/customSysScheduler.h
    src/shortagecalculator.cpp
    src/shortagecalculator.h
    src/shortageconfig.h
    src/shortagetestsession.cpp
    src/shortagetestsession.h
    src/shortagemonitor.cpp
    src/shortagemonitor.h
    src/palletscheduler.cpp
    src/palletscheduler.h
)
assign_test_folder(shortage-test-wiring-test)
target_link_libraries(shortage-test-wiring-test PRIVATE Qt6::Test Qt6::Core Qt6::Widgets Qt6::Network Qt6::SerialBus)
target_include_directories(shortage-test-wiring-test PRIVATE src/)
add_test(NAME shortage-test-wiring COMMAND shortage-test-wiring-test)
set_tests_properties(shortage-test-wiring PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

target_include_directories(shortage-test-wiring-test PRIVATE "${HUAYAN_SDK_ROOT}/include")
if(UNIX)
    target_link_directories(shortage-test-wiring-test PRIVATE "${HUAYAN_SDK_ROOT}/Linux")
    target_link_libraries(shortage-test-wiring-test PRIVATE "${HUAYAN_SDK_ROOT}/Linux/libHR_Pro.so")
elseif(WIN32)
    if(MSVC)
        target_link_directories(shortage-test-wiring-test PRIVATE "${HUAYAN_SDK_ROOT}/MSVC")
        target_link_libraries(shortage-test-wiring-test PRIVATE "${HUAYAN_SDK_ROOT}/MSVC/HR_Pro.lib")
    elseif(MINGW)
        target_link_directories(shortage-test-wiring-test PRIVATE "${HUAYAN_SDK_ROOT}/MinGW")
        target_link_libraries(shortage-test-wiring-test PRIVATE "${HUAYAN_SDK_ROOT}/MinGW/libHR_Pro.dll.a")
    endif()
endif()

add_executable(live-shortage-wiring-test
    tests/test_live_shortage_wiring.cpp
    src/devicemanager.cpp
    src/devicemanager.h
    src/agvcontroller.cpp
    src/agvcontroller.h
    src/visionclient.cpp
    src/visionclient.h
    src/huayanScheduler.cpp
    src/huayanScheduler.h
    src/lineorchestrator.cpp
    src/lineorchestrator.h
    src/linemanager.cpp
    src/linemanager.h
    src/taskexecutor.cpp
    src/taskexecutor.h
    src/taskqueue.h
    src/nscanscheduler.cpp
    src/nscanscheduler.h
    src/customSysScheduler.cpp
    src/customSysScheduler.h
    src/shortagecalculator.cpp
    src/shortagecalculator.h
    src/shortageconfig.h
    src/shortagetestsession.cpp
    src/shortagetestsession.h
    src/shortagemonitor.cpp
    src/shortagemonitor.h
    src/palletscheduler.cpp
    src/palletscheduler.h
)
assign_test_folder(live-shortage-wiring-test)
target_link_libraries(live-shortage-wiring-test PRIVATE Qt6::Test Qt6::Core Qt6::Widgets Qt6::Network Qt6::SerialBus)
target_include_directories(live-shortage-wiring-test PRIVATE src/)
add_test(NAME live-shortage-wiring COMMAND live-shortage-wiring-test)
set_tests_properties(live-shortage-wiring PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

target_include_directories(live-shortage-wiring-test PRIVATE "${HUAYAN_SDK_ROOT}/include")
if(UNIX)
    target_link_directories(live-shortage-wiring-test PRIVATE "${HUAYAN_SDK_ROOT}/Linux")
    target_link_libraries(live-shortage-wiring-test PRIVATE "${HUAYAN_SDK_ROOT}/Linux/libHR_Pro.so")
elseif(WIN32)
    if(MSVC)
        target_link_directories(live-shortage-wiring-test PRIVATE "${HUAYAN_SDK_ROOT}/MSVC")
        target_link_libraries(live-shortage-wiring-test PRIVATE "${HUAYAN_SDK_ROOT}/MSVC/HR_Pro.lib")
    elseif(MINGW)
        target_link_directories(live-shortage-wiring-test PRIVATE "${HUAYAN_SDK_ROOT}/MinGW")
        target_link_libraries(live-shortage-wiring-test PRIVATE "${HUAYAN_SDK_ROOT}/MinGW/libHR_Pro.dll.a")
    endif()
endif()

add_executable(shortage-test-ui-test
    tests/test_shortagetestui.cpp
    src/shortagetestpanel.cpp
    src/shortagetestpanel.h
    src/deviceindicator.cpp
    src/deviceindicator.h
    src/shortageconfig.h
    src/shortagecalculator.h
)
assign_test_folder(shortage-test-ui-test)
target_link_libraries(shortage-test-ui-test PRIVATE Qt6::Test Qt6::Core Qt6::Widgets)
target_include_directories(shortage-test-ui-test PRIVATE src/)
add_test(NAME shortage-test-ui COMMAND shortage-test-ui-test)
set_tests_properties(shortage-test-ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(live-shortage-ui-test
    tests/test_live_shortage_ui.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/mainwindow.ui
    src/devicemanager.cpp
    src/devicemanager.h
    src/deviceindicator.cpp
    src/deviceindicator.h
    src/agvcontroller.cpp
    src/agvcontroller.h
    src/visionclient.cpp
    src/visionclient.h
    src/huayanScheduler.cpp
    src/huayanScheduler.h
    src/lineorchestrator.cpp
    src/lineorchestrator.h
    src/linemanager.cpp
    src/linemanager.h
    src/taskexecutor.cpp
    src/taskexecutor.h
    src/taskqueue.h
    src/nscanscheduler.cpp
    src/nscanscheduler.h
    src/customSysScheduler.cpp
    src/customSysScheduler.h
    src/shortagecalculator.cpp
    src/shortagecalculator.h
    src/shortageconfig.h
    src/shortagetestsession.cpp
    src/shortagetestsession.h
    src/shortagetestpanel.cpp
    src/shortagetestpanel.h
    src/shortagemonitor.cpp
    src/shortagemonitor.h
    src/themeswitch.cpp
    src/themeswitch.h
    src/workflowwidget.cpp
    src/workflowwidget.h
    src/camerawindow.cpp
    src/camerawindow.h
    src/handeyedialog.cpp
    src/handeyedialog.h
    src/palletscheduler.cpp
    src/palletscheduler.h
    src/palletparamdialog.cpp
    src/palletparamdialog.h
)
assign_test_folder(live-shortage-ui-test)
target_link_libraries(live-shortage-ui-test PRIVATE Qt6::Test Qt6::Core Qt6::Widgets Qt6::Network Qt6::SerialBus)
target_include_directories(live-shortage-ui-test PRIVATE src/)
add_test(NAME live-shortage-ui COMMAND live-shortage-ui-test)
set_tests_properties(live-shortage-ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

target_include_directories(live-shortage-ui-test PRIVATE "${HUAYAN_SDK_ROOT}/include")
if(UNIX)
    target_link_directories(live-shortage-ui-test PRIVATE "${HUAYAN_SDK_ROOT}/Linux")
    target_link_libraries(live-shortage-ui-test PRIVATE "${HUAYAN_SDK_ROOT}/Linux/libHR_Pro.so")
elseif(WIN32)
    if(MSVC)
        target_link_directories(live-shortage-ui-test PRIVATE "${HUAYAN_SDK_ROOT}/MSVC")
        target_link_libraries(live-shortage-ui-test PRIVATE "${HUAYAN_SDK_ROOT}/MSVC/HR_Pro.lib")
    elseif(MINGW)
        target_link_directories(live-shortage-ui-test PRIVATE "${HUAYAN_SDK_ROOT}/MinGW")
        target_link_libraries(live-shortage-ui-test PRIVATE "${HUAYAN_SDK_ROOT}/MinGW/libHR_Pro.dll.a")
    endif()
endif()

# ── 奥比中光 SDK（仅 Linux 部署时链接，Windows 开发环境跳过）──
if(UNIX)
    set(ORBBEC_SDK_ROOT "${CMAKE_SOURCE_DIR}/3rd/OrbbecSdk")
    target_include_directories(wh-robot-visual PRIVATE "${ORBBEC_SDK_ROOT}/include")
    target_include_directories(live-shortage-ui-test PRIVATE "${ORBBEC_SDK_ROOT}/include")
    target_link_directories(wh-robot-visual PRIVATE "${ORBBEC_SDK_ROOT}/lib")
    target_link_directories(live-shortage-ui-test PRIVATE "${ORBBEC_SDK_ROOT}/lib")
    target_link_libraries(wh-robot-visual PRIVATE
        "${ORBBEC_SDK_ROOT}/lib/libOrbbecSDK.so.1.10.35"
    )
    target_link_libraries(live-shortage-ui-test PRIVATE
        "${ORBBEC_SDK_ROOT}/lib/libOrbbecSDK.so.1.10.35"
    )
    set_target_properties(wh-robot-visual PROPERTIES
        BUILD_RPATH "${ORBBEC_SDK_ROOT}/lib"
        INSTALL_RPATH "${ORBBEC_SDK_ROOT}/lib"
    )
    set_target_properties(live-shortage-ui-test PROPERTIES
        BUILD_RPATH "${ORBBEC_SDK_ROOT}/lib"
        INSTALL_RPATH "${ORBBEC_SDK_ROOT}/lib"
    )
endif()

# ── N-ScanHub 扫码枪 SDK（网络扫码，Linux / Windows MSVC）──
set(NSCAN_SDK_ROOT "${CMAKE_SOURCE_DIR}/3rd/NScanHubSdk")

if(UNIX AND NOT APPLE)
    set(NSCAN_PLATFORM_ROOT "${NSCAN_SDK_ROOT}/N-ScanHubForLinux_v1.0.05")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(NSCAN_ARCH x64)
    else()
        set(NSCAN_ARCH x32)
    endif()

    set(NSCAN_LIBRARY_SOURCE "${NSCAN_PLATFORM_ROOT}/lib/${NSCAN_ARCH}/N-ScanHub.so")
    set(NSCAN_LIBUSB_SOURCE "${NSCAN_PLATFORM_ROOT}/libusb/${NSCAN_ARCH}/libusb-1.0.so")
    set(NSCAN_RUNTIME_DIR "${CMAKE_CURRENT_BINARY_DIR}/nscan-runtime")

    if(NOT EXISTS "${NSCAN_LIBRARY_SOURCE}" OR NOT EXISTS "${NSCAN_LIBUSB_SOURCE}")
        message(FATAL_ERROR "N-ScanHub Linux ${NSCAN_ARCH} SDK files are incomplete")
    endif()

    # SDK 依赖的 SONAME 是 libusb-1.0.so.0，但厂商包文件名缺少末尾 .0；
    # 在构建目录暂存正确名称，避免修改 3rd 下的原始 SDK 文件。
    file(MAKE_DIRECTORY "${NSCAN_RUNTIME_DIR}")
    configure_file("${NSCAN_LIBRARY_SOURCE}"
                   "${NSCAN_RUNTIME_DIR}/libN-ScanHub.so" COPYONLY)
    configure_file("${NSCAN_LIBUSB_SOURCE}"
                   "${NSCAN_RUNTIME_DIR}/libusb-1.0.so.0" COPYONLY)

    target_include_directories(wh-robot-visual PRIVATE "${NSCAN_PLATFORM_ROOT}/include")
    target_include_directories(live-shortage-ui-test PRIVATE "${NSCAN_PLATFORM_ROOT}/include")
    target_link_libraries(wh-robot-visual PRIVATE
        "-Wl,--no-as-needed"
        "${NSCAN_RUNTIME_DIR}/libN-ScanHub.so"
        "${NSCAN_RUNTIME_DIR}/libusb-1.0.so.0"
        "-Wl,--as-needed"
    )
    target_link_libraries(live-shortage-ui-test PRIVATE
        "-Wl,--no-as-needed"
        "${NSCAN_RUNTIME_DIR}/libN-ScanHub.so"
        "${NSCAN_RUNTIME_DIR}/libusb-1.0.so.0"
        "-Wl,--as-needed"
    )
    target_compile_definitions(wh-robot-visual PRIVATE NSCAN_SDK_AVAILABLE=1)
    target_compile_definitions(live-shortage-ui-test PRIVATE NSCAN_SDK_AVAILABLE=1)

    # 使用 APPEND，保留前面已经配置的 Orbbec SDK RPATH。
    set_property(TARGET wh-robot-visual APPEND PROPERTY BUILD_RPATH
        "${NSCAN_RUNTIME_DIR}"
    )
    set_property(TARGET live-shortage-ui-test APPEND PROPERTY BUILD_RPATH
        "${NSCAN_RUNTIME_DIR}"
    )
    set_property(TARGET wh-robot-visual APPEND PROPERTY INSTALL_RPATH
        "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
    )
    set_property(TARGET live-shortage-ui-test APPEND PROPERTY INSTALL_RPATH
        "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
    )

    install(FILES
        "${NSCAN_RUNTIME_DIR}/libN-ScanHub.so"
        "${NSCAN_RUNTIME_DIR}/libusb-1.0.so.0"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    )
elseif(WIN32 AND MSVC)
    set(NSCAN_PLATFORM_ROOT
        "${NSCAN_SDK_ROOT}/N-ScanHubForWindows_v1.0.05/N-ScanHubForWindows")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(NSCAN_ARCH x64)
    else()
        set(NSCAN_ARCH x86)
    endif()

    set(NSCAN_LIBRARY "${NSCAN_PLATFORM_ROOT}/lib/${NSCAN_ARCH}/N-ScanHub.lib")
    set(NSCAN_RUNTIME "${NSCAN_PLATFORM_ROOT}/lib/${NSCAN_ARCH}/N-ScanHub.dll")
    if(NOT EXISTS "${NSCAN_LIBRARY}" OR NOT EXISTS "${NSCAN_RUNTIME}")
        message(FATAL_ERROR "N-ScanHub Windows ${NSCAN_ARCH} SDK files are incomplete")
    endif()

    target_include_directories(wh-robot-visual PRIVATE "${NSCAN_PLATFORM_ROOT}/include")
    target_include_directories(live-shortage-ui-test PRIVATE "${NSCAN_PLATFORM_ROOT}/include")
    target_link_libraries(wh-robot-visual PRIVATE "${NSCAN_LIBRARY}")
    target_link_libraries(live-shortage-ui-test PRIVATE "${NSCAN_LIBRARY}")
    target_compile_definitions(wh-robot-visual PRIVATE NSCAN_SDK_AVAILABLE=1)
    target_compile_definitions(live-shortage-ui-test PRIVATE NSCAN_SDK_AVAILABLE=1)

    add_custom_command(TARGET wh-robot-visual POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${NSCAN_RUNTIME}"
                "$<TARGET_FILE_DIR:wh-robot-visual>/N-ScanHub.dll"
        COMMENT "Copying N-ScanHub runtime DLL"
    )
    install(FILES "${NSCAN_RUNTIME}" DESTINATION "${CMAKE_INSTALL_BINDIR}")
elseif(WIN32 AND MINGW)
    message(STATUS "N-ScanHub SDK disabled: vendor package has no MinGW import library")
else()
    message(STATUS "N-ScanHub SDK disabled on this platform")
endif()

# ── 华沿机械臂 SDK（Linux/Windows 均可配置）──
set(HUAYAN_SDK_ROOT "${CMAKE_SOURCE_DIR}/3rd/HuaYansdk/HuayanRobotLibrary-C++-V1.0.15.0")

target_include_directories(wh-robot-visual PRIVATE "${HUAYAN_SDK_ROOT}/include")

if(UNIX)
    target_link_directories(wh-robot-visual PRIVATE "${HUAYAN_SDK_ROOT}/Linux")
    target_link_libraries(wh-robot-visual PRIVATE "${HUAYAN_SDK_ROOT}/Linux/libHR_Pro.so")
elseif(WIN32)
    if(MSVC)
        target_link_directories(wh-robot-visual PRIVATE "${HUAYAN_SDK_ROOT}/MSVC")
        target_link_libraries(wh-robot-visual PRIVATE "${HUAYAN_SDK_ROOT}/MSVC/HR_Pro.lib")
    elseif(MINGW)
        target_link_directories(wh-robot-visual PRIVATE "${HUAYAN_SDK_ROOT}/MinGW")
        target_link_libraries(wh-robot-visual PRIVATE "${HUAYAN_SDK_ROOT}/MinGW/libHR_Pro.dll.a")
    endif()
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

install(TARGETS wh-robot-visual
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

qt_finalize_executable(wh-robot-visual)
