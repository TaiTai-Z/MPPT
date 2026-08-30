# STM32G474RBT3 RT-Thread 安全框架 v2.3.0

固件标识：`G474-RBT3-RTT-SAFE-2.3.0`；内核：RT-Thread Nano 4.1.1。
这是仅含 RT-Thread 的版本。本版本仍是功率输出默认锁定的安全诊断/调试固件，
不得把“代码路径存在”解释为“已允许高压功率上电”。

## 本版整改

- USART1 TX 改为 4096 字节中断队列，应用按整帧全入或全丢；main 不再逐字节
  等待线速发送。RX 每个 1-ms 主循环最多处理 32 字节，避免突发命令积压。
- `fault_guard` 发布当前故障电平，不再永久 OR 历史输入；故障锁存只由 main
  单写者维护，`FAULT CLEAR` 后不会被已经消失的旧影子重新锁存。
- 两相输入电流按各自零点、`44 mV/A` 名义增益、校准增益和实测极性换算，
  `Iin=I1+I2`；逐相 OCP 仍使用绝对值。
- P&O 保留 100-mV 最小周期扰动，增加 1/8 IIR、0.2% 相对死区、样本新鲜度、
  正功率/正电流判据、Boost 可达性和 VBUS 限幅无扰切换。
- 新增 PV UVLO、相电流失衡、传感器卡零、母线建立超时、占空比饱和超时、
  300-W 输入功率上限、运行中样本陈旧故障。
- ADC ISR 中加入每相 2.0-A 周期同步占空比削减器；2.5-A OCP 继续直接锁存
  关断。该功能是逐周期限流器，不冒充尚未整定的平均电流 PI。
- 校准参数收紧到电流 35--53 mV/A、电压 0.9--1.1 倍；自动零点时先停
  HRTIM 注入采样，完成后恢复。
- 校准使用 W25Q128 末尾双 4-KiB A/B 槽；关键故障另用 4 个 4-KiB 循环槽，
  CRC32、提交标志和读回验证后才视为有效。
- 补齐双相 A2/B1、180° 相移、PA9/PA10 AF13、比较值写回和受保护上电时序代码；
  当前全部硬件确认门仍为 0，因此 `OENR`、OE3#、PA2、PA5 不会被运行时释放。
- 增加全部 HRTIM 强故障处理程序；中断入口先执行 `ODISR` 和 GPIO 物理关断。

## 已有采样增益与控制增益的边界

现有采样参数直接使用：

| 通道 | 名义换算 | 用途 |
|---|---|---|
| I1/I2 | CC6937S8-3FB030，44 mV/A；各自零点、增益、极性校准 | 工程量、`Iin`、功率、2.0-A限流、2.5-A OCP |
| PV | 网表 225 kΩ/4.42 kΩ及运放网络，另乘 `pv_gain_ppm` | PV电压、MPPT、UVLO/OVP |
| VBUS | 882 kΩ/5.1 kΩ，另乘 `vbus_gain_ppm` | 母线电压、CV、动态/绝对 OVP |

这些是传感与标定增益，不是功率级闭环补偿器的 `Kp/Ki`。外环代码中的定点系数
仍是低压调试候选值；没有电感、电容 ESR、开关延迟、采样延迟与台架频响证据时，
不能宣称其稳定。平均电流 PI 因同样缺少整定依据而保持禁用，当前只实现无需该参数
的逐周期限流器。

用户确认实装输出电容耐压为 450 V；本版不按旧网表的 400-V 标注降低目标。
软件仍维持 `target<=400 V`、绝对 `VBUS OVP=415 V`，为纹波、瞬态、容差和老化
保留余量；不允许把目标设为 450 V。

## 调度架构

RT-Thread 数字越小优先级越高：

| 上下文 | 优先级 | 周期 | 职责 |
|---|---:|---:|---|
| ADC1_2 ISR | NVIC 2 | 100 kHz | 四路注入采样、原始阈值关断、逐周期限流，不调用 RT API |
| fault_guard | RT 3 | 1 ms | 当前 FLT/EEV 电平兜底、物理关断、发布 pending |
| USART1 ISR | NVIC 5 | 字节事件 | RX/TX 环形队列，不阻塞控制任务 |
| main | RT 6 | 1 ms | 唯一控制状态写者；故障、CLI、1-ms外环、100-ms MPPT |
| telemetry | RT 20 | 1 s | 只发布心跳到期标志 |
| idle | RT 31 | 空闲 | RT idle |

main/fault_guard/telemetry 栈分别为 4096/512/384 B；这些栈、idle 栈与 RAM
事件日志放入 32-KiB CCM。ADC/DMA 对象不得放入 CCM。

## 时钟、采样与输出边界

- X1=8 MHz；`HSE/2*85/2=170 MHz`。HSE/PLL/CSS/Flash/PWR 读回失败时回退
  HSI16并锁定输出。
- HRTIM x32、PER=54,399，`170 MHz*32/54,400=100 kHz`；Timer A CMP2
  每周期触发一次 ADC1/ADC2 注入序列。
- ADC HCLK/4=42.5 MHz；ADC1采 PA0/PA1，ADC2采 PC0/PC1。
- CHE2=PA9/HRTIM Timer A output 2；CHF1=PA10/HRTIM Timer B output 1；
  功率模式后端将 Timer B 初相位设为半周期。
- JQOVF/OVR、错序、满量程、过压、过流或 5-ms 序列无进展都会关断并锁存。
- 当前 `BOARD_POWER_OUTPUT_ARMING_ENABLE=0`，且 FLT极性、15-V PGOOD、温度、
  PV边界、电流极性及控制器增益的硬件确认门均为0。默认运行仅启动高阻采样时基。
- 15-V PGOOD和HRTIM FLT运行时后端均采用默认返回0的失败关闭弱实现；不能通过
  只修改确认宏绕过真实板级实现和寄存器读回。

## 构建与验收

1. 运行 `tools\check_project.bat`。
2. 用 ARM Compiler 6.24 Clean/Rebuild `STM32G474_RTThread.uvprojx`，或运行
   `tools\build_keil.bat`。
3. 下载并 NRST，确认：

```text
BOOT G474-RBT3-RTT-SAFE-2.3.0
CLOCK source=HSE8_PLL170 sys_hz=170000000 ... css=1 fallback=0 error=0
```

4. 执行 `RTDIAG`、`RTSTACK`、`HRTIMDIAG`、`CONTROL`、`PROTECT`、`UARTDIAG`、
   `FAULTLOG STATUS`。压力测试后各线程剩余栈应不少于30%。
5. 做至少30分钟 UART/心跳压力、ADC错序/停滞、传感器开短路与低压限流注入，
   并用独立示波器确认100-kHz频率、两相180°关系和ISR最坏执行时间。

v2.2.0 有用户提供的 ArmClang 6.24 全量 Rebuild `0 Error/0 Warning`证据；
v2.3.0 在当前环境只完成静态检查和主机语法检查，尚未执行 ArmClang、烧录或实板
运行。两者不能混用为证据。详见 `remade_master.md` 和 `RELEASE_NOTES_v2.3.0.md`。
