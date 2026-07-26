find_package(Qt6 REQUIRED COMPONENTS Core Test Network Gui)

add_executable(auto_charge_coordinator_tests
    test_auto_charge_coordinator.cpp
    ../src/autochargecoordinator.cpp
    ../src/chargesettings.cpp
    # 协调器信号直接复用控制器的来源/停止原因强类型；链接现有控制器实现，
    # 仅用于提供其 QMetaObject，不在本测试中建立真实连接或发送任何写命令。
    ../src/chargepilecontroller.cpp
    ../src/chargepileprotocol.cpp
    ../src/agvmonitorfreshnessguard.cpp
)

target_include_directories(auto_charge_coordinator_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(auto_charge_coordinator_tests PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::SerialBus
    Qt6::Test
)

add_test(NAME auto_charge_coordinator_tests
         COMMAND auto_charge_coordinator_tests)

add_executable(charge_pile_controller_tests
    test_charge_pile_controller.cpp
    ../src/chargesettings.cpp
    ../src/chargepileprotocol.cpp
    ../src/chargepilecontroller.cpp
)

target_include_directories(charge_pile_controller_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(charge_pile_controller_tests PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Test
)

add_test(NAME charge_pile_controller_tests
         COMMAND charge_pile_controller_tests)

add_executable(charge_pile_protocol_tests
    test_charge_pile_protocol.cpp
    ../src/chargepileprotocol.cpp
)

target_include_directories(charge_pile_protocol_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(charge_pile_protocol_tests PRIVATE
    Qt6::Core
    Qt6::Test
)

add_test(NAME charge_pile_protocol_tests
         COMMAND charge_pile_protocol_tests)

add_executable(charge_settings_tests
    test_charge_settings.cpp
    ../src/chargesettings.cpp
)

target_include_directories(charge_settings_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(charge_settings_tests PRIVATE
    Qt6::Core
    Qt6::Test
)

add_test(NAME charge_settings_tests
         COMMAND charge_settings_tests)

add_executable(charge_settings_transaction_tests
    test_charge_settings_transaction.cpp
    ../src/chargesettings.cpp
    ../src/chargesettingstransaction.cpp
    ../src/chargepilecontroller.cpp
    ../src/chargepileprotocol.cpp
    ../src/autochargecoordinator.cpp
)

target_include_directories(charge_settings_transaction_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(charge_settings_transaction_tests PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::SerialBus
    Qt6::Test
)

add_test(NAME charge_settings_transaction_tests
         COMMAND charge_settings_transaction_tests)

add_executable(vision_alignment_decision_tests
    test_vision_alignment_decision.cpp
    ../src/visionalignmentdecision.cpp
)

target_include_directories(vision_alignment_decision_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(vision_alignment_decision_tests PRIVATE
    Qt6::Core
    Qt6::Test
)

add_test(NAME vision_alignment_decision_tests
         COMMAND vision_alignment_decision_tests)

add_executable(vision_alignment_safety_tests
    test_vision_alignment_safety.cpp
    ../src/visionalignmentdecision.cpp
)

target_include_directories(vision_alignment_safety_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(vision_alignment_safety_tests PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Test
)

add_test(NAME vision_alignment_safety_tests
         COMMAND vision_alignment_safety_tests)

add_executable(visionclient_generation_tests
    test_visionclient_generation.cpp
    ../src/visionclient.cpp
    ../src/runtimesettings.cpp
)

target_include_directories(visionclient_generation_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(visionclient_generation_tests PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Test
)

add_test(NAME visionclient_generation_tests
         COMMAND visionclient_generation_tests)

add_executable(vision_failure_signal_integration_tests
    test_vision_failure_signal_integration.cpp
    ../src/taskexecutor.cpp
    ../src/huayanScheduler.cpp
    ../src/agvcontroller.cpp
    ../src/palletscheduler.cpp
    ../src/palletplacesequence.cpp
    ../src/visionclient.cpp
    ../src/visionalignmentdecision.cpp
    ../src/runtimesettings.cpp
)

target_include_directories(vision_failure_signal_integration_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/3rd/HuaYansdk/HuayanRobotLibrary-C++-V1.0.15.0/include"
)

target_link_libraries(vision_failure_signal_integration_tests PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::SerialBus
    Qt6::Test
)

# 集成测试会真正装载 HuayanScheduler 目标文件，因此 Windows 启动测试进程时
# 必须能在可执行文件同目录找到厂商运行库；这里只复制 DLL，不调用任何机器人接口。
set(HUAYAN_TEST_SDK_ROOT
    "${CMAKE_SOURCE_DIR}/3rd/HuaYansdk/HuayanRobotLibrary-C++-V1.0.15.0")
if(WIN32 AND MSVC)
    target_link_libraries(vision_failure_signal_integration_tests PRIVATE
        "${HUAYAN_TEST_SDK_ROOT}/MSVC/HR_Pro.lib"
    )
    add_custom_command(TARGET vision_failure_signal_integration_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${HUAYAN_TEST_SDK_ROOT}/MSVC/HR_Pro.dll"
            "$<TARGET_FILE_DIR:vision_failure_signal_integration_tests>/HR_Pro.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${HUAYAN_TEST_SDK_ROOT}/MSVC/pthreadVC2.dll"
            "$<TARGET_FILE_DIR:vision_failure_signal_integration_tests>/pthreadVC2.dll"
        COMMENT "复制华沿集成测试运行时 DLL"
    )
elseif(WIN32 AND MINGW)
    target_link_libraries(vision_failure_signal_integration_tests PRIVATE
        "${HUAYAN_TEST_SDK_ROOT}/MinGW/libHR_Pro.dll.a"
    )
    add_custom_command(TARGET vision_failure_signal_integration_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${HUAYAN_TEST_SDK_ROOT}/MinGW/libHR_Pro.dll"
            "$<TARGET_FILE_DIR:vision_failure_signal_integration_tests>/libHR_Pro.dll"
        COMMENT "复制华沿 MinGW 集成测试运行时 DLL"
    )
elseif(UNIX)
    target_link_libraries(vision_failure_signal_integration_tests PRIVATE
        "${HUAYAN_TEST_SDK_ROOT}/Linux/libHR_Pro.so"
    )
    set_property(TARGET vision_failure_signal_integration_tests APPEND PROPERTY
        BUILD_RPATH "${HUAYAN_TEST_SDK_ROOT}/Linux"
    )
endif()

add_test(NAME vision_failure_signal_integration_tests
         COMMAND vision_failure_signal_integration_tests)

add_executable(runtime_settings_tests
    test_runtime_settings.cpp
    ../src/runtimesettings.cpp
)

target_include_directories(runtime_settings_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(runtime_settings_tests PRIVATE
    Qt6::Core
    Qt6::Test
)

add_test(NAME runtime_settings_tests
         COMMAND runtime_settings_tests)

add_executable(settings_manager_tests
    test_settings_manager.cpp
    ../src/settingsmanager.cpp
    ../src/runtimesettings.cpp
)

target_include_directories(settings_manager_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(settings_manager_tests PRIVATE
    Qt6::Core
    Qt6::Test
)

add_test(NAME settings_manager_tests
         COMMAND settings_manager_tests)

add_executable(station_pickup_config_tests
    test_station_pickup_config.cpp
)

target_include_directories(station_pickup_config_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(station_pickup_config_tests PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
)

add_test(NAME station_pickup_config_tests
         COMMAND station_pickup_config_tests)

add_executable(pallet_place_sequence_tests
    test_pallet_place_sequence.cpp
    ../src/palletscheduler.cpp
    ../src/palletplacesequence.cpp
)

target_include_directories(pallet_place_sequence_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(pallet_place_sequence_tests PRIVATE
    Qt6::Core
    Qt6::Test
)

add_test(NAME pallet_place_sequence_tests
         COMMAND pallet_place_sequence_tests)

add_executable(pallet_scheduler_tests
    test_pallet_scheduler.cpp
    ../src/palletscheduler.cpp
)

target_include_directories(pallet_scheduler_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(pallet_scheduler_tests PRIVATE
    Qt6::Core
    Qt6::Test
)

add_test(NAME pallet_scheduler_tests
         COMMAND pallet_scheduler_tests)

add_executable(pallet_occupancy_removal_contract_tests
    test_pallet_occupancy_removal_contract.cpp
)

target_include_directories(pallet_occupancy_removal_contract_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(pallet_occupancy_removal_contract_tests PRIVATE
    Qt6::Core
)

add_test(NAME pallet_occupancy_removal_contract_tests
         COMMAND pallet_occupancy_removal_contract_tests)

add_executable(huayan_scheduler_contract_tests
    test_huayan_scheduler_contract.cpp
)

target_include_directories(huayan_scheduler_contract_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(huayan_scheduler_contract_tests PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Test
)

add_test(NAME huayan_scheduler_contract_tests
         COMMAND huayan_scheduler_contract_tests)

add_executable(vision_joint_motion_contract_tests
    test_vision_joint_motion_contract.cpp
)

target_include_directories(vision_joint_motion_contract_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(vision_joint_motion_contract_tests PRIVATE
    Qt6::Core
)

add_test(NAME vision_joint_motion_contract_tests
         COMMAND vision_joint_motion_contract_tests)

add_executable(task_executor_pallet_commit_semantics_tests
    test_task_executor_pallet_commit_semantics.cpp
)

target_include_directories(task_executor_pallet_commit_semantics_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(task_executor_pallet_commit_semantics_tests PRIVATE
    Qt6::Core
)

add_test(NAME task_executor_pallet_commit_semantics_tests
         COMMAND task_executor_pallet_commit_semantics_tests)

add_executable(pallet_param_dialog_contract_tests
    test_pallet_param_dialog_contract.cpp
)

target_include_directories(pallet_param_dialog_contract_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(pallet_param_dialog_contract_tests PRIVATE
    Qt6::Core
)

add_test(NAME pallet_param_dialog_contract_tests
         COMMAND pallet_param_dialog_contract_tests)

add_executable(vision_target_selection_tests
    test_vision_target_selection.cpp
    ../src/visionclient.cpp
    ../src/runtimesettings.cpp
)

target_include_directories(vision_target_selection_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(vision_target_selection_tests PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
)

add_test(NAME vision_target_selection_tests
         COMMAND vision_target_selection_tests)

add_executable(anchor_target_selection_tests
    test_anchor_target_selection.cpp
    ../src/visionclient.cpp
    ../src/runtimesettings.cpp
)

target_include_directories(anchor_target_selection_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(anchor_target_selection_tests PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
)

add_test(NAME anchor_target_selection_tests
         COMMAND anchor_target_selection_tests)

add_executable(locked_target_selection_tests
    test_locked_target_selection.cpp
    ../src/visionclient.cpp
    ../src/runtimesettings.cpp
)

target_include_directories(locked_target_selection_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(locked_target_selection_tests PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
)

add_test(NAME locked_target_selection_tests
         COMMAND locked_target_selection_tests)

add_executable(standalone_pickup_contract_tests
    test_standalone_pickup_contract.cpp
)

target_include_directories(standalone_pickup_contract_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(standalone_pickup_contract_tests PRIVATE
    Qt6::Core
)

add_test(NAME standalone_pickup_contract_tests
         COMMAND standalone_pickup_contract_tests)

add_executable(navigation_timeout_contract_tests
    test_navigation_timeout_contract.cpp
)

target_include_directories(navigation_timeout_contract_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(navigation_timeout_contract_tests PRIVATE
    Qt6::Core
)

add_test(NAME navigation_timeout_contract_tests
         COMMAND navigation_timeout_contract_tests)

add_executable(line_manager_charge_hold_contract_tests
    test_line_manager_charge_hold_contract.cpp
    ../src/linemanager.cpp
    # 测试目标保留真实 LineManager；TaskExecutor、AGV 和华沿方法只在测试文件
    # 提供最小设备替身，避免单元测试装载或调用现场 SDK。
    ../src/linemanager.h
    ../src/taskexecutor.h
    ../src/agvcontroller.h
    ../src/runtimesettings.cpp
    ../src/chargebusinessrules.cpp
    ../src/chargesettings.cpp
    ../src/autochargecoordinator.cpp
    ../src/chargepilecontroller.cpp
    ../src/chargepileprotocol.cpp
)

target_include_directories(line_manager_charge_hold_contract_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/3rd/HuaYansdk/HuayanRobotLibrary-C++-V1.0.15.0/include"
)

target_link_libraries(line_manager_charge_hold_contract_tests PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::SerialBus
    Qt6::Test
)

add_test(NAME line_manager_charge_hold_contract_tests
         COMMAND line_manager_charge_hold_contract_tests)

add_executable(depth_descent_contract_tests
    test_depth_descent_contract.cpp
)

target_include_directories(depth_descent_contract_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_compile_definitions(depth_descent_contract_tests PRIVATE
    PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(depth_descent_contract_tests PRIVATE
    Qt6::Core
)

add_test(NAME depth_descent_contract_tests
         COMMAND depth_descent_contract_tests)

add_executable(stable_z_validation_contract_tests
    test_stable_z_validation_contract.cpp
)

target_include_directories(stable_z_validation_contract_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_compile_definitions(stable_z_validation_contract_tests PRIVATE
    PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(stable_z_validation_contract_tests PRIVATE
    Qt6::Core
)

add_test(NAME stable_z_validation_contract_tests
         COMMAND stable_z_validation_contract_tests)

add_executable(settings_dialog_tests
    test_settings_dialog.cpp
    ../src/settingsdialog.cpp
    ../src/runtimesettings.cpp
)

target_include_directories(settings_dialog_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(settings_dialog_tests PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Test
)

add_test(NAME settings_dialog_tests
         COMMAND settings_dialog_tests)
