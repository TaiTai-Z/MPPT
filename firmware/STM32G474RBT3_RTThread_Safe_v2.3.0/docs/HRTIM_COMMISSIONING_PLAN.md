# HRTIM、100 kHz与逐周期ADC验收计划

当前v2.3.0已经实现“安全采样时基”、受门控的CMP写回，以及A2/B1双相180°功率
输出代码路径；所有功率安全门仍为0，所以运行时不会放行输出。采样与功率许可必须
分开验收：Timer A计数和ADC触发可以运行，但U11三组OE#、PA2 inhibit、
PWM GPIO以及HRTIM OEN始终阻断功率输出。

## 阶段0：本版本静态契约

- X1=8 MHz，PLL=`8/2*85/2=170 MHz`，Range 1 Boost、Flash 4WS。
- HSE ready后启用CSS；HSE/PLL/CSS和PLL M/N/R读回任一不符即保持HSI16。
- HRTIM CK_PSC=0（x32），PER寄存器=54,399；硬件实际计数PER+1=54,400：

  `170,000,000 * 32 / (54,399+1) = 100,000 Hz`。

- DLL校准必须出现DLLRDY；只启用Timer A计数器。
- Timer A CMP2=(PER+1)/2，每周期只生成一次HRTIM_TRG2；Timer B CMP2不加入
  ADC触发，避免一个10 us周期产生两次采样。
- ADC1注入PA0/PA1，ADC2注入PC0/PC1；HCLK/4=42.5 MHz，47.5周期采样，
  每个ADC两通道约2.824 us。
- 工程不写HRTIM `OENR`，全部12路PWM脚为模拟态；OE1/PB2、OE2/PC15、
  OE3/PC14为高，PA2 inhibit为高，PA5 aux为低。
- JQOVF、OVR、ADC半序列重复、满量程、VBUS OVP和校准后双路OCP都会
  停Timer A并执行物理关断。
- Timer A运行但快采序列连续5 ms不增长时，由1-ms服务锁存ADC_TIMEOUT并
  关断，覆盖两个ADC中断同时静默的情况。

## 阶段1：无母线实板验证

1. 断开直流母线和15 V功率驱动，保留控制板3.3 V与调试串口。
2. Clean/Rebuild/Download并手动NRST。
3. `CLOCKDIAG`必须显示：
   `source=HSE8_PLL170 sys_hz=170000000 hse_ready=1 pll_ready=1 boost=1 flash_ws=4 css=1 fallback=0 error=0`。
4. `HRTIMDIAG`必须显示：
   `backend_ready=1 timing=1 clock_validated=1 period=54399 adc_trigger=1 dll=1 sampling=1 calculated_hz=100000 pwm_high_z=1`。
5. 运行至少10 s后查看`CONTROL`：`measured_hz`应接近100000，
   `incomplete=0`、`fast_fault=0`。该值验证触发和ISR吞吐，不是独立
   绝对频率基准。
6. 反复执行`PING`、`CONTROL`、`HRTIMDIAG`，确认串口不会使快采
   `incomplete`增长。RT版本还需执行`RTDIAG`。

## 阶段2：独立频率与晶振验证

- 用高阻低电容探头或专用时钟测试固件测HSE/安全MCO测试点；PA8在本板是
  CHE1的U11输入，不得在默认固件中长期输出MCO。若临时使用PA8 MCO，必须
  先实测OE3#为高、PA2 inhibit为高且断开功率母线。
- C8=C9=12 pF给出的外接等效负载是6 pF；晶体标称CL=10 pF，因此需要约
  4 pF的引脚和PCB总寄生才匹配。ESR标称250 ohm，必须按AN2867测启振时间、
  负阻/增益裕量和温压角，不能只由网表确认可靠性。
- 用示波器或频率计验证100 kHz时，首选专用无功率诊断固件/测试点。不得为
  了测频提前拉低OE3#或PA2 inhibit。
- 记录仪器型号、探头、电源电压、温度、测得HSE和100 kHz频率及误差。

## 阶段3：冻结硬件保护真值

1. 确认控制板U6到实际功率板的连续性。
2. 将比较器/驱动故障真实接入HRTIM FLT；当前非同步功率板的
   FLT1..6/EEV1..2仅终止于连接器，不构成硬件关断链。
3. 增加MCU可读15 V PGOOD或合适的隔离/分压监测。
4. 完成两路电流零点、增益、方向，VPV/VBUS比例和温度校准。
5. 使用低压限流电源逐项注入故障，证明硬件无需CPU指令即可关断。
6. 测ADC ISR最坏执行时间；目标低于10 us周期的30%，并记录嵌套中断情况。

## 阶段4：功率PWM开发（不在本版本）

- 非同步板只允许从原生Timer A output2（PA9→外部CHE2）和Timer B output1
  （PA10→外部CHF1）开始；不得按外部字母误选Timer E/F或PC9/PC6。
- UCC27511两路是独立单端通道，不能凭同步板UCC21550的约99 ns硬件死区
  值为其虚构互补死区。
- 先在1 kHz、低压、限流、假负载条件验证极性与相位，再逐级到100 kHz。
- 只有阶段1至3的记录完整后，才能评审三个安全编译门；改单个宏不等于完成
  调试或保护验收。

ST官方B-G474E-DPOW1 HRTIM同步Buck示例只能作为寄存器/API参考，不能直接
作为本PCB的已验证功率初始化：
<https://github.com/STMicroelectronics/STM32CubeG4/blob/master/Projects/B-G474E-DPOW1/Examples_MIX/HRTIM/HRTIM_Buck_Sync_Rect/Src/main.c>
