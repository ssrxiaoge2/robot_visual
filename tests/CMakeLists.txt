find_package(Qt6 REQUIRED COMPONENTS Core)

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
