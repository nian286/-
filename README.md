# STM32F407 底层外设驱动实践（寄存器级）

基于 **STM32F407ZGT6** 的裸机外设驱动实践：逐个外设从零实现、上板验证。
其中 **I2C1 / SPI1 / ADC1 为不依赖 HAL 库的寄存器级手写驱动**，并用逻辑分析仪做时序验证。

## 硬件与工具链

| 项 | 内容 |
|---|---|
| 主控 | STM32F407ZGT6（ARM Cortex-M4，168 MHz，1 MB Flash / 192 KB RAM，硬件 FPU） |
| 开发板 | 正点原子 M144Z-M4 小系统板（核心板 + 底板，**无板载 ST-Link**） |
| 编译器 | Arm GNU Toolchain `arm-none-eabi-gcc` 12.2.1 |
| 构建 | GNU Make 3.81（工程由 STM32CubeMX 生成，Toolchain/IDE = Makefile） |
| 编辑器 | VS Code |
| 烧录 | ATK-XISP 串口 ISP（一键下载电路，波特率 ≤ 76800，烧录前需先解除芯片读保护） |
| 仪器 | 逻辑分析仪（24 MHz / 8ch + PulseView）、示波器、USB-TTL 串口模块 |
| 版本控制 | Git |

## 已实现的外设

| 外设 | 实现方式 | 验证方式 |
|---|---|---|
| USART1 收发 + `printf` 重定向 | HAL | 串口助手收发实测 |
| UART 接收（中断 → IDLE → DMA 三级递进） | HAL + **手写环形缓冲区** | 串口发不定长命令控制 LED0，实测通过 |
| TIM14 PWM | HAL | 上板呼吸灯；载波 1 kHz，PSC=83 / ARR=999 |
| GPIO 外部中断 EXTI0 | HAL | PA0（WK_UP）按下翻转 LED1 + 串口打印 |
| I2C1 驱动 SSD1306 OLED | **寄存器级手写**（`Core/Src/oled.c`） | 实物显示边框 / 对角线 / 圆 / 6×8 ASCII 文字 |
| SPI1 主模式 | **寄存器级手写**（`Core/Src/spi.c`） | 短接 MISO↔MOSI 回环自测，8 字节收发全部一致 |
| ADC1 内部温度传感器 | **寄存器级手写**（`Core/Src/adc.c`） | 串口每秒打印温度，raw ≈ 956（约 29 ℃） |
| IWDG / WWDG 看门狗 | **寄存器级手写**（`Core/Src/adc.c`） | 发 `HANG` 命令停喂狗，约 4 s 自动复位 |

## 目录结构

```
.
├── Core/
│   ├── Inc/
│   │   ├── main.h
│   │   ├── oled.h          # SSD1306 I2C 驱动接口
│   │   ├── spi.h           # SPI1 寄存器级驱动接口
│   │   ├── adc.h           # ADC1 + IWDG/WWDG 接口
│   │   ├── stm32f4xx_hal_conf.h
│   │   └── stm32f4xx_it.h
│   └── Src/
│       ├── main.c          # 主流程、状态机、串口命令解析
│       ├── oled.c          # ★ 寄存器级 I2C1 主发送 + SSD1306 驱动
│       ├── spi.c           # ★ 寄存器级 SPI1 驱动 + 回环自测
│       ├── adc.c           # ★ 寄存器级 ADC1 + IWDG/WWDG
│       ├── stm32f4xx_it.c  # 中断服务函数
│       ├── stm32f4xx_hal_msp.c
│       ├── system_stm32f4xx.c
│       ├── syscalls.c
│       └── sysmem.c
├── Drivers/                # STM32F4 HAL 库
├── Makefile
├── STM32F407xx_FLASH.ld
├── startup_stm32f407xx.s
└── f407_blinky.ioc         # CubeMX 工程配置
```

## 编译与烧录

```bash
# 编译（需已安装 arm-none-eabi-gcc 与 make，且在 PATH 中）
make

# 产物在 build/ 下
#   f407_blinky.hex  ← 用于 ATK-XISP 串口 ISP 烧录
#   f407_blinky.bin / .elf / .map
```

烧录步骤（ATK-XISP）：

1. 打开 ATK-XISP，选择串口（如 COM6），波特率 76800
2. 加载 `build/f407_blinky.hex`
3. 勾选：校验、编程后执行、编程前全片擦除、DTR 低电平复位、RTS 高电平进 BootLoader
4. **先点「解除芯片保护」**（否则 RDP 读保护会拦下 bootloader 写入），再点「开始编程」

串口观测：USB-TTL 接 PA9(TX)↔模块 RX、PA10(RX)↔模块 TX，共地，电平拨 3.3 V；
串口助手 115200 / 8-N-1。发送 `LED ON` / `LED OFF` 控制 LED0，发送 `HANG` 触发看门狗复位。

## 关键调试记录

这几个坑是这个工程里最有价值的部分，记录下来避免重复踩：

**1. CubeMX 重新生成代码导致 HardFault（板子全灭）**
现象：Generate Code 之后双 LED 全灭、按键无反应。
定位：逐项核查中断句柄、NVIC 使能、DMA 链接，确认硬件链路无误。
根因：Generate Code 重写 `main()` 时删掉了手写的 `MX_DMA_Init()` 调用 →
DMA 句柄未初始化（全 0）→ `HAL_UARTEx_ReceiveToIdle_DMA` 内部访问 `hdma->Instance`（地址 0）→ HardFault，死在初始化阶段。
修复：把 `MX_DMA_Init();` 放进 `USER CODE BEGIN 2` 保护块内，生成器不再覆盖。

**2. PWM 呼吸灯不亮（LED0 恒灭）**
根因：`HAL_TIM_PWM_Start_IT` 只使能了捕获比较中断（CC1），**未使能更新中断（UPDATE）**；
而呼吸步进写在 `HAL_TIM_PeriodElapsedCallback` 里 → 回调永不触发 → `breath_ccr` 停在初值 0 →
`CCR = 0` → 引脚整周期输出高电平 → active-low LED 恒灭。
修复：改用 `HAL_TIM_PWM_Start()` 启动硬件输出 + 手动 `__HAL_TIM_ENABLE_IT(&htim14, TIM_IT_UPDATE)`。

**3. SPI 回环自测全错**
现象：8 字节仅 1 字节偶然相等，其余 `tx≠rx` 且 `rx=0`。
根因：PA6(MISO) / PA7(MOSI) 的复用功能误写入 `GPIOA->AFR[1]`，
而 **AFRL 只管理 pin0–7、AFRH 管理 pin8–15**，应写 `AFR[0]` → 复用功能未接通 SPI1 → MISO 恒读 0。
修复：改用 `AFR[0]` 一次性掩码配置，8 字节收发全部一致。

**4. 外部中断不触发**
根因：PA0 配成 `PULLUP` 对 WK_UP 无效——按键另一端接 VCC，平时上拉为高、按下仍为高，电平无跳变。
修复：改为 `PULLDOWN` + 上升沿触发。

**5. 看门狗配错导致串口完全无输出**
根因：WWDG 窗口值 `W = 0x55` 使主循环第一圈喂狗时计数器仍在窗口上限之上 → 立即复位 →
复位频率过高，串口表现为空白/乱码。
修复：`W` 改为 `0x7F`（退化为纯超时模式），并把 IWDG 重装载值放宽到约 4 s。

## 已知问题 / 未完成

- 输入捕获、NVIC 优先级分组尚未覆盖
- 手头的 HSDAP（CMSIS-DAP，VID_04D8/PID_00DF 非标）无法使用，SWD 在线调试功能延后
- 尚未引入 RTOS（FreeRTOS / RT-Thread），目前为裸机前后台 + 状态机结构
- `Drivers/` 下的 HAL 库为 CubeMX 生成，未做裁剪

## 说明

本仓库为个人学习实践记录，代码由本人编写、编译、烧录并在真实硬件上验证。
参考了《STM32 库开发实战指南》与江科大自化协公开教程，驱动部分为自行实现。
