# Hardware netlist audit — 2026-08-28

This audit joins the supplied 2026-08-28 STM32G474 control-board and power-board
netlists. Connector labels after U11 must not be interpreted as the native
HRTIM timer names on the MCU side of U11.

## 2026-08-28 STM32G474 control board

Source: `hardware/netlists/Netlist_474控制板_2026-08-28.net`.

| External net after U11 | MCU pin before U11 | Native HRTIM output |
|---|---|---|
| `CHA1` | PB12 | Timer C output 1 |
| `CHA2` | PB13 | Timer C output 2 |
| `CHB1` | PB14 | Timer D output 1 |
| `CHB2` | PB15 | Timer D output 2 |
| `CHC1` | PC6 | Timer F output 1 |
| `CHC2` | PC7 | Timer F output 2 |
| `CHD1` | PC8 | Timer E output 1 |
| `CHD2` | PC9 | Timer E output 2 |
| `CHE1` | PA8 | Timer A output 1 |
| `CHE2` | PA9 | Timer A output 2 |
| `CHF1` | PA10 | Timer B output 1 |
| `CHF2` | PA11 | Timer B output 2 |

The non-synchronous board therefore uses PA9/HRTIM1_CHA2 and
PA10/HRTIM1_CHB1. PC9/HRTIM1_CHE2 is routed to external `CHD2`, while
PC6/HRTIM1_CHF1 is routed to external `CHC1`; selecting PC9/PC6 from the
external `CHE2/CHF1` names would drive the wrong connector nets.

U11 bank enables are PB2=`PWM_OE1_N`, PC15=`PWM_OE2_N` and
PC14=`PWM_OE3_N`; each has a 10 kohm pull-up and is active low. PA2 joins the
cross-board `4_ADC1_3` net and PA5 joins `GPIO_1`. USART1 is PB6 TX/PB7 RX.

The U3 library fields conflict: `PARTTYPE`, symbol and device say
`STM32G474RET3`, while `Name` and `Manufacturer Part` say `STM32G474RBT3`.
The fitted device has been confirmed by the user as RBT3, so the build target
and flash algorithm remain RBTx/128 KiB; the EDA record should be corrected.


## ADC与时钟引脚复核

| 跨板网络 | MCU引脚/通道 | 当前用途 | 2026-08-28非同步板证据 |
|---|---|---|---|
| `2_ADC1_1` | PA0 / ADC1_IN1 | I1逐周期采样 | U9-18，经R49来自U2B调理输出 |
| `3_ADC1_2` | PA1 / ADC1_IN2 | I2逐周期采样 | U9-20，经R47来自U2A调理输出 |
| `0_ADC2_6` | PC0 / ADC2_IN6 | VPV逐周期采样 | U9-14，经R58来自U3B电压调理输出 |
| `1_ADC2_7` | PC1 / ADC2_IN7 | VBUS逐周期采样 | U9-16，经R67来自U3A电压调理输出 |
| `5_ADC1_4` | PA3 / ADC1_IN4 | NTC1慢采样 | U9-32，经R81来自U4A |
| `6_ADC2_3` | PA6 / ADC2_IN3 | NTC2慢采样 | U9-30，经R82来自U4B |
| `7_ADC2_4` | PA7 / ADC2_IN4 | 辅助/预留慢采样 | U9-28仅连接器端点 |
| `8_ADC2_5` | PC4 / ADC2_IN5 | 辅助/预留慢采样 | U9-26仅连接器端点 |
| `9_ADC3_1` | PB1 / ADC3_IN1 | 辅助/预留慢采样 | U9-24仅连接器端点 |

PA2虽带ADC1_IN3复用能力，但跨板网络`4_ADC1_3`连接两颗UCC27511的IN-，
当前必须作为共享高有效gate inhibit，不能配置成ADC输入。

X1连接PF0/OSC_IN与PF1/OSC_OUT，标称8 MHz、CL=10 pF、ESR=250 ohm，
C8=C9=12 pF。外接电容等效值是6 pF，只有加上约4 pF引脚/PCB寄生才达到
标称CL；这说明配置可行但不证明振荡裕量。PC14/PC15已用作OE3#/OE2#，不能
再作LSE；PA8是CHE1的U11输入，默认也不用于MCO。


## 2026-08-28 non-synchronous half-bridge small board

Source: `hardware/netlists/Netlist_非同步功率板_2026-08-28.net`.

The user identifies this attachment as the non-synchronous half-bridge small
board; its filename calls it the non-synchronous power board. This audit treats
those names as the same board. It is not the STM32G474 control-board netlist.

| Net | Connector | Board destination | Firmware consequence |
|---|---:|---|---|
| `GPIO_1` | U9-6 | MP9486 `DIM` and `EN` | PA5 high requests V15; no voltage readback exists |
| `CHE2` | U9-21 | U24/UCC27511 `IN+` through R19/R1 | One switching command input |
| `4_ADC1_3` | U9-22 | Both UCC27511 `IN-` paths through R2/R8 | This is a shared active-high gate inhibit, not an ADC input |
| `CHF1` | U9-23 | U1/UCC27511 `IN+` through R12/R7 | Second switching command input |
| `FLT_1..FLT_6` | U9-27/29/36/38/34/31 | Connector only | No board protection source is connected in this netlist |
| `EEV_1/EEV_2` | U9-35/U9-33 | Connector only | No board event source is connected in this netlist |

Joining U9 connector names to the control-board U6 connector proves the complete
path: PA9/Timer A2 -> U11 -> `CHE2` -> UCC27511 IN+, and PA10/Timer B1 -> U11
-> `CHF1` -> the second UCC27511 IN+. The shared `4_ADC1_3` path makes PA2 an
active-high inhibit for both drivers, not an ADC measurement input. `GPIO_1`
makes PA5 the MP9486 DIM/EN request. No V15/PGOOD feedback returns to the MCU.

## Evidence boundary

The supplied files now prove the control-board side, the complete U11
permutation and the non-synchronous daughterboard connector join. They still do
not prove an assembled-board continuity test, HRTIM kernel frequency, driver
polarity/timing, a real FLT/EEV source or V15 feedback. Those items remain
mandatory before any firmware may expose an output-enable path.
