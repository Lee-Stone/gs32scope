# GS32Link 通讯库 Makefile (MinGW g++)
# 用法: 在本目录执行 mingw32-make
# 输出: ..\drivers\ 下的 gs32link_sim.dll / gs32link_jlink.dll / gs32link_gslink.dll / gs32link_sci.dll

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DGS32LINK_BUILDING
INCLUDES  = -Iinclude -Icommon
LDFLAGS   = -shared -static-libgcc -static-libstdc++

OUTDIR = ..\drivers

all: $(OUTDIR)\gs32link_sim.dll $(OUTDIR)\gs32link_jlink.dll $(OUTDIR)\gs32link_gslink.dll $(OUTDIR)\gs32link_sci.dll $(OUTDIR)\gs32link_can.dll

$(OUTDIR)\gs32link_sim.dll: drivers\sim\sim_driver.cpp include\gs32link.h common\gs32link_util.h
	@if not exist $(OUTDIR) mkdir $(OUTDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ drivers\sim\sim_driver.cpp

$(OUTDIR)\gs32link_jlink.dll: drivers\jlink\jlink_driver.cpp include\gs32link.h common\gs32link_util.h
	@if not exist $(OUTDIR) mkdir $(OUTDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ drivers\jlink\jlink_driver.cpp

GSLINK_SRCS = drivers\gslink\gslink_driver.cpp drivers\gslink\riscv_debug.cpp drivers\gslink\ftdi_jtag.cpp
GSLINK_HDRS = drivers\gslink\gslink_log.h drivers\gslink\ftdi_jtag.h drivers\gslink\riscv_debug.h

$(OUTDIR)\gs32link_gslink.dll: $(GSLINK_SRCS) $(GSLINK_HDRS) include\gs32link.h common\gs32link_util.h
	@if not exist $(OUTDIR) mkdir $(OUTDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ $(GSLINK_SRCS)

$(OUTDIR)\gs32link_sci.dll: drivers\sci\sci_driver.cpp include\gs32link.h common\gs32link_util.h
	@if not exist $(OUTDIR) mkdir $(OUTDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ drivers\sci\sci_driver.cpp -lws2_32

$(OUTDIR)\gs32link_can.dll: drivers\sci\sci_driver.cpp include\gs32link.h common\gs32link_util.h
	@if not exist $(OUTDIR) mkdir $(OUTDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(LDFLAGS) -DGS32LINK_DRIVER_CAN -o $@ drivers\sci\sci_driver.cpp -lws2_32

clean:
	@if exist $(OUTDIR)\gs32link_sim.dll del /q $(OUTDIR)\gs32link_sim.dll
	@if exist $(OUTDIR)\gs32link_jlink.dll del /q $(OUTDIR)\gs32link_jlink.dll
	@if exist $(OUTDIR)\gs32link_gslink.dll del /q $(OUTDIR)\gs32link_gslink.dll
	@if exist $(OUTDIR)\gs32link_sci.dll del /q $(OUTDIR)\gs32link_sci.dll
	@if exist $(OUTDIR)\gs32link_can.dll del /q $(OUTDIR)\gs32link_can.dll

.PHONY: all clean

