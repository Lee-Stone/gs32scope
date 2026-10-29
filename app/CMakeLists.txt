# GS32Scope 上位机
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets PrintSupport Svg)

set(QCUSTOMPLOT_DIR ${CMAKE_SOURCE_DIR}/third_party/qcustomplot/qcustomplot-source)

add_executable(GS32Scope WIN32
    main.cpp
    core/sample_engine.cpp
    core/monitor_model.cpp
    core/variable_writer.cpp
    gui/mainwindow.cpp
    gui/mainwindow.ui
    gui/combo_box.h
    gui/spin_box.h
    gui/plot_widget.cpp
    gui/dashboard_widget.cpp
    gui/variable_dialog.cpp
    gui/monitor_dock.cpp
    gui/display_list.cpp
    gui/write_dialog.cpp
    symbols/elf_parser.cpp
    symbols/svd_parser.cpp
    symbols/dwarf_parser.cpp
    resources/app.qrc
    ${QCUSTOMPLOT_DIR}/qcustomplot.cpp
    ${QCUSTOMPLOT_DIR}/qcustomplot.h
)

target_include_directories(GS32Scope PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/gs32link/include
    ${QCUSTOMPLOT_DIR}
)

target_link_libraries(GS32Scope PRIVATE Qt6::Widgets Qt6::PrintSupport Qt6::Svg)

# 运行目录放一份 drivers/(开发时直接从构建树执行)
add_custom_command(TARGET GS32Scope POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:GS32Scope>/drivers"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${GS32_DRIVERS_DIR}" "$<TARGET_FILE_DIR:GS32Scope>/drivers"
    COMMENT "Copy gs32link driver DLLs"
)
