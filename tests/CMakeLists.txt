find_package(Qt6 REQUIRED COMPONENTS Core Test)

add_executable(station_pickup_config_tests
    test_station_pickup_config.cpp
)

target_include_directories(station_pickup_config_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(station_pickup_config_tests PRIVATE
    Qt6::Core
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
