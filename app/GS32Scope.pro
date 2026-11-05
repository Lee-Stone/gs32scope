# GS32Scope 上位机 (qmake, Qt 6.8.3)
# 用法: 在本目录执行 qmake GS32Scope.pro && mingw32-make release

QT += core gui widgets printsupport svg
CONFIG += c++17
TEMPLATE = app
TARGET = GS32Scope

# gs32link.h 的 interface 字段与 windows.h 宏冲突; 需求文档要求这四个宏
DEFINES += UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN

QCUSTOMPLOT_DIR = $$PWD/../third_party/qcustomplot/qcustomplot-source

INCLUDEPATH += $$PWD \
               $$QCUSTOMPLOT_DIR \
               $$PWD/../gs32link/include

SOURCES += \
    main.cpp \
    core/sample_engine.cpp \
    core/monitor_model.cpp \
    core/variable_writer.cpp \
    gui/mainwindow.cpp \
    gui/plot_widget.cpp \
    gui/dashboard_widget.cpp \
    gui/array_view.cpp \
    gui/variable_dialog.cpp \
    gui/monitor_dock.cpp \
    gui/write_dialog.cpp \
    symbols/elf_parser.cpp \
    symbols/svd_parser.cpp \
    symbols/dwarf_parser.cpp \
    $$QCUSTOMPLOT_DIR/qcustomplot.cpp

HEADERS += \
    core/variable_def.h \
    core/value_codec.h \
    core/sample_engine.h \
    core/monitor_model.h \
    core/variable_writer.h \
    gui/mainwindow.h \
    gui/combo_box.h \
    gui/spin_box.h \
    gui/plot_widget.h \
    gui/dashboard_widget.h \
    gui/array_view.h \
    gui/variable_dialog.h \
    gui/monitor_dock.h \
    gui/write_dialog.h \
    gui/symbol_item.h \
    link/link_driver.h \
    symbols/elf_parser.h \
    symbols/svd_parser.h \
    symbols/dwarf_parser.h \
    $$QCUSTOMPLOT_DIR/qcustomplot.h

RESOURCES += resources/app.qrc

FORMS += gui/mainwindow.ui

# timeBeginPeriod/timeEndPeriod (采样定时精度)
win32:LIBS += -lwinmm

# 构建后复制驱动 DLL 到可执行文件目录 (开发时直接从构建目录执行)
# 注意: 不能把整个 drivers 目录复制到已有 drivers 目标(会生成 drivers/drivers 且旧 DLL 不更新),
# 这里先建目录再逐文件覆盖。
win32 {
    DESTDIR = $$OUT_PWD
    QMAKE_POST_LINK += -$(MKDIR) $$shell_path($$OUT_PWD/drivers) $$escape_expand(\\n\\t)
    QMAKE_POST_LINK += -$(COPY_FILE) $$shell_path($$PWD/../drivers/gs32link_sim.dll) $$shell_path($$OUT_PWD/drivers) $$escape_expand(\\n\\t)
    QMAKE_POST_LINK += -$(COPY_FILE) $$shell_path($$PWD/../drivers/gs32link_jlink.dll) $$shell_path($$OUT_PWD/drivers) $$escape_expand(\\n\\t)
    QMAKE_POST_LINK += -$(COPY_FILE) $$shell_path($$PWD/../drivers/gs32link_gslink.dll) $$shell_path($$OUT_PWD/drivers) $$escape_expand(\\n\\t)
    QMAKE_POST_LINK += -$(COPY_FILE) $$shell_path($$PWD/../drivers/gs32link_sci.dll) $$shell_path($$OUT_PWD/drivers) $$escape_expand(\\n\\t)
    QMAKE_POST_LINK += -$(COPY_FILE) $$shell_path($$PWD/../drivers/gs32link_can.dll) $$shell_path($$OUT_PWD/drivers) $$escape_expand(\\n\\t)
}

# shadow build: 源码目录保持干净, 构建产物在 build/
# 默认 OUT_PWD 即 qmake 运行目录
