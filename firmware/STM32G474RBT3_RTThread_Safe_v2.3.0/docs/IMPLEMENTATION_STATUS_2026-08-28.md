# v2.2.0 历史实现状态（RT-Thread）

> 本页保留原始审查日期文件名，内容仅描述 v2.2.0。当前 v2.3.0 实现和证据以
> 根目录 `README.md`、`RELEASE_NOTES_v2.3.0.md` 与 `remade_master.md` 最后章节为准。

目标：实物确认的 `STM32G474RBT3`，128 KiB Flash、96 KiB普通 SRAM、
32 KiB CCM SRAM；内核为RT-Thread Nano 4.1.1。控制板网表中的RET3字段按
EDA元数据错误处理，Keil目标保持RBTx/128 KiB。

## 已在源码中完成

- 使用ST完整118项G474向量表、CMSIS设备头、RT-Thread Cortex-M4
  ARMClang上下文端口，并启用`RT_USING_LIBC`和栈溢出检查。
- 与裸机版共用已审计引脚：`CHE2 <- PA9/Timer A CH2`、
  `CHF1 <- PA10/Timer B CH1`，不是PC9/PC6或Timer E/F。
- X1=8 MHz，Range 1 Boost、Flash 4WS，PLL=`8/2*85/2=170 MHz`；
  HSE/PLL/CSS/读回失败保留HSI16并锁定输出。RT SysTick按实际时钟配置。
- USART1固定使用HSI16内核，PB6/PB7 AF7、115200，并由IRQ5接收环形缓存。
- HRTIM x32、PER寄存器=54,399（硬件计数PER+1=54,400），理论100 kHz；DLL通过后只运行Timer A采样时基，
  所有PWM输出、OE#、PA2 inhibit和PA5辅助电源仍处于安全态。
- HRTIM_TRG2每周期触发ADC1 PA0/PA1和ADC2 PC0/PC1注入序列；两通道每ADC
  预算约2.824 us。ADC ISR优先级2且不调用任何RT API。
- ADC序列重复、JQOVF/OVR、满量程、VBUS OVP和校准后双路OCP均直接关断；
  快采率按真实tick间隔归一化，而非假定正好1秒。
- Timer A运行而快采序列连续5 ms无进展时，main锁存ADC超时并安全关断。
- 调度采用少线程/单写者：fault_guard优先级3，main优先级6，telemetry
  优先级20，idle优先级31；外环10 ms、MPPT 100 ms作为main内定时作业。
- main 4096 B、fault_guard 512 B、telemetry 384 B及idle栈放入CCM；
  DMA可见对象留在普通SRAM。
- 命令安全、400 V目标上限、MPPT模式选择和单行FireWater协议均已整改。

## 有意保持阻断

- `BOARD_POWER_OUTPUT_ARMING_ENABLE=0`，工程中没有HRTIM `OENR`写入。
- FLT/EEV在功率板上没有硬件故障源，15 V没有MCU反馈；软件逐周期ADC只能
  作为后备关断，不能解除功率输出硬锁。
- 快速电流环尚未更新PWM比较值；当前100 kHz ISR只采样、快照和保护。
- 晶振负阻/启振时间、100 kHz绝对频率、ISR最坏执行时间、线程栈水位和
  长期调度都必须上板测量。

## 证据边界

- 已完成工程XML、网表、118项向量、内存、调度契约、RT内核/端口和项目
  自有C文件的主机GCC编译级检查。
- 本环境没有Keil ARMClang、ST-LINK和实物板；不能声称RT-Thread v2.2.0
  已真实Rebuild、烧录或正常运行。
- 裸机v1.8.1实板证据不等于RT-Thread证据。当前RT验收与维护记录以根目录
  `remade_master.md`为准。
