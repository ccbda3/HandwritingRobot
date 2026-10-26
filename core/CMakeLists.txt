add_library(handwriting_core STATIC
    src/bounds.cpp
    src/curve.cpp
    src/gcode.cpp
    src/glyph.cpp
    src/grbl.cpp
    src/json.cpp
    src/layout.cpp
    src/render.cpp
    src/serial_posix.cpp
    src/serial_win32.cpp
    src/style_model.cpp
    src/trajectory.cpp
    src/utf8.cpp
)

add_library(handwriting::core ALIAS handwriting_core)

target_include_directories(handwriting_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(handwriting_core PUBLIC cxx_std_17)

# 串口枚举用 SetupAPI 拿友好名（"USB-SERIAL CH340 (COM3)"），
# 比只给 COM 号有用得多 —— 用户靠它才能认出哪块板子是哪块
if(WIN32)
    target_link_libraries(handwriting_core PUBLIC setupapi)
endif()

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

    add_executable(test_json tests/test_json.cpp)
    target_link_libraries(test_json PRIVATE handwriting_core)
    add_test(NAME cpp.json COMMAND test_json)

    add_executable(test_curve tests/test_curve.cpp)
    target_link_libraries(test_curve PRIVATE handwriting_core)
    add_test(NAME cpp.curve COMMAND test_curve)

    add_executable(test_glyph tests/test_glyph.cpp)
    target_link_libraries(test_glyph PRIVATE handwriting_core)
    add_test(NAME cpp.glyph COMMAND test_glyph)

    add_executable(test_render tests/test_render.cpp)
    target_link_libraries(test_render PRIVATE handwriting_core)
    add_test(NAME cpp.render COMMAND test_render)

    add_executable(test_layout tests/test_layout.cpp)
    target_link_libraries(test_layout PRIVATE handwriting_core)
    add_test(NAME cpp.layout COMMAND test_layout)

    add_executable(test_grbl tests/test_grbl.cpp)
    target_link_libraries(test_grbl PRIVATE handwriting_core)
    add_test(NAME cpp.grbl COMMAND test_grbl)

    add_executable(test_bounds tests/test_bounds.cpp)
    target_link_libraries(test_bounds PRIVATE handwriting_core)
    add_test(NAME cpp.bounds COMMAND test_bounds)

    add_executable(test_style_model tests/test_style_model.cpp)
    target_link_libraries(test_style_model PRIVATE handwriting_core)
    add_test(NAME cpp.style_model COMMAND test_style_model)

    # 版面导出工具。Python 侧调用它做跨语言一致性校验，
    # 防止 C++ 与 Python 两份版面实现悄悄漂移（见 ADR-0003 的后果一节）。
    add_executable(hr-layout-dump tools/layout_dump.cpp)
    target_link_libraries(hr-layout-dump PRIVATE handwriting_core)

    # 风格模型推理的跨语言对拍工具（Python 训练侧调用）
    add_executable(hr-style-dump tools/style_dump.cpp)
    target_link_libraries(hr-style-dump PRIVATE handwriting_core)
endif()
