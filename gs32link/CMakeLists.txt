# GS32Link 通讯库: 一个后端一个 DLL, 导出同一套 gs32link ABI
set(GS32LINK_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---- 模拟器驱动 ----
add_library(gs32link_sim SHARED
    drivers/sim/sim_driver.cpp
)
target_include_directories(gs32link_sim PRIVATE ${GS32LINK_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_compile_definitions(gs32link_sim PRIVATE GS32LINK_BUILDING NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(gs32link_sim PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${GS32_DRIVERS_DIR}
    PREFIX "")

# ---- J-Link 驱动 ----
add_library(gs32link_jlink SHARED
    drivers/jlink/jlink_driver.cpp
)
target_include_directories(gs32link_jlink PRIVATE ${GS32LINK_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_compile_definitions(gs32link_jlink PRIVATE GS32LINK_BUILDING)
set_target_properties(gs32link_jlink PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${GS32_DRIVERS_DIR}
    PREFIX "")

# ---- GSLink(FTDI2232H / RISC-V SBA) 驱动 ----
# 底层二选一:
#   默认 D2XX 直驱 MPSSE(自实现 JTAG 位流, 不依赖 libMPSSE)
#   -DGS32LINK_GSLINK_USE_LIBMPSSE=ON 改用 FTDI 官方 libMPSSE.dll(验证用)
option(GS32LINK_GSLINK_USE_LIBMPSSE "Use FTDI libMPSSE.dll instead of raw D2XX MPSSE" OFF)
add_library(gs32link_gslink SHARED
    drivers/gslink/gslink_driver.cpp
)
target_include_directories(gs32link_gslink PRIVATE ${GS32LINK_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_compile_definitions(gs32link_gslink PRIVATE GS32LINK_BUILDING)
if(GS32LINK_GSLINK_USE_LIBMPSSE)
    target_compile_definitions(gs32link_gslink PRIVATE GS32LINK_GSLINK_LIBMPSSE)
endif()
set_target_properties(gs32link_gslink PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${GS32_DRIVERS_DIR}
    PREFIX "")

# ---- SCI(串口/TCP 测试) 驱动 ----
add_library(gs32link_sci SHARED
    drivers/sci/sci_driver.cpp
)
target_include_directories(gs32link_sci PRIVATE ${GS32LINK_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_compile_definitions(gs32link_sci PRIVATE GS32LINK_BUILDING)
target_link_libraries(gs32link_sci PRIVATE ws2_32)
set_target_properties(gs32link_sci PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${GS32_DRIVERS_DIR}
    PREFIX "")

# ---- CAN 网关(TCP/CAN-Ethernet 桥, 与 SCI 同帧协议) ----
add_library(gs32link_can SHARED
    drivers/sci/sci_driver.cpp
)
target_include_directories(gs32link_can PRIVATE ${GS32LINK_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_compile_definitions(gs32link_can PRIVATE GS32LINK_BUILDING GS32LINK_DRIVER_CAN)
target_link_libraries(gs32link_can PRIVATE ws2_32)
set_target_properties(gs32link_can PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${GS32_DRIVERS_DIR}
    PREFIX "")
