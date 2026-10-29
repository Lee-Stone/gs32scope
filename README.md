# GS32Scope

面向 GS32（RISC-V DSP）的嵌入式在线变量示波器 / 上位机。通过 JTAG、SCI 或 CAN 网关周期读取目标 RAM 变量与外设寄存器，实时绘图、统计、修改变量，并提供可自定义的电机控制仪表盘。

对标工具：SEGGER J-Scope、STM32CubeMonitor、NXP FreeMASTER。

## 功能

- **变量监控**：ELF/DWARF（首选）或 `.symtab` 兜底导入全局变量，支持嵌套结构体、数组、位域、枚举类型名、按源文件分组。
- **寄存器监控**：CMSIS-SVD 导入，外设 → 寄存器 → 位域三级树；外设整块监控会按寄存器跨度读取。
- **高频采样**：POLL 模式 1 ms–10 s，deadline 调度 + 高精度 waitable timer；单次采样模式。
- **实时波形**：统一坐标系、暗色示波器风格、X/Y 网格、30 fps 重绘、X 轴自动跟随、60 s 历史缓冲、加入通道自动回放历史、CSV 导出。
- **实时统计**：左下绘制列表实时显示当前值 / 最大 / 最小 / 平均，颜色列居中。
- **变量写入**：监控栏双击当前值、仪表盘滑块/开关/数值编辑均可写入；位域走读-改-写；写入后立即回读校验。
- **自定义仪表盘**：数值 / 圆形仪表 / 滑块 / 开关 / 数值编辑五类组件，统一尺寸（按窗口均分铺满网格，默认约 287×180）；拖动自动吸附到单元网格并禁止重叠；QSettings 自动持久化。
- **电机控制演示**：一键生成转速/电流仪表、目标转速滑块与目标转速数值编辑、使能开关、温度数值编辑面板。
- **波形增删**：监控栏顶部「加入波形 / 移出波形」按钮、波形列勾选、左下绘制列表右键「添加波形变量…」、变量对话框「波形」列四种入口。
- **主题**：`主题` 菜单可切换浅色 / 深色 / 跟随系统；下拉框统一向下展开，顶部菜单/工具栏按钮、下拉箭头和 SpinBox 上下按钮都有明确样式。
- **多通讯后端**：Simulation、SCI（串口/TCP 测试）、CAN 网关（TCP/CAN-Ethernet 桥）、J-Link、GS32-Link（FTDI MPSSE + JTAG + RISC-V SBA）。
- **CSV 导出**：`time_s,channel,value` 长表，UTF-8 BOM，Excel 可直接打开。

## 目录

```
app/                 Qt 6 上位机
  core/              采样引擎、监控模型、值编解码、写入器
  gui/               主窗口、波形、仪表盘、变量对话框、监控树、绘制列表
  symbols/           DWARF / ELF / SVD 解析
  link/              gs32link ABI 的 QLibrary 封装
gs32link/            独立通讯 DLL（纯 C ABI）
  drivers/sim/       模拟器（含电机演示信号与写入回读）
  drivers/jlink/     SEGGER JLinkARM.dll
  drivers/gslink/    FTDI FT2232H + MPSSE + JTAG + RISC-V Debug 0.13.2 SBA
  drivers/sci/       SCI 串口 / CAN 网关（同帧协议，TCP 测试传输）
tests/               解析器/模型控制台测试
third_party/         QCustomPlot 2.1.1 (GPLv3)
.zcode/              需求文档、样例 ELF/SVD、SCI 目标模拟器、UI 测试脚本
REQUIREMENTS.md      原始需求与硬件踩坑记录
```

## 构建

依赖：Windows x64、Qt 6.8.3+（已验证 6.11.0）、MinGW 13.1、`mingw32-make`。

```powershell
# 1) 通讯库（输出 drivers\gs32link_*.dll）
cd gs32link
mingw32-make

# 2) 上位机（shadow build）
mkdir build
cd build
qmake CONFIG+=release ..\app\GS32Scope.pro
mingw32-make -j4

# 3) 部署 Qt 运行库（绿色包）
windeployqt --release --compiler-runtime GS32Scope.exe
```

- `.pro` 的 post-link 会把 `drivers\` 里的 DLL 逐个复制到构建目录/`drivers`。
- 先编 `gs32link` 再编 app；若驱动力被占用，先关闭 `GS32Scope.exe`。
- CMake 工程也保留，qmake 是主构建路径。

## 运行

### 一键仿真演示

```powershell
GS32Scope.exe --demo
```

自动连接 Simulation、加载 `sim.speed/current/target/enable/temp`、生成电机控制仪表盘并开始 50 Hz 采样。可以直接拖目标转速滑块、点使能开关验证写入。

### 导入真实 ELF / SVD

```powershell
GS32Scope.exe --import firmware.elf
GS32Scope.exe --import chip.svd
```

`--import` 会静默导入并监控全部顶层变量/外设。也可以在界面点「变量…」手动导入和勾选。

### SCI（串口）

1. 「通讯接口」选择 `SCI`，点「连接」。
2. 输入 `COM3@115200`（串口）或 `tcp:127.0.0.1:7000`（测试）。
3. 目标固件需实现下方帧协议。

本机无硬件时可先用目标模拟器：

```powershell
python .zcode\sci_target_sim.py 7000
$env:GS32SCOPE_BACKEND='sci'
$env:GS32SCOPE_SCI_CONFIG='tcp:127.0.0.1:7000'
GS32Scope.exe --demo
```

### CAN 网关

`CAN-GW` 后端面向 CAN↔Ethernet/串口网关：网关把 CAN 帧还原成与 SCI 相同的字节流。默认 TCP `7001`：

```powershell
python .zcode\sci_target_sim.py 7001
$env:GS32SCOPE_BACKEND='can'
$env:GS32SCOPE_SCI_CONFIG='tcp:127.0.0.1:7001'
GS32Scope.exe --demo
```

真实 USB-CAN 适配器需要提供等价网关固件；`gs32link_can.dll` 本身不直接驱动 PCAN/Vector 硬件。

### JTAG

- `GS32-Link`：自研 FTDI FT2232H + MPSSE + JTAG + RISC-V SBA，目标 `RV32`，接口 `jtag`。
- `JLink`：运行时加载 `JLinkARM.dll`，目标 `RV32`（内部 `riscv32`），接口 `swd`。

## 使用要点

- **监控集 / 显示集独立**：变量对话框节点勾选 = 监控；「波形」列 = 进波形。
- **波形增删**：在右侧监控栏选中变量/结构体，点顶部「加入波形 / 移出波形」；也可直接勾选「波形」列，或在左下绘制列表右键「添加波形变量…」。
- **写变量**：监控栏双击当前值；仪表盘滑块松开、开关点击、数值编辑输入后点「写入」；状态栏显示回读值。
- **仪表盘**：`添加组件` 选择数值/仪表/滑块/开关/数值编辑并绑定变量；所有组件统一尺寸、按单元网格均分铺满且不会重叠；右键绑定/移除。
- **主题**：菜单栏 `主题 → 浅色 / 深色 / 跟随系统`，选择自动记忆；跟随系统会随 Windows 深浅色变化实时切换。
- **CSV**：`数据 → 导出 CSV` 或 `Ctrl+Shift+E`；`Ctrl+Shift+L` 清空波形。
- **会话恢复**：工具栏参数、导入文件、监控/绘制勾选、仪表盘布局退出自动保存到 `%APPDATA%\GS32\GS32Scope.ini`。

## SCI / CAN 网关帧协议

小端：

```
Request : A5 5A | cmd | seq | addr(4) | len(2) | payload(len) | crc16(2)
Response: 5A A5 | cmd | seq | status | len(2) | payload(len) | crc16(2)

cmd    0x01 read   (payload 为空, len = 要读字节数)
       0x02 write  (payload = 数据, len = 字节数)
crc16  CCITT-FALSE, poly 0x1021, init 0xFFFF,
       覆盖 cmd..payload(请求含 addr/len, 响应含 status/len)
status 0=OK, 1=CRC, 2=参数, 3=未知命令
```

单帧 payload 上限 256 字节，驱动自动分块。参考实现：`gs32link/drivers/sci/sci_driver.cpp` 与 `.zcode/sci_target_sim.py`。

## 测试与样例

- `tests/`：ELF、DWARF、MonitorModel 控制台测试（CMake 构建）。
- `.zcode/sample_motor.elf`：最小 DWARF4 结构体样例（`g_motor` @ `0x20100000`）。
- `.zcode/sample_motor.svd`：`MOTOR` 外设 + 寄存器/位域样例。
- `.zcode/sci_target_sim.py`：SCI/CAN 网关目标模拟器（TCP）。
- `.zcode/ui_test.ps1` / `capture_printwindow.ps1`：窗口点击/拖动/截图辅助脚本。

样例验证：

```powershell
GS32Scope.exe --demo --import .zcode\sample_motor.elf
GS32Scope.exe --demo --import .zcode\sample_motor.svd
```

## 已知限制

- STREAM（硬件定时流）仅有 ABI 定义，各后端返回 `GS32_ERR_NOT_IMPL`。
- OpenOCD 后端未实现；新增后端按 `gs32link.h` 实现即可。
- CAN 目前是网关路径，不直接驱动 USB-CAN 适配器。
- `.gsr` 录制/回放、游标/边沿搜索、FFT、多分屏仍属后续规划。
- QCustomPlot 2.1.1 为 GPLv3；对外分发前请确认许可策略。
- 详细硬件约束（SBA 地址、TAP 时序、批量聚合等）见 `REQUIREMENTS.md`。

## 相关文档

- `.zcode/GS32Scope.md`：产品能力清单
- `.zcode/竞品分析.md`：竞品参考
- `REQUIREMENTS.md`：原始需求、架构与硬件踩坑记录
- `begin.rule`：开发/构建/硬件操作约束
