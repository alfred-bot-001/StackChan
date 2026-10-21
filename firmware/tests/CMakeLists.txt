cmake_minimum_required(VERSION 3.16)

project(stackchan_firmware_tests LANGUAGES CXX)

enable_testing()

add_executable(motion_math_test
    motion_math_test.cpp
    ../main/stackchan/motion/motion_math.cpp
)

target_include_directories(motion_math_test PRIVATE
    ../main
)

target_compile_features(motion_math_test PRIVATE cxx_std_17)

add_test(NAME motion_math_test COMMAND $<TARGET_FILE:motion_math_test>)

add_executable(home_link_protocol_test
    home_link_protocol_test.cpp
    ../main/hal/utils/home_link/home_link_protocol.cpp
)

target_include_directories(home_link_protocol_test PRIVATE
    ../main
    ../components/ArduinoJson/src
)

target_compile_features(home_link_protocol_test PRIVATE cxx_std_17)

add_test(NAME home_link_protocol_test COMMAND $<TARGET_FILE:home_link_protocol_test>)
