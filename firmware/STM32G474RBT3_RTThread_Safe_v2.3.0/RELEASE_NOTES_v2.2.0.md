# RT-Thread Nano 安全框架 v2.2.0

日期：2026-08-29  
目标：STM32G474RBT3（128 KiB Flash，96 KiB SRAM + 32 KiB CCM）  
内核：RT-Thread Nano 4.1.1，ARM Compiler 6 配置包含 `RT_USING_LIBC`

## 本次修复

RT-Thread 版与裸机版共用同一套网表真值、时钟、HRTIM 安全时基、逐周期 ADC、
校准存储和功率状态机：`CHE2=PA9/Timer A CH2`、`CHF1=PA10/Timer B CH1`；
HSE/PLL 为 170 MHz，HRTIM x32/PER寄存器54399（PER+1=54400）为理论 100 kHz；ADC1/ADC2 注入
序列每周期取得两路电流和 VPV/VBUS。电流使用有符号 `I1+I2` 重构，P&O、动态
VBUS OVP、OTP/NTC 诊断、传感器诊断、Boost 可达性和 W25Q128 双槽校准均已纳入。
UART 改为中断环形缓存；输出不等待线缆发送，避免控制任务被串口阻塞。

`MODE OFF`、校准、`FAULT CLEAR` 和外部 FLT/EEV 锁存均进入统一安全闸门；
故障影子会清零，故障码 `0x0701..0x0705` 不再复用。FireWater 心跳保持单行
`Vin,Vo,Iin,Iout,Duty,T1,T2,State,Fault`。

## 调度审查

RT-Thread 数字越小优先级越高：

| 上下文 | 优先级 | 作用 |
|---|---:|---|
| ADC1_2 ISR | NVIC 2 | 100 kHz 注入采样、原始阈值和直接关断；不调用 RT API |
| `fault_guard` | RT 3 | 1 ms FLT/EEV/快采兜底，只发布 pending |
| `main` | RT 6 | 唯一控制状态写者，CLI、1/10/100 ms 控制作业 |
| USART1 ISR | NVIC 5 | RX/TX 环形缓存 |
| `telemetry` | RT 20 | 1 s 心跳到期标志，不直接输出 |
| idle | RT 31 | RT-Thread 空闲线程 |

外环与 MPPT 保留在 main 单写者内，避免多个线程并发写功率状态；PendSV/SysTick
保持最低异常优先级。`RTDIAG` 可读回 NVIC 优先级、线程栈水位和 pending 故障。
这是经过源码审查的合理分配，但必须由目标板压力测试确认最坏执行时间、栈余量
和长期调度稳定性。

## 有意保持的安全门

全部功率 arming/硬件保护/15 V 反馈/温度、电流极性、PV 限值和控制增益确认宏
仍为 0；不写 HRTIM `OENR`，不切换 PWM GPIO AF，不拉低 OE#，不释放 PA2/PA5。
非同步板网表的 FLT1..6/EEV1..2 仅到连接器，15 V 没有 MCU 可读反馈，故不能
把软件逐周期关断冒充异步硬件保护，也不能宣称 MPPT 已可接高能量母线。

## 证据边界

- `[STATIC PASS]` `tests/check_project.py`：RT 内核/端口、工程路径、完整向量、
  128 KiB Flash、96 KiB SRAM + 32 KiB CCM、优先级、网表及时钟/ADC 契约通过。
- `[HOST SYNTAX PASS]` 项目 C 源码及 RT Nano C 单元通过 GCC C11 语法检查；
  第三方内核仅豁免上游未用参数、显式贯穿及主机指针宽度告警。
- `[ARMCLANG NOT RUN]`、`[FLASH NOT RUN]`、`[TARGET NOT RUN]`：本环境没有当前
  2.2.0 的 Keil/ST-LINK/实板证据。历史 v2.1.0 的 ArmClang 日志只属于该版本。

## 验收命令

```text
CLOCKDIAG
HRTIMDIAG
CONTROL
RTDIAG
PROTECT
PINMAP
UARTDIAG
PING
```

100 kHz 的 `calculated_hz` 来自 HRTIM 寄存器算式，`measured_hz` 来自实际触发
序列差值；晶体绝对频率、启振和负阻仍需独立仪器测量。
