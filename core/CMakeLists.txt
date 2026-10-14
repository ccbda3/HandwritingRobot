add_library(handwriting_core STATIC
    src/gcode.cpp
    src/trajectory.cpp
)

add_library(handwriting::core ALIAS handwriting_core)

target_include_directories(handwriting_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(handwriting_core PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(handwriting_core PRIVATE /W4 /utf-8)
else()
    target_compile_options(handwriting_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(HR_BUILD_TESTS)
    add_executable(test_trajectory tests/test_trajectory.cpp)
    target_link_libraries(test_trajectory PRIVATE handwriting_core)
    add_test(NAME cpp.trajectory COMMAND test_trajectory)

    add_executable(test_gcode tests/test_gcode.cpp)
    target_link_libraries(test_gcode PRIVATE handwriting_core)
    add_test(NAME cpp.gcode COMMAND test_gcode)
endif()
