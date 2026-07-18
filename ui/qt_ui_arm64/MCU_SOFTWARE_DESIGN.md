# STM32H750 机器人底盘 MCU 软件系统设计

> 陪护机器人设计报告 — MCU 软件系统部分

## 1. 系统概述

MCU 采用 **STM32H750VBT6**（ARM Cortex-M7, 480MHz, LQFP-100），在机器人系统中负责**底盘运动控制和云台舵机驱动**。通过 UART4 和 UART8 双串口接收上位机（RK3588）的 JSON 格式指令，驱动 2 路直流电机（TB6612 双 H 桥芯片）和 2 路舵机云台，并具备编码器反馈和看门狗安全保护。

## 2. 软件架构

```
┌──────────────────────────────────────────────────────────────┐
│                      main.c (应用层)                          │
│         系统初始化序列 / 主循环 / 电机裸机测试函数               │
├──────────────────────────────────────────────────────────────┤
│  protocol.c           tb6612.c              servo.c          │
│  (协议层)             (电机驱动层)           (舵机驱动层)       │
│  帧状态机/JSON解析     TB6612真值表控制       PWM 50Hz舵机     │
│  cmd派发/看门狗        编码器反馈/速度计算     双轴角度映射      │
├──────────────────────────────────────────────────────────────┤
│               HAL 外设抽象层 (CubeMX 生成)                      │
│  tim.c / gpio.c / usart.c / i2c.c / stm32h7xx_it.c           │
├──────────────────────────────────────────────────────────────┤
│  TIM1(电机PWM) │ TIM2(舵机PWM) │ TIM3/4/5/8(编码器) │ TIM6(滴答) │
│  UART4/UART8(通信) │ I2C1(I2C2)(外设) │ GPIO(方向/传感器)       │
└──────────────────────────────────────────────────────────────┘
```

### 核心源文件

| 文件 | 功能描述 |
|------|----------|
| `BSP/tb6612.c/h` | TB6612 电机驱动：数据结构定义、单电机/双电机 API、TB6612 真值表实现、编码器反馈与速度计算 |
| `BSP/servo.c/h` | 双轴舵机云台：50Hz PWM 控制、脉宽-角度换算、双轴独立中位 |
| `Core/Src/protocol.c` | 串口通信协议 v2：帧状态机、XOR 校验、cmd 字段派发、300ms 看门狗、JSON 轻量解析 |
| `Core/Src/main.c` | 系统入口：完整初始化序列、4 个裸机电机测试函数 MotorXX_Run() |

## 3. 系统时钟树

| 时钟节点 | 频率 | 推导 |
|----------|------|------|
| HSI 内部振荡器 | 64 MHz | DIV1 |
| PLL 输入 (PLLM=/4) | 16 MHz | |
| PLL VCO (PLLN=×60) | 960 MHz | |
| PLL P 输出 (PLLP=/2) | 480 MHz | |
| **SYSCLK** | **480 MHz** | = PLL P 输出 |
| CPU 核心时钟 | 480 MHz | |
| **HCLK (AHB 总线)** | **240 MHz** | SYSCLK / 2 |
| APB1 / APB2 | 120 MHz | HCLK / 2 |
| **定时器时钟 (所有 TIM)** | **240 MHz** | 2 × APB (H7 特性: APB 分频>1时定时器时钟加倍) |

## 4. 定时器资源分配

| 定时器 | 类型 | 模式 | PSC | ARR | 输出频率 | 用途 |
|--------|------|------|-----|-----|----------|------|
| **TIM1** | 高级 | PWM1, 4通道 | 240 | 999 | **1 kHz** | 4路电机速度控制 |
| **TIM2** | 通用 | PWM1, 2通道 | 240 | 19999 | **50 Hz** | 2路舵机云台 |
| **TIM3** | 通用 | 编码器 TI12 | 0 | 65535 | — | 右轮正交编码器 |
| **TIM4** | 通用 | 编码器 TI12 | 0 | 65535 | — | 备用编码器 |
| **TIM5** | 通用 | 编码器 TI12 | 0 | 65535 | — | 左轮正交编码器 |
| **TIM6** | 基本 | 定时中断 | 240 | 999 | **1 kHz** | 1ms 系统滴答 / 看门狗时基 |
| **TIM8** | 高级 | 编码器 TI12 | 0 | 65535 | — | 备用编码器 |

### 定时器通道与 GPIO 详细映射

**TIM1 — 电机 PWM (1kHz, 0~999 占空比):**

| 通道 | GPIO | AF | 标签 | 电机 |
|------|------|-----|------|------|
| CH1 | PE9 | AF1 | LPWMB | 右轮 (MotorLB) |
| CH2 | PE11 | AF1 | LPWMA | 左轮 (MotorLA) |
| CH3 | PE13 | AF1 | RPWMB | 右后 (备用) |
| CH4 | PE14 | AF1 | RPWMA | 右前 (备用) |

**TIM2 — 舵机 PWM (50Hz, 500~2500μs 脉宽):**

| 通道 | GPIO | AF | 轴 | 说明 |
|------|------|-----|-----|------|
| CH1 | PA5 | AF1 | Pan 水平 | 摄像头左右旋转 |
| CH2 | PB3 | AF1 | Tilt 俯仰 | 摄像头上下俯仰 |

**编码器定时器:**

| 定时器 | CH1 / CH2 | AF | 标签 | 对应电机 |
|--------|-----------|-----|------|----------|
| TIM5 | PA0 / PA1 | AF2 | LE1A / LE2B | 左轮 (MotorLA) |
| TIM3 | PA6 / PA7 | AF2 | RE1A / RE2B | 右轮 (MotorLB) |
| TIM4 | PD12 / PD13 | AF2 | — | 备用 |
| TIM8 | PC6 / PC7 | AF3 | — | 备用 |

## 5. 电机驱动设计

### 5.1 TB6612 控制接口

每个电机使用 3 根信号线连接 TB6612 芯片：

| 电机 | IN1 (方向) | IN2 (方向) | PWM (速度) | 编码器 |
|------|-----------|-----------|-----------|--------|
| 左轮 (MotorLA) | PE15 (LAIN1) | PE12 (LAIN2) | PE11 TIM1_CH2 | TIM5 PA0/PA1 |
| 右轮 (MotorLB) | PE10 (LBIN1) | PB2 (LBIN2) | PE9 TIM1_CH1 | TIM3 PA6/PA7 |

> **注**: 右轮在代码中标签为 LB（Left-Back），因其挂载在左侧 TB6612 芯片的 B 通道。物理上该电机安装在机器人右侧。RA/RB 两路预留但当前因右侧 TB6612 硬件问题未使用。

### 5.2 TB6612 真值表实现

| 模式 | IN1 | IN2 | PWM 占空比 | 电机行为 |
|------|-----|-----|-----------|----------|
| **正转** (Forward) | H (SET) | L (RESET) | 1~999 | 前进驱动 |
| **反转** (Reverse) | L (RESET) | H (SET) | 1~999 | 后退驱动 |
| **制动** (Brake) | H (SET) | H (SET) | 0 | 短路制动 (快速停止) |
| **滑行** (Coast) | L (RESET) | L (RESET) | 0 | 惯性滑行 |

零速时默认使用**制动模式** (IN1=1, IN2=1)，使电机快速停止。滑行模式 (IN1=0, IN2=0) 作为独立 API 提供。

### 5.3 速度范围与 PWM 参数

| 参数 | 值 |
|------|-----|
| PWM 频率 | 1 kHz (TIM1) |
| 占空比范围 | 0 ~ 999 (TIM1 ARR=999) |
| 分辨率 | 1000 级 |
| 0 = 停止 | 999 = 100% 占空比 |

### 5.4 方向补偿机制

左轮 (MotorLA) 物理反向安装，通过 `motor_dir = -1` 进行软件补偿：

```
实际方向 = 指令方向 × motor_dir
  正值 → 触发 Reverse (IN1=L, IN2=H) → 电机物理正转
  负值 → 触发 Forward (IN1=H, IN2=L) → 电机物理反转
```

右轮 (MotorLB) 正向安装，`motor_dir = +1`，无需补偿。

### 5.5 核心数据结构

```c
typedef struct {
    GPIO_TypeDef      *IN1_Port;        // 方向脚 1 端口
    uint16_t           IN1_Pin;         // 方向脚 1 引脚
    GPIO_TypeDef      *IN2_Port;        // 方向脚 2 端口
    uint16_t           IN2_Pin;         // 方向脚 2 引脚
    TIM_HandleTypeDef *htim;            // PWM 定时器
    uint32_t           TIM_Channel;     // PWM 通道
    TIM_HandleTypeDef *encoder_htim;    // 编码器定时器
    int8_t             encoder_dir;     // 编码器方向系数 (±1)
    int8_t             motor_dir;       // 安装方向系数 (±1)
    int32_t            prev_encoder;    // 上次编码器值
    int32_t            speed;           // 当前速度 (脉冲/采样周期)
} TB6612_Motor_t;

typedef struct {
    TB6612_Motor_t MotorLA;  // 左轮
    TB6612_Motor_t MotorLB;  // 右轮
    TB6612_Motor_t MotorRA;  // 预留
    TB6612_Motor_t MotorRB;  // 预留
} TB6612_Car_t;
```

### 5.6 主要 API

| 函数 | 作用 | 说明 |
|------|------|------|
| `TB6612_Motor_Init()` | 初始化单电机 | 配置 GPIO + 启动 PWM + 启动编码器 |
| `TB6612_Motor_Forward()` | 单电机正转 | IN1=1, IN2=0 |
| `TB6612_Motor_Reverse()` | 单电机反转 | IN1=0, IN2=1 |
| `TB6612_Motor_Brake()` | 单电机制动 | IN1=1, IN2=1, PWM=0 |
| `TB6612_Motor_Stop()` | 单电机滑行 | IN1=0, IN2=0, PWM=0 |
| `TB6612_Motor_SetSpeed()` | 带符号速度 | 自动方向补偿 + 正转/反转/制动 |
| `TB6612_Car_Init()` | 初始化整车 | 初始化 LA + LB |
| `TB6612_Car_Run(left, right)` | 差速驱动 | left→LA, right→LB |
| `TB6612_Car_Brake()` | 整车制动 | 全部电机短路制动 |

## 6. 编码器反馈

### 6.1 配置参数

| 参数 | TIM3 (右轮) | TIM5 (左轮) |
|------|------------|------------|
| 编码器模式 | TI12 (4倍频) | TI12 (4倍频) |
| 计数器位宽 | 16 bit (ARR=65535) | 16 bit (ARR=65535) |
| 输入滤波 | 8 | 8 |
| IC1 极性 | Rising | Rising |
| IC2 极性 | Falling | Rising |

TI12 模式在 CH1 和 CH2 的上升沿和下降沿均计数，提供 **4倍频分辨率**。

### 6.2 速度计算方法

```c
speed = 当前编码器值 − 上次编码器值    // 脉冲数/采样周期
```

速度更新在 TIM6 1ms 中断中调用，记录每毫秒的编码器增量。

## 7. 舵机云台控制

### 7.1 PWM 参数

| 参数 | 值 | 说明 |
|------|-----|------|
| PWM 频率 | **50 Hz** | TIM2, PSC=240, ARR=19999 |
| 周期 | 20 ms | 标准舵机信号 |
| 脉宽范围 | **500 ~ 2500 μs** | 2.5% ~ 12.5% 占空比 |
| 中位脉宽 | 1500 μs | 7.5% 占空比 |
| 最大偏移 | ±1000 μs | SERVO_RANGE |
| 分辨率 | 1 μs/tick | 时钟 1MHz |

### 7.2 双轴控制

| 轴 | 通道 | 引脚 | 软件输入范围 | 方向 | 说明 |
|----|------|------|-------------|------|------|
| Pan (水平) | TIM2_CH1 | PA5 | -1.0 ~ +1.0 | 已对调 | 正=右转, 负=左转 |
| Tilt (俯仰) | TIM2_CH2 | PB3 | -1.0 ~ +1.0 | 默认 | 正=上仰, 负=下俯 |

### 7.3 脉宽计算公式

```c
pan_pulse  = 1500 + (−pan × 1000)    // pan 方向已对调
tilt_pulse = 1500 + ( tilt × 1000)   // tilt 默认方向
```

结果钳位至 [500, 2500] μs。

## 8. 串口通信协议 (v2)

### 8.1 物理层

| 参数 | 值 |
|------|-----|
| 接口 | UART, 全双工 |
| 波特率 | 115200 |
| 数据格式 | 8N1 (8数据位, 无校验, 1停止位) |
| 主命令端口 | **UART4** (PD0=RX, PD1=TX) |
| 辅命令端口 | **UART8** (PE0=RX, PE1=TX) |
| 调试输出 | UART8 printf 重定向 |

### 8.2 帧格式

```
┌──────┬──────┬──────┬──────────────────┬──────┬──────┬──────┐
│ 0xAA │ 0x55 │ LEN  │  JSON payload    │ XOR  │ 0x0D │ 0x0A │
│帧头1 │帧头2 │1字节 │   LEN bytes       │1字节 │  \r  │  \n  │
│      │      │      │   (1 ~ 250)       │校验  │      │      │
└──────┴──────┴──────┴──────────────────┴──────┴──────┴──────┘
```

**XOR 校验**: `XOR = LEN ^ payload[0] ^ payload[1] ^ ... ^ payload[LEN-1]`

### 8.3 接收状态机

```
WAIT_H1 → WAIT_H2 → WAIT_LEN → WAIT_DATA → WAIT_XOR → WAIT_CR → WAIT_LF
    ↑                                                            │
    └──────────────────── (超时/错误/完成后) ─────────────────────┘
```

- WAIT_H1/WAIT_H2: 等待帧头 0xAA 0x55
- WAIT_LEN: 读取长度，初始化 XOR
- WAIT_DATA: 逐字节接收 payload，链式 XOR
- WAIT_XOR: 校验比对
- WAIT_CR/WAIT_LF: 帧尾确认

### 8.4 命令类型 (v2)

| 命令 | JSON 示例 | 行为 | 看门狗 |
|------|----------|------|--------|
| `move` | `{"cmd":"move","v":0.5,"w":0}` | 差速驱动 | 重置 300ms |
| `ptz` | `{"cmd":"ptz","pan":0,"tilt":0.3}` | 舵机控制 | 重置 300ms |
| `move_ptz` | `{"cmd":"move_ptz","v":0.5,"w":0.1,"pan":0,"tilt":0.3}` | 底盘+云台 | 重置 300ms |
| `stop` | `{"cmd":"stop"}` | 电机急停 + 舵机回中 | **关闭** |
| `ptz_home` | `{"cmd":"ptz_home"}` | 仅舵机回中位 | 不启动 |

### 8.5 差速驱动公式

```
left  = v × 999 − w × 999 × 0.5
right = v × 999 + w × 999 × 0.5
```

| 字段 | 类型 | 范围 | 说明 |
|------|------|------|------|
| v | float | -1.0 ~ +1.0 | 线速度，正=前进 |
| w | float | -1.0 ~ +1.0 | 角速度，正=左转 |

结果钳位至 [-999, 999]。

### 8.6 应答帧

| 情况 | 应答 |
|------|------|
| 成功 | `{"ack":"ok"}` |
| 长度错误 | `{"ack":"err","msg":"len"}` |
| 校验错误 | `{"ack":"err","msg":"checksum"}` |

### 8.7 双串口支持

UART4 和 UART8 均可接收命令，共享同一个协议状态机。接收在中段回调中完成：

```
HAL_UART_RxCpltCallback(UART) → protocol_feed(byte) → 状态机处理 → handle_frame()
```

应答统一从 UART4 发出。

## 9. 安全机制

### 9.1 软件看门狗

| 参数 | 值 |
|------|-----|
| 时基 | TIM6 1ms 中断 |
| 超时 | **300 个 tick = 300 ms** |
| 触发条件 | 收到 move / ptz / move_ptz 命令时重置为 300 |
| 超时动作 | `TB6612_Car_Run(0,0)` + `Servo_Stop()` |
| 关闭 | stop 命令将计数器清零 |

上位机在按键按住期间每 **80ms** 重发一条命令，300ms 超时提供约 **3.75 倍**的余量，容忍偶发丢帧或网络抖动。

### 9.2 电机供电使能

`DJ_EN` (PA4) 控制电机和舵机的电源轨道。上电默认低电平（断电），在 `Servo_Init()` 中拉高。确保初始化期间电机不会误动作。

### 9.3 故障处理

Cortex-M7 的 5 种硬件故障 (NMI, HardFault, MemManage, BusFault, UsageFault) 均实现中断服务函数，进入无限循环便于调试。`Error_Handler()` 关闭全局中断并死循环。

## 10. GPIO 引脚分配总表

### 电机控制 (输出)

| 标签 | GPIO | 功能 |
|------|------|------|
| LAIN1 | PE15 | 左轮 IN1 |
| LAIN2 | PE12 | 左轮 IN2 |
| LBIN1 | PE10 | 右轮 IN1 |
| LBIN2 | PB2 | 右轮 IN2 |
| RAIN1 | PC9 | 预留 IN1 |
| RAIN2 | PC8 | 预留 IN2 |
| RBIN1 | PD15 | 预留 IN1 |
| RBIN2 | PD14 | 预留 IN2 |

### 电机 PWM (TIM1 AF)

| 标签 | GPIO | 通道 |
|------|------|------|
| LPWMA | PE11 | TIM1_CH2 |
| LPWMB | PE9 | TIM1_CH1 |
| RPWMA | PE14 | TIM1_CH4 |
| RPWMB | PE13 | TIM1_CH3 |

### 编码器 (TIMx AF)

| 标签 | GPIO | 定时器 |
|------|------|--------|
| LE1A | PA0 | TIM5_CH1 |
| LE2B | PA1 | TIM5_CH2 |
| RE1A | PA6 | TIM3_CH1 |
| RE2B | PA7 | TIM3_CH2 |

### 舵机 PWM (TIM2 AF)

| 功能 | GPIO | 通道 |
|------|------|------|
| Pan 水平 | PA5 | TIM2_CH1 |
| Tilt 俯仰 | PB3 | TIM2_CH2 |

### 通信 (UART AF)

| 端口 | RX | TX |
|------|-----|-----|
| UART4 | PD0 (AF8) | PD1 (AF8) |
| UART8 | PE0 (AF8) | PE1 (AF8) |
| UART5 | PB12 (AF14) | PC12 (AF8) |
| USART2 | PA3 (AF7) | PA2 (AF7) |

### 电源与指示

| 标签 | GPIO | 方向 | 功能 |
|------|------|------|------|
| POWER_EN | PE2 | 输出 | 系统电源保持 |
| DJ_EN | PA4 | 输出 | 电机/舵机供电使能 |
| LED | PA15 | 输出 | 状态指示灯 |
| BEEP | PE3 | 输出 | 蜂鸣器 |

### 按键与传感器 (输入)

| 标签 | GPIO | 模式 |
|------|------|------|
| KEY1~2 | PE4~PE5 | 输入 |
| KEY3 | PE6 | 下降沿中断 |
| KEY4 | PC13 | 下降沿中断 |
| SensorL/R | PD6/PB5 | 下拉输入 |
| RED1~8 | PB13~15, PD8~11, PA8 | 输入 |

## 11. 初始化序列

MCU 上电后的精确初始化顺序：

```
 1. MPU_Config()            — 内存保护单元配置
 2. HAL_Init()              — HAL 库初始化, SysTick 配置
 3. SystemClock_Config()    — 时钟树: HSI→PLL→480MHz SYSCLK
 4. MX_GPIO_Init()          — 所有 GPIO 初始化 (输出默认电平, 输入配置, EXTI)
 5. MX_I2C1_Init()          — I2C1 (OLED 显示屏)
 6. MX_I2C2_Init()          — I2C2 (预留)
 7. MX_TIM1_Init()          — 电机 PWM, 1kHz, 4通道
 8. MX_TIM3_Init()          — 右轮编码器, TI12 模式
 9. MX_TIM5_Init()          —  左轮编码器, TI12 模式
10. MX_TIM6_Init()          — 1ms 系统滴答定时器
11. MX_UART8_Init()         — 调试串口 + 辅命令口
12. MX_UART5_Init()         — 预留串口
13. MX_USART2_UART_Init()   — 预留串口
14. MX_UART4_Init()         — 主命令串口
15. MX_TIM2_Init()          — 舵机 PWM, 50Hz, 2通道
16. MX_TIM4_Init()          — 备用编码器
17. MX_TIM8_Init()          — 备用编码器
18. TB6612_Init()           — 电机 BSP 层初始化 (PWM启动, 编码器启动, 制动)
19. Servo_Init()            — 舵机 PWM 启动 + DJ_EN 供电使能
20. HAL_TIM_Base_Start_IT() — 使能 TIM6 1ms 中断
21. protocol_init()         — 协议状态机复位 + UART4 接收启动
22. HAL_UART_Receive_IT()   — UART8 接收启动
23. while(1) { HAL_Delay(10); } — 主循环，10ms 周期
```

## 12. 通信接口汇总

| 外设 | 引脚 | 速率 | 用途 |
|------|------|------|------|
| UART4 | PD0/PD1 | 115200 | 上位机主控制链路 (RK3588 → MCU) |
| UART8 | PE0/PE1 | 115200 | 调试输出 (printf) + 辅控制链路 |
| I2C1 | PB8/PB9 | Fast | SSD1306 OLED (128×64) |
| I2C2 | PB10/PB11 | Fast | 预留 (传感器扩展) |

## 13. 关键技术参数汇总

| 参数 | 数值 |
|------|------|
| MCU 型号 | STM32H750VBT6 |
| 内核 | ARM Cortex-M7 |
| 主频 | 480 MHz |
| 电机 PWM 频率 | 1 kHz |
| 电机速度分辨率 | 1000 级 (0~999) |
| 编码器模式 | 4 倍频正交 |
| 舵机 PWM 频率 | 50 Hz |
| 舵机脉宽范围 | 500 ~ 2500 μs |
| 串口波特率 | 115200 |
| 协议帧格式 | 0xAA 0x55 + LEN + JSON + XOR + CRLF |
| 看门狗超时 | 300 ms |
| 命令保活间隔 | 80 ms |
| CubeMX 版本 | 6.17.0 |
| HAL 固件包 | STM32Cube FW_H7 V1.13.0 |
| 工具链 | MDK-ARM V5.32 |
