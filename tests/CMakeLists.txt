find_package(Qt6 REQUIRED COMPONENTS Core Test Network Gui)

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
