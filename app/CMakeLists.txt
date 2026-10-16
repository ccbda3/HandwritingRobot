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
    src/boundswidget.cpp
    src/boundswidget.h
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

# ---------------------------------------------------------------------------
# 自动部署：把运行所需的 Qt / MinGW 运行时 DLL 与平台、图像格式插件
# 复制到 exe 所在目录，使生成的 handwriting-studio.exe 可以直接双击运行，
# 不再依赖系统 PATH 里是否装有 Qt（即解决“运行找不到 dll”的问题）。
#
# 注意：windeployqt 在本机因 qtpaths 报错无法使用，这里改为手工按已知清单拷贝，
# 清单来自对 exe 的 PE 导入表 + Qt6Core/Gui/Widgets 传递依赖的分析，覆盖完整闭包。
# ---------------------------------------------------------------------------
if(WIN32 AND Qt6_FOUND)
    # Qt6::Core 的 LOCATION 即 <qt>/6.x/mingw_64/bin/Qt6Core.dll，取其所在目录。
    get_target_property(_qt_core_dll Qt6::Core LOCATION)
    get_filename_component(_qt_bin_dir "${_qt_core_dll}" DIRECTORY)
    # 插件不在 bin/ 下，而在同级 plugins/ 里（bin/../plugins）。
    get_filename_component(_qt_plugins_dir "${_qt_bin_dir}/../plugins" ABSOLUTE)
    set(_deploy_dst "$<TARGET_FILE_DIR:handwriting-studio>")

    # 运行时 DLL（exe 直接 + 经 Qt 传递依赖）
    set(_deploy_dlls
        Qt6Core.dll
        Qt6Gui.dll
        Qt6Widgets.dll
        libgcc_s_seh-1.dll
        libstdc++-6.dll
        libwinpthread-1.dll
    )
    # 平台/图像插件（qwindows 为普通 GUI 必选；qoffscreen 为无头快照/演示必选）
    set(_deploy_plugins
        platforms/qwindows.dll
        platforms/qoffscreen.dll
        imageformats/qjpeg.dll
        imageformats/qico.dll
        imageformats/qgif.dll
        imageformats/qsvg.dll
    )

    add_custom_command(TARGET handwriting-studio POST_BUILD
        COMMENT "部署 Qt/MinGW 运行时 DLL 与插件到 exe 目录"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_deploy_dst}/platforms"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_deploy_dst}/imageformats"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_deploy_dst}/styles"
    )

    foreach(_dll ${_deploy_dlls})
        add_custom_command(TARGET handwriting-studio POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${_qt_bin_dir}/${_dll}" "${_deploy_dst}/${_dll}"
            COMMENT "  复制 ${_dll}"
        )
    endforeach()

    foreach(_plug ${_deploy_plugins})
        add_custom_command(TARGET handwriting-studio POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${_qt_plugins_dir}/${_plug}" "${_deploy_dst}/${_plug}"
            COMMENT "  复制 ${_plug}"
        )
    endforeach()
endif()
