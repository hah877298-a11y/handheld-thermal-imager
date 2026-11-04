# 固件工程说明

STM32F103ZET6 + 标准外设库（FWLIB）+ Keil MDK5，无操作系统，主循环轮询调度。

## 打开与编译

1. 双击 `USER/ThermalImager.uvprojx`，Keil MDK5 直接打开。
2. 工程已配置好头文件路径与源文件分组，点 Build（F7）即可，无需改动任何选项。
3. 编译产物在 `OBJ/`（首次编译自动生成），目标文件 `OBJ/ThermalImager.hex`。
4. 已编译好的固件在 `../04_烧录文件/ThermalImager_v1.2.hex`。
5. 优化等级 -O2。全量编译结果：Code=58058，RO=8210，RW=152，ZI=25168，0 error 0 warning。

烧录用 ST-Link V2，SWD 四线接板上 J1：3.3V / SWDIO / SWCLK / GND。

## 目录

| 目录 | 内容 |
|---|---|
| `USER/` | `main.c` 主流程、中断服务、时钟与工程文件 |
| `HARDWARE/` | 各功能模块驱动，见下表 |
| `CORE/` | Cortex-M3 内核与启动文件 |
| `SYSTEM/` | 延时、串口、位带操作等基础函数 |
| `STM32F10x_FWLib/` | ST 官方标准外设库 |

## 驱动模块

| 目录 | 作用 |
|---|---|
| `HARDWARE/IIC` | 软件 I2C（PB6/PB7），MLX90640 通信 |
| `HARDWARE/MLX90640` | 传感器初始化、参数读取、帧数据解算 |
| `HARDWARE/INTERP` | 定点 Q8 双线性插值，32×24 → 256×192 |
| `HARDWARE/PALETTE` | 铁红 / 彩虹 / 黑白三套伪彩色查找表 |
| `HARDWARE/TRACK` | 高低温极值搜索与 EMA 平滑，输出双色十字坐标 |
| `HARDWARE/LCD` | ATK-MD0280 并口屏驱动（FSMC） |
| `HARDWARE/SDIO`、`HARDWARE/FATFS`、`HARDWARE/BMP` | TF 卡挂载与 BMP 编码存图 |
| `HARDWARE/KEY`、`HARDWARE/LED`、`HARDWARE/BEEP` | 按键扫描、指示灯、蜂鸣器与超温报警状态机 |

## 运行逻辑

上电初始化 → 等待 MLX90640 首帧有效 → 读帧并解算温度 → 双线性插值生成 256×192 图 → 按调色板上色刷屏 → 更新极值十字 → 检查按键与超温阈值。串口 115200 每秒输出一行统计（`F=` 帧号、`Ta` 环境温度、`min/max`、渲染耗时），便于联调。

## 已知限制

- 传感器读取为软件 I2C，单帧约 200 ms，整机有效帧率约 3.9 Hz。
- 显示为整帧 16 位并口刷屏，约 15 ms；手机拍摄录像时可能出现水平色带，属显示刷新与相机帧率的时间混叠，肉眼不可见。

