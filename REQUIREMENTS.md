# GS32Scope 需求文档

版本：v1.18（更新：**工具栏 UI 重构**——「后端」改「通讯接口」，显示名映射 gslink→GS32-Link、jlink→JLink、sim→Simulation（真实后端名存 item userData，连接逻辑不变）；目标 QLineEdit 改下拉框（RV32 默认，内部映射回 riscv32）；通讯接口/目标/TCK 下拉框统一 100x30。**连接锁定**——连接成功后禁用通讯接口/目标/采样间隔/TCK，断开恢复（直接断开、采样中断开→stopped() 收尾两条路径均覆盖）。**会话持久化（QSettings IniFormat）**——closeEvent 保存工具栏参数 + 变量对话框导入文件（完整路径）与监控/绘制勾选；启动恢复：静默重导入 ELF/SVD（缺失跳过）+ 按全路径**重放勾选**（与手动勾选同信号链，结构体整块监控语义自然还原）；导入拆分 `importElfFile/importSvdFile(path, interactive)` 供恢复复用）

前版 v1.17：新增**单次采样**模式——「连续采样」+「单次采样」双按钮，单次读取一拍即自动停止；两按钮同条件使能，任一运行期间均禁用。修复退出崩溃（`~MainWindow` 收 engineThread_）与状态栏重影（QStatusBar 控件需显式 `addPermanentWidget`）
日期：2026-09-04
状态：需求冻结；P0 开发中

> **2026-10-28 实现补充**：已加入变量写入（监控栏双击/仪表盘滑块开关，写后回读）、
> 自定义仪表盘与电机演示面板、CSV 导出、SCI/CAN 网关后端、ELF/SVD 一键导入监控、
> 主题切换；左下绘制列表改为 当前值/最大/最小/平均，移除 Y Div/Y Offset 列。
> 构建/运行/协议说明见 `README.md`。下表的 P0 状态保留为原始记录。

## 0. 实现状态

| P0 任务 | 状态 | 说明 |
|---|---|---|
| 1 工程骨架 | ✅ | **qmake(.pro) + Qt 6.8.3** 主窗口、**浅色三栏布局（波形区保持暗色黑底）**，可编译运行 |
| 2 GS32Link 库 | ✅ | `gs32link.h` ABI 定稿；sim 模拟器 + jlink（自动查找 JLinkARM.dll）+ **gslink（FTDI2232H/D2XX 直驱 MPSSE JTAG + RISC-V SBA，v1.14 硬件验证读通目标内存，v1.15 分层重构 + 批量聚合）** |
| 3 ELF/DWARF 解析 | ✅ | DWARF 类型树 + symtab 兜底；**已用真实 GS32 DSP ELF（RISC-V / DWARF5 / gs-riscv-gcc 13.2.0）验证：181 变量 / 85 结构体，嵌套与位域展开正常**；`test_elf_parser`、`test_dwarf_parser`、`test_dwarf_real` 全绿 |
| 4 采样引擎 | ✅ | POLL deadline 调度、批量读、类型/位域解码、实际速率统计；**v1.16：高精度 waitable timer 调度（wake 峰值 12~13ms→0.xms）+ 抖动插桩（状态栏 wake/wr 峰值显示）；v1.17：单次采样（一拍即停）** |
| 5 波形显示 | ✅ | **统一主坐标系**（所有通道叠加同一 X/Y）；X 默认 [-5,5]、Y 默认 [-100,100]、**零点正中**；无图例/无右侧坐标轴；双击恢复默认量程 |
| 5a 监控/显示两级选择 | ✅ | 变量对话框：节点勾选框=监控、独立「绘制」列；右侧监控树（树形统计、列可配）+ 左下绘制列表；**两集合互不相关** |
| 5b 符号树排序/基地址 | ✅ | v1.10：排序只作用于顶层（名称列小写优先），结构体成员固定定义顺序（地址升序）；结构体/外设/寄存器显示基地址 |
| 5c 源文件分组/数组/枚举 | ✅ | v1.11：ELF 根→源文件(相对路径)→变量 三层；同名 static 可区分；数组多维/嵌套/结构体元素展开、超100分页；枚举类型名显示 |
| 6 .gscope 工程文件 | ⬜ | 待开发（v1.18 已先行落地 **QSettings 会话持久化**：工具栏参数/导入文件/监控与绘制勾选退出自动保存、启动自动恢复；.gscope 仍保留为可显式携带/切换的工程文件格式） |
| 7 CSV 导出 | ⬜ | 待开发 |

P1 剩余：OpenOCD 后端、自定义调试器模板、STREAM 模式、SVD 高级防护、游标/搜索、.gsr 录制回放、变量写。
P2 剩余：寄存器写、滑块调参、压缩编码、FFT、数组元素选择、多语言、安装包。

### 0.1 真实目标平台（已确认）

- 芯片：GS32 DSP（**RISC-V 内核**，外设寄存器风格同 TI C2000 类 DSP：`AdcaRegs`/`EPwm1Regs`/`Cmpss*Regs` 等）
- 工具链：`gs-riscv-gcc 13.2.0`（`-march=rv32imafc_xgsdida`，DSP 扩展）
- 调试信息：**DWARF5**（含 `.debug_loclists/.debug_rnglists/.debug_line_str`，无 `.debug_addr/.debug_str_offsets`）
- 样例工程：`gs32fmt5000`（RAM 变量 @0x201xxxxx，外设区 @0x40000000）

---

## 1. 产品定位

GS32Scope 是一款面向嵌入式调试的**软件示波器 / 在线变量观测工具**。
通过 JTAG/SWD 调试接口（J-Link、OpenOCD、自定义调试器）周期性读取运行中芯片的内存变量与外设寄存器，实现实时波形显示、测量分析、数据录制与回放。

对标工具：J-Scope (SEGGER)、STM32CubeMonitor。

## 2. 总体架构

```
┌─────────────────────────────────────────────────────┐
│                 GS32Scope 上位机 (Qt 6.8.3 / C++)    │
│  ┌──────────┐ ┌──────────────┐ ┌──────────────────┐  │
│  │ 符号面板  │ │   波形显示区  │ │  状态/测量/搜索   │  │
│  │(变量/寄存器)│ │ (分屏 Plot)  │ │     面板          │  │
│  └──────────┘ └──────────────┘ └──────────────────┘  │
│  ┌────────────────────────────────────────────────┐  │
│  │ 采样引擎: POLL/STREAM → 时基归一 → 环形缓冲      │  │
│  └────────────────────────────────────────────────┘  │
│  ┌──────────┐  ┌────────────┐  ┌──────────────────┐  │
│  │ ELF/DWARF│  │ SVD 解析    │  │ 录制/回放 (.gsr)  │  │
│  └──────────┘  └────────────┘  └──────────────────┘  │
└───────────────────────┬─────────────────────────────┘
                        │ 仅依赖 gs32link.h (纯 C ABI)
┌───────────────────────┴─────────────────────────────┐
│              GS32Link 通讯动态库（独立交付）           │
│  gs32link_sim.dll │ gs32link_jlink.dll │             │
│  gs32link_gslink.dll │ gs32link_openocd.dll │ 自定义 │
└─────────────────────────────────────────────────────┘
```

### 2.1 工程结构

```
GS32Scope/
├── gs32link/                 # 独立通讯库（可单独交付）
│   ├── include/gs32link.h    # 公共 ABI 头文件
│   ├── common/               # 错误处理、JSON 配置解析等公共代码
│   ├── drivers/
│   │   ├── jlink/            # → gs32link_jlink.dll（内部加载 JLinkARM.dll）
│   │   ├── gslink/           # → gs32link_gslink.dll（FTDI D2XX 直驱 MPSSE + RISC-V SBA）
│   │   ├── openocd/          # → gs32link_openocd.dll（Telnet/GDB-RSP）
│   │   └── template/         # 自定义调试器空模板（接入文档）
│   └── Makefile              # MinGW g++ 直编，输出到 ../drivers/
├── app/                      # GS32Scope 上位机源码（Qt 6.8.3，只放源码）
│   ├── GS32Scope.pro         # qmake 工程文件（shadow build 到 ../build/）
│   ├── core/                 # 采样调度、环形缓冲、时基归一
│   ├── link/                 # QLibrary 封装 gs32link ABI → C++ 类
│   ├── symbols/elf/          # ELF/DWARF 变量解析
│   ├── symbols/svd/          # SVD 寄存器解析
│   ├── storage/              # .gsr 录制、CSV 导出、回放
│   ├── analysis/             # 统计、FFT、搜索
│   ├── gui/                  # 主窗口、波形区、面板
│   └── main.cpp
├── build/                    # 编译目录（app 产物 GS32Scope.exe + drivers/）
├── drivers/                  # 运行时目录：gs32link Makefile 输出；上位机从此扫描加载驱动 DLL
├── tests/
└── REQUIREMENTS.md
```

### 2.2 构建系统（v1.13 定稿）

**背景**：源码文件受终端安全软件透明加密（`%TSD-Header`），CMake/Ninja 等未被授权的进程读到密文无法解析缓存，弃用 CMake。

- **gs32link**：`gs32link/Makefile`（MinGW g++ 单文件直编每个驱动 DLL）
  ```powershell
  cd gs32link; mingw32-make          # 输出 ..\drivers\gs32link_*.dll
  ```
- **app**：`app/GS32Scope.pro`（qmake，shadow build，源码目录保持干净）
  ```powershell
  cd build; qmake CONFIG+=release ..\app\GS32Scope.pro; mingw32-make
  ```
  post-link 自动 xcopy `drivers\` → `build\drivers\`（exe 同目录，重编 gs32link 后重跑 make 即同步）
- 编译宏沿用：`-DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN`（**必需**，否则 `gs32link.h` 的 `interface` 字段撞 windows.h 宏）
- qcustomplot.h 必须列入 HEADERS（否则 moc 缺失，链接报 vtable 未定义）
- 工具链：`D:\Qt\6.8.3\mingw_64` + `D:\Qt\Tools\mingw1310_64`（qmake/moc 与 g++ 需同时在 PATH）

### 2.3 设计原则

- 通讯层与 UI 彻底分离：通讯封装为**独立动态库**，纯 C ABI、不依赖 Qt，可被其他软件（C#/Python 等）直接调用
- 上位机通过 `QLibrary` 动态加载 `drivers/` 目录下的驱动 DLL，并枚举展示
- 一个后端一个 DLL（驱动化模式），自定义调试器只需实现同一套 ABI 即可接入

## 3. GS32Link 通讯库 ABI

### 3.1 核心接口（P0）

```c
typedef void* gs32_handle_t;

typedef struct {
    const char* backend;     /* "sim" / "jlink" / "gslink" / "openocd" / ... */
    const char* device;      /* 芯片型号/内核, 如 "riscv32" / "Cortex-M4" */
    const char* interface;   /* "swd" / "jtag" */
    uint32_t    speed_khz;   /* 调试时钟 */
} gs32_config_t;

typedef struct { uint32_t addr; uint32_t size; } gs32_mem_region_t;

/* 错误码 */
enum {
    GS32_OK = 0,
    GS32_ERR_PARAM = -1, GS32_ERR_CONNECT = -2, GS32_ERR_READ = -3,
    GS32_ERR_WRITE = -4, GS32_ERR_TIMEOUT = -5, GS32_ERR_NOT_IMPL = -6
};

GS32LINK_EXPORT int  gs32_get_version(char* buf, uint32_t buflen);
GS32LINK_EXPORT int  gs32_open (const gs32_config_t* cfg, gs32_handle_t* out);
GS32LINK_EXPORT int  gs32_close(gs32_handle_t h);
GS32LINK_EXPORT int  gs32_read (gs32_handle_t h,
                                const gs32_mem_region_t* regions, uint32_t count,
                                uint8_t* out_buf, uint32_t buf_size,
                                uint32_t timeout_ms);
GS32LINK_EXPORT const char* gs32_last_error(gs32_handle_t h);
/* gs32_write 接口已留位，P0 不实现（返回 GS32_ERR_NOT_IMPL） */
```

设计要点：
- `open` 用字符串/结构传配置，向前兼容（未来加参数不改 ABI）
- `gs32_read` 支持**一次事务批量读多个不连续区域**（多变量聚合读取，减少 JTAG 事务开销）
- 同步阻塞式 API；周期采样调度由上位机负责

### 3.2 能力协商

```c
typedef enum {
    GS32_CAP_POLL   = 1 << 0,   /* 支持主机轮询 */
    GS32_CAP_STREAM = 1 << 1,   /* 支持硬件定时流式推送 */
    GS32_CAP_WRITE  = 1 << 2    /* 支持采样中写入（预留） */
} gs32_caps_t;

GS32LINK_EXPORT int gs32_get_caps(gs32_handle_t h, uint32_t* caps);
```

### 3.3 STREAM 模式（P1）：硬件定时采样 + notify 推送

采样频率与 notify 频率**解耦**：硬件按 `period_ns` 精确定时采样，DLL 内部攒批后以 notify 回调推送给上位机。

```c
/* 一次 notify 回调传入一个 Block: BlockHeader + 紧跟 payload */
typedef struct {
    uint32_t magic;             /* 'GSNS' */
    uint16_t version;
    uint16_t header_size;       /* 头大小, 向后兼容 */
    uint64_t seq;               /* Block 序号(连续), 丢块检测 */
    uint64_t t0_ns;             /* 本块第一个样本时间戳(硬件时钟域) */
    uint32_t period_ns;         /* 样本间隔; 0 表示间隔不均匀(见 flags) */
    uint16_t sample_count;      /* 本块样本数 N */
    uint16_t region_count;      /* 区域数 R, 与 config 时一致 */
    uint32_t flags;             /* bit0: 逐样本时间戳; bit1: t0绝对时间;
                                   bit2: 截断告警; bit3: 压缩(预留) */
    uint32_t payload_size;
} gs32_block_hdr_t;

/* payload 默认布局(sample-major 交织):
   [s0: r0..r(R-1)] [s1: r0..r(R-1)] ... 共 N 组; 区域宽度在 config 时固定 */

typedef void (*gs32_notify_cb)(gs32_handle_t h,
                               const gs32_block_hdr_t* hdr,
                               const uint8_t* payload, void* user);

typedef struct {
    uint32_t max_samples;       /* 攒够 N 个样本就发 */
    uint32_t max_latency_ns;    /* 或最长等待时间到就发 */
} gs32_flush_policy_t;

GS32LINK_EXPORT int gs32_config_stream  (gs32_handle_t h, uint32_t period_ns,
                                         const gs32_mem_region_t* regions,
                                         uint32_t count,
                                         const gs32_flush_policy_t* policy);
GS32LINK_EXPORT int gs32_register_notify(gs32_handle_t h, gs32_notify_cb cb, void* user);
GS32LINK_EXPORT int gs32_start_stream   (gs32_handle_t h);
GS32LINK_EXPORT int gs32_stop_stream    (gs32_handle_t h);
GS32LINK_EXPORT int gs32_get_stats      (gs32_handle_t h, ...);  /* 丢帧计数等 */
```

回调契约：
1. 回调运行在 DLL 内部线程，回调内**只做拷贝入队**，禁止 UI/IO/重活
2. 回调内**禁止重入**调用本 DLL 的阻塞函数
3. 上位机按 `seq` 与 `t0 + i*period` 连续性检测丢样，统计丢样率
4. 攒批释放条件：`max_samples` 或 `max_latency_ns` 任一满足

### 3.4 自定义调试器接入

以 `drivers/template` 提供空模板与接入文档。约定：实现上述导出函数、放到 `drivers/` 目录即可被上位机枚举加载。

### 3.5 GSLink 后端（v1.15 分层重构 + 批量聚合）：FTDI2232H / D2XX 直驱 MPSSE + RISC-V SBA

**定位**：自研调试器后端，芯片为 FT2232H（MPSSE 引擎），JTAG 连接，**不使用 OpenOCD**。

**分层架构（v1.15 重构）**：
- 层1 `ftdi_jtag.{h,cpp}`——FTDI D2XX 动态加载 + MPSSE 位流 + **JTAG TAP 状态机**
  （tapReset/scanIR 带缓存/scanDR 命令流压缩/IR 长度探测/批量聚合模式）
- 层2 `riscv_debug.{h,cpp}`——RISC-V Debug 0.13.2 DTM/DMI/DM/SBA，
  对上提供 `ensureReady()/readMem()/writeMem()`
- 层3 `gslink_driver.cpp`——gs32_* C ABI（句柄/互斥锁/一次性诊断编排）
- 公共 `gslink_log.h`（日志分级 + hexDump）；libMPSSE 死分支已删除（从未编译过）

- **依赖（v1.12.1 定稿）**：运行时 `LoadLibrary` 加载 **`Ftd2xx.dll`**（FTDI 官方 D2XX 驱动，
  **不依赖 libMPSSE**——官方已停止维护）；自行实现最小 MPSSE 位流：
  搜索顺序：驱动同目录 → 环境变量 `GS32LINK_GSLINK_PATH` → 系统路径（绿色分发可把 DLL 放 `drivers/`）
- **目标**：**RISC-V 32 内核**（RISC-V Debug Spec 0.13）；上位机目标框默认 `riscv32`（Cortex 不再默认，优先级降低）
- **MPSSE 位流（自实现）**：引脚映射 TCK=ADBUS0/TDI=ADBUS1/TDO=ADBUS2/TMS=ADBUS3；
  **TAP 状态机**——复位(连续 TMS=1)、IR/DR 扫描，从 IDLE 出发结束回 IDLE；
  **v1.13 修复**：Shift 退出序列必须 `Exit1 --TMS=1--> Update --TMS=0--> IDLE`
  （原实现 TMS=0 卡死 Pause 态）。
  USB 分包由 FTDI 驱动/硬件处理（FT2232H 可枚举为高速 USB），本层不关心包大小——
  JTAG 是静态逻辑，命令流分断处只是 TCK 停顿，状态机不受影响
  （早期"<64B 必须"的结论与 TMS bug 同期，属归因混淆，v1.15 修正）
- **所用 MPSSE 命令（完整清单）**：
  | 命令 | 作用 | 用途 |
  |---|---|---|
  | `0x85` | 关闭内部 60MHz 3 分频（disable divide-by-5） | FT2232H 专用，开 60MHz 时钟源 |
  | `0x8A` | 禁用 3 阶段时钟（disable 3-phase clocking） | 与 0x85 配对，恢复 2 相位（TCK 每位 1 拍） |
  | `0x86 L H` | 设置时钟分频（TCK = 30MHz/(1+div)，div≤65535） | 速度控制（1~30MHz） |
  | `0x80 val dir` | 设置 ADBUS 低字节 GPIO（本设计 val=0x0A/dir=0x0B：TMS/TDI 空闲高，TCK/TDI/TMS/TDO 使能） | 引脚初始化 |
  | `0x87` | 立即发送（Send Immediate）——刷新 USB 缓冲，让挂起的命令马上上线 | 每笔命令流末尾 |
  | `0x4B n tms` | TMS 位迁移（clock TMS out，-ve edge，≤7 bit/块，TDI 静态） | TAP 状态跳转（IDLE→Select-DR→Capture→Shift、Exit1→Update→IDLE、复位） |
  | `0x6B n tdi` | 末位数据移位（TMS=1 同时移出最后 1 bit TDI，Shift→Exit1，带读 TDO 在 bit7） | DR/IR 移位段的最后一位 |
  | `0x39 L H data...` | 字节级数据移位（clock bytes out on -ve edge, LSB first，**带读**） | DR 整字节段（41bit DMI 中的 40bit）批量移位+回读 |
  | `0x19 L H data...` | 同 0x39 但**不带读** | 不需要 TDO 的整字节段（纯写） |
  | `0x3B n tdi` | 逐位数据移位（≤7 bit，**带读**，TDO 在 bit7） | 41bit DMI 的余数位 + 突发读 R sbdata0 类需要精读的位段 |
  | `0x1B n tdi` | 同 0x3B 但**不带读** | 纯写的余数位 |
  说明：读回路径统一为「0x39 段每字节 1B（TDO=bit0，LSB first）+ 逐位/末位段每拍 1B（TDO=bit7）」，
  层1 `decodeDr_` 按各段偏移一次性解码；`0x4B/0x6B` 的 TDI 静态电平=1（JTAG 空闲惯例）
- **JTAG 协议栈**：IR/DR 扫描（DTMCS IR=0x10、DMI IR=0x11，IR 值缓存相同则跳过重扫）
  → DMI 41 位事务 `{op[1:0], data[31:0], address[6:0]}`（LSB 先出）访问 DM 寄存器；
  DMI 扫描间按 `dtmcs.idle`（实测=7）插入 IDLE 周期；busy(resp=2) 自动重试（log 带 nop#N try#M）
- **批量聚合模式（v1.15 新增，核心性能优化）**：
  - 动机：每笔 DMI 事务一次 FT_Write/FT_Read 往返，ftd2xx 调用间隔实测 100µs~1ms，
    是采样吞吐瓶颈（JTAG 线上反而大部分空闲）
  - 实现：层1 `beginBatch()/flushBatch()` 把整块读取的全部扫描命令拼成**单笔 FT_Write**，
    TDO 按各扫描 rxB 偏移**一次性读回**统一解码；超过 kBatchMax 自动分段
  - 层2 `readRegions()` 跨 region 聚合：**全部变量**（多 region）的所有扫描
    （含宽度切换 sbcs 写、头尾 8bit 对齐补偿）进同一批量缓冲——每采样周期
    **固定 2 次 ftd2xx 调用（1 写 + 1 读），与变量数无关**（v1.15 初版仅单 region
    聚合，多变量间仍有 200µs 往返间隔，后扩展为跨 region）
  - 利用 DMI 流水语义（扫描 N 返回操作 N-1 的结果）事后校验全部响应码，
    异常自动回退逐变量慢路径（数据永不丢错）
  - kBatchMax=64KB（曾观察到一次首字虚假 resp=3 疑硬件偶发，4096=FT2232H 内部缓冲
    大小为保守值；fallback 机制保证任何情况数据正确）
  - 读稳态走快速路径仅 debug 级日志（默认静默）；`GS32LINK_GSLINK_LOG=debug` 可见
    `[tx BATCH]`/`[sba-multi]`
- **SBA 模式读写内存**（不暂停内核，**v1.14 硬件验证读通**）：
  - `sbcs`(0x38) 位布局按 **GS32 权威定义**（v1.13.3 定稿）：
    `[31:29] sbversion / [22] sbbusyerror / [21] sbbusy / [20] sbreadonaddr /
    [19:17] sbaccess / [16] sbautoincrement / [15] sbreadondata / [14:12] sberror(W1C) /
    [11:5] sbasize / [4:0] sbaccess128/64/32/16/8`；实测回读 `0x20140407`
    （ver=1, 支持 32/16/8bit, sbasize=32）
  - 配置流程：**先独立一笔 W1C** 清 `sberror[14:12]` + `sbbusyerror[22]`（混在配置位里
    写清不掉），再写 `sbaccess=32bit | sbreadonaddr=1`
  - 读：写 `sbaddress0`(0x39) 自动触发读 → 读 **`sbdata0`(0x3C)**；
    **v1.14 关键修复：SBDATA0 地址 0x3D→0x3C**（0x3D 是 sbdata1 即数据高 32 位，
    32 位系统恒 0——此前"SBA 读全 0"的直接根因）
  - **突发读模式（v1.15 实测支持，吞吐 ×3）**：`sbautoincrement`(bit16) +
    `sbreadondata`(bit15) **GS32 均支持**——写 bit 后回读 sbcs 读回 1（spec：未实现位
    硬连线 0）。早期"不支持"结论出自 SBDATA0 错地址时期，系误判（v1.15 重测推翻）。
    突发路径：每连续段 = 1 笔 W 起始地址（readonaddr 触发首读）+ N 笔 R sbdata0
    （每次读自动触发下一地址读、地址硬件自增）+ 1 笔 NOP 取末单元——**每字扫描数
    3 → ~1**，线上时间同比缩短（此时瓶颈为 JTAG 线上时间 = TCK 数 ÷ TCK 频率，
    正比于扫描数）
  - **突发模式陷阱**：readondata 副作用是末次 R 触发**段末+1 的越界读**——若该地址
    无效会置粘滞 sberror 且**不反映在 DMI resp**（静默毒化后续所有 SBA 访问）。
    对策：批末追加 R SBCS + NOP（2 笔开销）检查 sberror，发现即 W1C 清除 +
    整体回退慢路径重读（实测运行安静，监控地址附近越界未踩雷）
  - 写：写 `sbaddress0` → 写 `sbdata0` → 轮询 `sbbusy` 清零（上笔未完成时写下一笔会置
    sbbusyerror 粘滞）；读路径无需轮询——DMI 事务（ms 级）远慢于总线访问（ns 级），
    下一笔 JTAG 到达时访问必然已完成
  - 上位机读区域按地址对齐自动拆分：未对齐头尾用 8bit，中间用 32bit 块
  - 访问宽度：**8/16/32bit 均支持**（SBCS 能力位自证：回读 0x20140407 的
    sbaccess8/16/32 位均为 1）；**64/128bit 不支持**（sbaccess64/128 位为 0）。
    16bit 尚无实测（早期按 8bit 读，后直用 32bit）；早期所有"读不到"均为
    寄存器地址错误所致，与访问宽度无关
- **诊断设施**：`[dmi W/R]` 寄存器访问日志；`[sba-diag]`/`[abs-diag]` 首次 read 时
  探测（诊断地址仅实际监控地址）；abstract command（Access Memory cmdtype=2）路径
  保留作备胎（GS32 实测不支持 cmderr，SBA 已通，仅日志可见）
- **性能演进（读 N 字）**：v1.14 基线（6N 次 ftd2xx、每字 3 扫描）→ 批量聚合
  （消包间隔）→ 跨 region 聚合（消变量间隔，固定 2 次）→ SBA 突发（每字扫描
  3→~1）——**综合提升 3 倍以上**
- **能力**：POLL + WRITE（SBA 天然支持写）；STREAM 未实现（退化 POLL）
- **连接流程**：open 完成 DLL 加载/设备打开/时钟；首次 read 惰性完成 TAP 复位 →
  读 IDCODE 自检 → DTM 复位（dtmcs.dmihardreset）→ `dmcontrol.dmactive` 置位后
  **回读 dmcontrol 确认 dmactive=1**（dmstatus 无 active 位，仅作诊断解码打印）→ SBCS 配置
  （含突发模式探测，log `[init] SBA burst autoincrement=...`）
- **遗留问题（v1.15 审查）**：
  - 写路径尚未批量聚合/突发化（读路径同思路可做）
  - 未对齐头尾用 8bit 逐字节（可改 16bit 减半扫描数，16bit 支持性已由能力位自证）
  - `debugInit` 强制 AllocConsole（日志等级已由环境变量 `GS32LINK_GSLINK_LOG` 控制），
    发布前需开关化

## 4. 变量观测（ELF 解析）

### 4.1 符号来源

- **DWARF 调试信息**（首选）：变量名 + 完整类型 + 地址 + 所属源文件，区分 static/extern；覆盖 GCC/Clang/Keil ARMCC/IAR（均为 DWARF）
- **.symtab 符号表**（兜底）：仅有名字+地址+大小，无调试信息时类型按字节数推断

### 4.2 类型解析深度（P0 已实现）

- 基础类型：uint8/16/32、int8/16/32、float、double、bool、枚举（按 DWARF base_type 名称与 encoding 映射）
- **结构体成员树形展开**，支持**嵌套结构体**：成员地址 = 根地址 + 各级偏移叠加（编译期确定，已实现）
- **位域**：携带 bitOffset + bitSize，按**小端目标机**语义换算——
  DWARF≤4 的 `DW_AT_bit_offset` 从存储单元 MSB 侧计，实际 LSB 位偏移 = `unit_bits - bit_offset - bit_size`；
  DWARF5 的 `DW_AT_data_bit_offset` 直接使用（已实现）
- 匿名结构体/联合体成员**路径合并**到父级（已实现）；volatile/const 忽略限定符（已实现）
- **数组展开（v1.11 已实现）**：多维（`int m[3][4]`）、嵌套 array_type、**结构体数组**（元素展开成员）、
  基础/枚举元素；元素地址行优先计算；**超过 100 元素自动分页**为 `arr[99..0]`、`arr[199..100]` 等分组节点
- **枚举（v1.11 已实现）**：提取枚举类型名（`DW_AT_name`），UI 类型列显示枚举类型名（含数组元素）；
  枚举常量名映射（值→名称）归 P1
- **源文件归属（v1.11 已实现）**：变量记录所属编译单元的 `DW_AT_name`（相对路径，如 `../src/main.c`），
  **区分跨文件同名 static**；symtab 兜底无源文件信息归入「(未知)」
- 解析实现：`symbols/dwarf_parser.cpp`，支持 DWARF 2~5：
  - **多缩写表**：`.debug_abbrev` 段按 CU 分段，遇表终止符 `0x00` 即止（不串表）
  - **CU 边界**：全程单读取器 + seek 绝对偏移定位（多 CU 不错位）
  - form：strp/line_strp/addrx/ref*/sec_offset/exprloc/implicit_const 等；
    `strx` 经 `.debug_str_offsets`、`addrx` 经 `.debug_addr`(DW_AT_addr_base) 解析
  - **跨 CU 类型引用**（ref_addr）统一按文件绝对偏移索引
  - 类型缓存共享指针唯一归属（防重复释放）
- **真实 ELF 验证**：GS32 RISC-V DSP（DWARF5）解析出 181 变量 / 85 结构体，
  位域/嵌套结构体（`Cmpss1Regs.COMPCTL`）与 J-Scope 结果一致
- 边界情况：union（偏移全 0）、函数内 static（有固定地址，P1 纳入）、
  超大数组不整体读（P1，UI 已分页显示但采样仍整块）

### 4.3 数据模型

```
Variable (可监控变量 = 路径 + 类型节点)
 ├─ name:  "g_motor.ctrl.pid.kp"   成员路径
 ├─ rootAddr / offset              根地址 + 相对偏移
 ├─ type:  BaseType{FLOAT, 4}
 └─ bitfield: {bitOffset, bitSize} 位域成员才有
```

### 4.4 读取策略：按根变量聚合

勾选同一结构体的多个成员时，一次读出整个根变量内存，PC 端按成员树裁剪取值，大幅减少 JTAG 事务次数。

### 4.5 过滤规则（默认）

排除：无调试信息的启动/运行时符号、`__` 开头、size=0、（可配置）第三方库目录。

## 5. 寄存器观察（SVD 导入）

- 导入 CMSIS **SVD 文件**（XML，Qt QXmlStreamReader 解析，无第三方依赖），
  形成 `外设 → 寄存器 → 位域` 三级树列表，用户勾选要观察的寄存器/位域
- 与变量树并列在符号面板中，来源图标区分（变量 / 寄存器）
- **位域粒度观察**：如 `USART1.SR.TXE` 可单独加入监控并画波形
- **读副作用防护**：读取可能有副作用（FIFO 弹数据/清标志）——
  默认手动刷新；仅用户显式勾选"持续监控"才进入轮询；预留敏感地址段黑名单
- SVD 路径绑定到工程文件，按芯片型号可自动提示导入

## 6. 采样

### 6.1 两种采集模式

| 模式 | 节拍来源 | 精度 | 说明 |
|---|---|---|---|
| POLL 连续（P0） | 上位机定时轮询 `gs32_read` | ~1ms（高精度等待计时器） | 全后端可用 |
| POLL 单次（P0，v1.17） | 手动触发一次 `gs32_read` | 不适用 | 一拍即停，用于手动校验读取/查看瞬时值 |
| STREAM（P1） | 硬件定时采样 + notify 推送 | 硬件级（µs 级可期） | 需硬件/Agent 支持 |

- **单次采样实现**：引擎 `startImpl(single)` 共用连续采样路径，读完一拍置 `running=false`
  跳过周期调度等待即退，复用既有的 batch flush + `stopped()` 信号收尾流程；
  不经过 waitable timer，无调度开销

### 6.2 POLL 模式细节

- 采样间隔设置范围：**1ms ~ 10s**（典型精度 ±1ms）
- **截止时间调度**：`t_next += interval`，读取后只睡到 t_next；读取耗时波动只影响抖动不影响平均速率
- **调度等待实现（v1.16 定稿）**：`CreateWaitableTimerExW(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)`（Win10 1803+）亚毫秒睡眠粒度，创建失败退回 `sleep_until`。曾用 `timeBeginPeriod(1)` 提升定时器分辨率，实测被 Win11 后台进程策略静默忽略（wake 峰值 12~13ms ≈ 默认 15.6ms 时钟节拍），弃用为主方案（保留为回退路径的兜底）
- **抖动插桩（v1.16）**：状态栏显示 1s 窗口内峰值——`wake`（线程唤醒晚于理想时刻，定位调度延迟）与 `wr`（FT_Write 提交耗时，经 gs32link ABI `gs32_last_write_ms` 透传，定位 USB 驱动延迟）。实测（20ms 周期 / 400B 读取）：修复前 wake 12~13ms、wr 0.35ms；修复后 wake 0.xms
- 读取耗时会占用周期预算：连接后自动测速，设定间隔 < 2×读取耗时 时 UI 提示实际可达速率
- UI 区分三个概念：采样间隔（节拍）/ 刷新间隔（绘图 30~60fps，解耦）/ 实际速率（按时间戳统计，状态栏显示）

### 6.3 时基归一化

- 所有样本记录时间戳；显示按时间戳铺 X 轴（非等间隔容错）
- STREAM（硬件时钟域）与 POLL（主机时钟域）通道混合显示时，统一映射到主机主时间轴：
  连接建立时校准偏移 + 硬件时钟频率线性累推

## 7. 波形显示

### 7.1 两级选择：监控集 vs 显示集（完全独立，v1.9 定稿）

| | 监控集（采样/统计） | 显示集（绘制/波形） |
|---|---|---|
| 入口 | 节点勾选框（任意节点） | 「绘制」列（仅叶子） |
| 粒度 | **任意节点**：可整个结构体（整块内存**聚合读**） | **仅叶子**（简单类型） |
| 例子 | `AdcaRegs`（整块 174 成员一次读） | `AdcaRegs.SOCFRC1`、`sim.speed` |
| 数据去向 | 缓冲/录制/统计/右侧监控树 | 左下绘制列表 + 波形 |

```
变量选择对话框 ──节点勾选框──▶ 监控集(右侧树, 统计/采样)
变量选择对话框 ──「绘制」列──▶ 显示集(绘制列表 + 波形)
            ↑ 两个操作完全独立, 互不影响
```

**独立性规则（v1.9）**：
- 勾「监控」只进监控集（右侧树 + 统计），**不**影响绘制状态
- 勾「绘制」只进显示集（绘制列表 + 波形），**不**进右侧监控树——
  即使未勾「监控」，采样引擎也**内部自动覆盖**该变量（否则无数据），但右侧监控树**不显示**它
- 先「绘制」后补「监控」：该叶子**升级为监控**（出现在右侧树），绘制状态保留
- 先「监控」结构体、再对成员勾「绘制」：成员直接进入显示集
- 勾「绘制」但未勾「监控」的叶子，后续在结构体整块监控时被自动合并，绘制状态随成员保留

其余：
- 数据在监控层持续采集 → 显示集变更不影响采样连续性
- 变量加入显示时**立即回放缓冲区内历史波形**（不从加入时刻才开始）
- 监控整块结构体 = 一次读根地址 + 成员树裁剪，采样事务数最少（与 §4.4 一致）
- 统计按**叶子**分别计算（结构体不合并统计）
- 底部监控栏同时提供**实时读数与统计**（颜色/当前值/min/max/均值）

### 7.2 显示形态（v1.11 定稿：统一主坐标系 + 零点居中）

- **P0 只有一个波形窗口**：所有通道叠加在**统一主坐标系**（共享同一 X/Y 轴，真实数值直接叠加，
  不做每通道独立缩放）
- **默认量程**：X `[-5, 5]`、Y `[-100, 100]`，**零点位于绘图区正中心**；
  滚轮缩放/拖动平移后脱离默认，**双击恢复默认量程**
- **不显示图例/曲线标识，不显示右侧坐标与曲线名称**，最大化绘图区（只保留底部时间轴与左侧 Y 轴）
- 通道配色靠左下绘制列表色块区分
- 左下绘制列表 **Y Div / Y Offset** 编辑作用于主 Y 轴（5 格 × V/div，以 offset 为中心），输入 `Auto` 恢复默认
- **多分屏窗口归入 P1**（相关性对比、分析视图并排等进阶场景），
  现有 Plot 容器架构已预留，P0 不做窗口分配交互

### 7.3 通道类型

| 类型 | 画法 |
|---|---|
| 模拟量 | 连续折线，Y 轴自适应/手动 |
| 数字量/位域 | 逻辑分析仪式方波 |
| 枚举/状态 | 阶梯线或分段色带 |

类型来自 ELF/SVD 解析，拖入自动选画法，可手动修改。

### 7.4 性能策略

- min/max 降采样金字塔：缩小时画包络，放大到原始分辨率画原始点
- 显示缓冲窗口默认 60s（可调），更早数据在录制文件中，回放模式查看

## 8. 测量与分析

- **光标系统**：单光标（读所有通道值）/ 双光标组（Δt + 区间 min/max/avg/ΔV）/
  **多组并存**（不同颜色，激活显示限 1~3 组）
- **边沿吸附**：光标绑定通道后自动吸附到上升/下降沿（采样精度对齐）——
  支撑"同变量两次变化的时间差""不同变量变化的时间差"两类测量
- **搜索驱动的批量测量**：通道 A/B 各设边沿条件搜索 → 自动配对生成 Δt 列表
  （T2 取 T1 后最近命中）→ 统计 min/max/avg/**抖动(jitter)** → 点击定位
- **波形值搜索**：条件（==/!=/>/</区间/边沿/变化/保持）在显示缓冲或回放文件中扫描，
  结果列表 + X 轴标记 + 点击跳转 + 上一处/下一处导航，多搜索并存异色标记
- 统计面板、FFT（P1/P2）

## 9. 数据存储

### 9.1 资源策略

- **在线缓冲**：内存上限（如 128MB，默认）或时间上限二选一，达到即**循环覆盖最旧**
- **录制文件**：点录制后连续流式写盘（独立写线程 + 异步队列，不阻塞采样），
  队列水位过高告警丢旧；无上限或按大小分卷

### 9.2 .gsr 录制格式（与 STREAM Block 同构）

```
FileHeader: 版本/芯片/时间基准/监控变量表快照(路径/类型/地址/布局)/采样模式与周期
Block 序列:  gs32_block_hdr_t + payload（POLL 模式由引擎攒块，STREAM 原样转写）
```

- 回放 = 文件 Block 喂回采样引擎数据通路，显示/游标/搜索全部复用在线逻辑
- 文件头含符号表快照，回放不需要原始 ELF/SVD

### 9.3 压缩编码（P2，低优先级，格式预留）

观察依据：多数时间变量值不变。编码梯度：
1. 通道级 RLE（常值变量近 100:1）
2. Delta + 变长编码（慢变信号）
3. LZ4/Zstd 兜底

`flags` 已预留压缩位，文件头预留每通道编码方式字段，旧文件永远可读。

### 9.4 CSV 导出（P0）

监控数据导出 CSV（时间戳 + 各通道值）。

## 10. 写功能（P1/P2，P0 明确不做）

P0 阶段变量视为**只读**，无修改功能。

| 阶段 | 内容 | 防护 |
|---|---|---|
| P1 | 变量写（双击编辑→写入→回读校验）；滑块/开关绑定变量实时调参 | 写频率限速、只读区检测 |
| P2 | 寄存器写（位域编辑） | 二次确认对话框（地址+原值+新值）、写后回读校验 |

- `gs32_write` ABI 已留位；"采样中并发写"能力由 `gs32_get_caps` 暴露
- POLL 模式写操作由采样线程在两次读之间顺带执行；STREAM 模式视硬件能力暂停-写-恢复

## 11. UI 布局与交付

- **布局（v1.4 定稿）三区 + 两集合**：
  - **监控列表在右侧**、**绘制列表在左下**、**波形在左上**
  - **左侧上下分区默认 3:1**（波形 : 绘制列表，v1.11）
  - 顶部连接/采样工具栏 + 状态栏
- **采样启停（v1.17 定稿）：双按钮**：「连续采样」⇄「停止采样」切换 + 「单次采样」一拍即停
  - **使能逻辑**（两按钮同条件）：已连接 + 有监控变量 + 引擎空闲（非 stopping/running）；
    任一采样运行期间**两按钮均禁用**——连续运行时单次按钮灰、单次运行时单次按钮显示「等待停止」
  - 单次采样：`startSingle()` 读一拍即退，`stopped()` 回调统一恢复按钮（详见 §6.1）
  - **非阻塞停止**：点击停止只置标志（`requestStop`），worker 线程最迟在本次 `gs32_read`
    返回后退出并发 `stopped()` 信号，UI 在回调里收尾（恢复按钮/清理/重启）——
    **绝不在 UI 线程 join 采样线程**（读取耗时可达秒级，会冻结界面）
  - 断开连接/采样中改模型：同样经「先停止 → stopped() 回调 → 再断开/重启」串行化，
    避免与读线程竞态 FT 句柄/模型数据
  - **优雅退出（v1.17 修复）**：`~MainWindow` 先 `requestStop()+stop()` 收 worker、
    再 `quit()+wait()` 收引擎线程——QThread 带运行中线程被析构会 qFatal 崩溃
- **连接工具栏（v1.18 重构）**：
  - **「通讯接口」下拉框**（原「后端」）：显示友好名 **GS32-Link / JLink / Simulation / OpenOCD**，
    真实后端名存 item userData（连接时 `currentData()` 取回加载 DLL，驱动层无感知）
  - **「目标」下拉框**（原 QLineEdit）：当前选项 **RV32**（默认），连接时内部映射回 `riscv32`
  - 通讯接口/目标/TCK 三个下拉框统一 **100x30**；采样间隔 SpinBox 保持原样
  - **连接锁定**：连接成功后禁用通讯接口/目标/采样间隔/TCK（防运行中改参数），
    断开后恢复——直接断开、采样中断开（stopped() 收尾）两条路径均处理；
    停止采样/采样中改模型**不再**提前解锁采样间隔（重启沿用当前值）
- **会话持久化（v1.18，QSettings IniFormat，`%APPDATA%\GS32\GS32Scope.ini`）**：
  - **保存**（closeEvent）：工具栏四参数（backend 按真实名、device/interval/tck）、
    变量对话框导入的 ELF/SVD **完整路径**（存文件根节点 UserRole）、监控/绘制勾选
    （只记最顶层勾选节点——父勾选即覆盖子，避免结构体展开成散叶路径）
  - **恢复**（启动）：静默重导入文件（`interactive=false`，缺失/解析失败跳过）→
    按全路径重放 `setCheckState`（**不锁信号**，与手动勾选走完全相同的信号链
    monitorAdded/displayToggled → 监控模型/绘制列表/波形，结构体整块监控、
    绘制专用叶子等语义自然还原）
  - 与 `.gscope`（§12 P0-6）的关系：QSettings 是**隐式会话记忆**（无需用户操作）；
    .gscope 是**显式工程文件**（可携带、可切换现场），两者并存互补
- **两个集合（监控 vs 绘制，互不相关）**：
  - **监控集**：右侧监控栏，树形统计（参与采样）；入口 = 变量对话框**节点勾选框**
  - **显示集（绘制）**：左下绘制列表 + 左上波形，仅叶子；入口 = 变量对话框**「绘制」列**
  - 勾「绘制」未勾「监控」→ **独立绘制**：进绘制列表+波形，采样内部自动覆盖，右侧监控树不显示
  - 绘制状态两处同步：对话框「绘制」列 ↔ 左下绘制列表+波形；移除 = 对话框取消勾选 或 绘制列表右键移除
- **变量选择对话框（弹出式，非模态）**：
  - 列序：**变量(节点勾选框) | 地址 | 大小 | 类型 | 绘制(仅叶子)**
  - **节点勾选框 = 选择监控**（任意节点：结构体=整块聚合监控，叶子=单变量）；
    **最后一列「绘制」= 选择加入绘制列表（仅叶子可勾）**
  - **「监控」与「绘制」完全独立、各干各的**：勾任一不影响另一列的状态与效果
  - **层级（v1.11 定稿）**：**ELF 根 → 源文件分组(相对路径，整行显示，无勾选框) → 变量**；
    SVD 保持 根 → 外设 → 寄存器 → 位域；内置变量直接挂根
  - **源文件分组**：显示编译单元 `DW_AT_name`（相对路径如 `../src/main.c`），整行显示路径、
    不显示地址/类型、常规字重、可悬停看 tooltip；**区分跨文件同名 static**；
    symtab 兜底归「(未知)」分组
  - 内容：搜索过滤框（按名称）+ 添加(ELF/SVD 导入)/删除按钮
  - 树属性：**交替行色**、复选框蓝底白对勾、类型列；
    **默认展开：文件根 + 源文件分组**；变量（结构体/数组）节点折叠
  - **列宽**：表头分隔线可拖拽调整（Interactive）；初始列宽——**变量名列最宽（~190px）**，
    **大小列最窄（~40px）**，地址列次之（~86px）；**最后一列(绘制)随窗口拉伸**
  - **排序规则（v1.11 定稿）**：表头点击排序**只作用于源文件分组内部**（不跨文件），
    分组本身按名称排；名称列字母序、小写优先于大写；**结构体/寄存器/数组元素成员永不参与排序**，
    固定按地址升序（即 C 源码定义顺序）；**排序后保持各节点展开/折叠状态**
  - **结构体/外设/寄存器节点显示基地址**（地址列）；**数组分页节点不显示地址/类型**
  - **数组**：可展开元素（多维 `m[3][4]`、结构体数组、嵌套类型）；**超 100 元素自动分页**为
    `arr[99..0]`、`arr[199..100]` 分组节点
  - **枚举**：类型列显示枚举类型名（含数组元素）
  - 勾选实时生效；关闭后选择保留；同名文件重复导入复用根节点
- **右侧监控变量栏（树形实时统计表）**：
  - **默认只显示三列：变量 | 地址 | 当前值**
  - **表头右键菜单可配置列**（默认不选中）：类型、大小、最大值、最小值、均值
  - 树形**保留文件层级**（嵌套结构体展开，默认折叠，展开状态记忆）；无颜色块
  - 加入/移出绘制列表通过：对话框「绘制」列勾选、左下绘制列表右键移除
  - 顶部「只显示已显示项」过滤开关（默认开启可用）
  - 当前值实时刷新（~10Hz 节流）；min/max/均值**增量计算**（O(1)/样本）
  - 统计范围默认"采样开始以来全部"，右键可切「全部/最近 60s/最近 10s」（游标区间 P1）
  - **右键菜单**：**添加变量…**（弹出变量选择对话框）/ **从监控列表移除（按根节点）**
  - 数值按类型格式化（float 小数、整型/寄存器十六进制）；P1 扩展列：RMS、峰峰值、标准差
- **左下部绘制列表（表格）**：显示集管理清单，**表格形式**，列序：
  **颜色 | 变量 | 类型 | 地址 | 大小 | 最大值 | 最小值 | Y Div | Y Offset**
  - Y Div / Y Offset 可双击编辑（与波形拖动/滚轮实时同步）
  - 右键「从显示移除」「通道设置…」
- **波形区（左上部）**：单窗口统一主坐标系；X 默认 [-5,5]、Y 默认 [-100,100]、**零点正中**；
  无图例/无右侧坐标/无曲线名称（配色靠绘制列表色块）；滚轮缩放、拖动平移、**双击恢复默认量程**；
  左下绘制列表 **Y Div / Y Offset** 编辑作用于主 Y 轴（5 格 × V/div，`Auto` 恢复默认）
- **主题（v1.11）**：**浅色主题**（白/浅灰底 + 深色文字），按钮/表格清晰；
  **波形区保持暗色黑底亮线**（示波器惯例）
- **状态栏常驻**：连接状态、实际采样率、读取耗时、超载告警（丢样率/缓冲占用 P1）；
  布局 = 左侧动态消息 + 右侧常驻（连接/速率，`addPermanentWidget`）——
  v1.17 修复：.ui 拖入 QStatusBar 的控件只设 parent 不进布局，悬浮 (0,0) 与左侧消息重影
- **交付**：Windows x64、Qt 6.8.3 **动态链接**、**绿色免安装 zip**（发布时随包携带 Qt DLL 与驱动 DLL；
  `windeployqt --compiler-runtime` 已部署验证）
- **工程文件**：`.gscope`（JSON）保存 ELF/SVD 路径、监控/显示集、采样配置、窗口布局，可复现现场

## 12. 版本规划

### P0（MVP：只读观测闭环）
1. ~~工程骨架：qmake(.pro) + Qt 6.8.3 主窗口（三栏 + 浅色主题）~~ ✅（v1.13 弃用 CMake 改 qmake+Makefile，详见 §2.2）
2. ~~GS32Link 库：ABI 定稿、DLL 加载框架、J-Link 后端（连接/读内存）~~ ✅（另含 sim 模拟器、自动查找 JLinkARM.dll、**gslink（FTDI D2XX 直驱 MPSSE + RISC-V SBA，v1.14 硬件验证读通目标内存**：TAP 状态机/USB 单包压缩/SBCS 位布局/SBDATA0 地址四轮修复；**v1.15 分层重构（FTDI-JTAG/RISC-V-Debug/C-ABI 三层）+ 批量聚合模式**（ftd2xx 调用 6N→~2），详见 §3.5））
3. ~~ELF/DWARF 解析器（基础类型 + 嵌套结构体 + 位域 + 数组展开/分页 + 枚举类型名 + 源文件归属）~~ ✅（DWARF 优先 + symtab 兜底；已用真实 GS32 RISC-V/DWARF5 ELF 验证）
4. ~~采样引擎：POLL deadline 调度、按根变量聚合读、环形缓冲~~ ✅（含类型/位域解码、实际速率统计）
5. ~~波形显示：QCustomPlot 集成、单窗口统一坐标系、零点居中、无图例/右侧坐标、监控/显示两级（独立）~~ ✅
6. .gscope 工程文件 ⬜
7. CSV 导出 ⬜

### P1
OpenOCD 后端、自定义调试器模板、STREAM 模式、SVD 高级防护、
**多分屏窗口**（相关性对比/分析视图并排）、游标系统 + 边沿吸附 + 搜索驱动批量 Δt、
波形值搜索、.gsr 录制回放、变量写、**枚举常量名映射（值→名称显示）**、
超大数组分页采样（不整体读）

### P2
寄存器写、滑块调参、压缩编码、FFT、多语言、安装包
