# Qt 桌面前端：优先 Qt6，找不到时回退 Qt5.15（便于本地开发机快速构建）。
find_package(Qt6 6.4 QUIET COMPONENTS Widgets)

if(Qt6_FOUND)
    set(HR_QT_MAJOR 6)
    qt_standard_project_setup()
    set(HR_QT_LIBS Qt6::Widgets)
else()
    find_package(Qt5 5.15 REQUIRED COMPONENTS Widgets)
    set(HR_QT_MAJOR 5)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)
    set(HR_QT_LIBS Qt5::Widgets)
    message(STATUS "未找到 Qt6，回退到 Qt5 构建（发布版仍以 Qt6 为准）")
endif()

add_executable(handwriting-studio
    src/main.cpp
    src/machinepanel.cpp
    src/machinepanel.h
    src/mainwindow.cpp
    src/mainwindow.h
    src/previewcanvas.cpp
    src/previewcanvas.h
)

target_include_directories(handwriting-studio PRIVATE src)

# 前端直接链接 C++ 核心库，不走子进程、不做序列化。
target_link_libraries(handwriting-studio PRIVATE handwriting_core ${HR_QT_LIBS})

if(MSVC)
    target_compile_options(handwriting-studio PRIVATE /W4 /utf-8)
else()
    target_compile_options(handwriting-studio PRIVATE -Wall -Wextra -Wpedantic)
endif()

# MinGW 下必须静态链接 libstdc++ / libgcc。
#
# 原因：Qt 的 mingw_64 包自带一个用较旧 MinGW 编译的 libstdc++-6.dll，
# 运行时会优先加载它。若本工程用更新的 GCC 编译，构造 std::ifstream 这类
# 触及 std::locale 的对象就会在该 DLL 内崩溃（实测为 SIGSEGV），
# 而且栈回溯只指向 libstdc++，完全看不出是自己的代码问题。
# 静态链接后本程序只用自己这份 libstdc++，问题消失，顺带也少两个部署文件。
if(MINGW)
    target_link_options(handwriting-studio PRIVATE -static-libstdc++ -static-libgcc)
endif()

set_target_properties(handwriting-studio PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)
