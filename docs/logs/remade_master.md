# STM32G474RBT3 RT-Thread Safe — 唯一工程日志

本文件从 2026-08-29 起作为本工程唯一的新增工程日志。历史 `remade.md`、`remade_statusfix_20260829.md` 和版本发布说明保留为只读归档；后续构建、烧录、代码修改和实测记录只追加到本文件。

## 2026-08-29 — v2.3.0 启动逻辑、命令和日志规范修正

### 串口现象

串口日志显示 `MPPT ARM LIMITED 150` 返回 OK，但随后 `CONTROL mode=OFF duty=0`，状态回到 SAFE_OFF/READY，输出没有建立。执行前 ADC 采样有效，PV 约 75 V、母线约 74 V，故障输入为 0；这不是串口命令大小写或硬件 FLT 输入导致的拒绝。

### 根因

1. MPPT 外环在母线低于目标时错误使用 PV 电压误差。启动时 PV 参考被设为当前 PV 电压，误差约为零，控制器一直停在最小/零启动命令，最终触发母线建立超时或运行监督关断。
2. 原命令在模式切换后只报告“请求成功”，没有留下自动停机的最后故障位，导致瞬态 ADC/采样/运行监督故障在 SAFE_OFF 清理后不可追溯。
3. 帮助文本含中文 UTF-8 字面量，在非 UTF-8 串口终端中显示乱码，且启动/停止/校准操作需要输入过长命令。

### 本次代码修正

- 母线低于目标时先使用 `target_vbus - vbus` 建压；母线达到目标后再使用 PV 参考进行 MPPT 调节。
- 模式切换成功后预置最小启动占空比和外环积分器，保证第一控制周期有确定的非零启动命令。
- 新增 `last_stop_fault_bits`、`last_stop_fast_fault_flags`、`last_stop_duty_q15`、`last_stop_time_ms`，`CONTROL` 输出保留最近一次自动保护停机证据。
- 新增短命令：`START [100..400]`（默认 150 V，自动完成校准并启动 MPPT）、`STOP`/`HALT`（关断 PWM 和 AUX）、`CAL`/`CAL CURRENT`（全失能时自动电流零点校准）。原长命令继续兼容且大小写不敏感。
- `HELP` 改为纯 ASCII，避免串口代码页造成中文乱码，并明确每个命令用途。
- 自动校准失败统一记录错误码和参数，不再只递增计数器。

### 构建证据

- Keil ArmClang V6.24：`0 Error(s), 0 Warning(s)`。
- 镜像：`Code=34730 RO-data=8226 RW-data=4740 ZI-data=9868`。
- 静态工程/网表检查：37 个工程条目、118 个向量、128 KiB Flash、外部保护配置、12 路 PWM 路由和跨板网络检查通过。

### 烧录后现场验证

手动复位后先发送 `STATUS`，确认输出关闭；再发送 `START 150`。随后读取 `STATUS`、`CONTROL`、`HRTIMDIAG`，应至少看到 `mode=MPPT`、`cycle_sampling=1`、`duty>0`。如果再次回到 OFF，读取 `CONTROL` 中的 `last_stop_fault`、`last_stop_fast`、`last_stop_duty`，按故障位继续定位。首次功率验证仍必须使用低压、限流和预充，禁止直接接入高压母线。

### 触发源与日志编码

`board_fast_adc.c` 将 HRTIM ADC trigger 2 的 JEXTSEL 编码从裸数值改为 CMSIS 位定义组合，避免后续移植时把触发源写错；该编码与 ST 官方 STM32G4 LL ADC 定义一致：[stm32g4xx_ll_adc.h](https://github.com/STMicroelectronics/stm32g4xx-hal-driver/blob/master/Inc/stm32g4xx_ll_adc.h)。构建/烧录脚本使用 UTF-8 代码页追加日志，避免新的记录继续出现乱码。

### 日志策略

`tools/build_keil.bat` 与 `tools/flash_keil.bat` 已切换到本文件追加记录；`Out/keil_build.log`、`Out/keil_flash.log` 仍保留原始工具输出。

============================================================
[ARMCLANG] BUILD 閸涖劌鍙?2026/08/29 21:10:47.00 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
*** Using Compiler 'V6.24', folder: 'D:\Keil\Keil\ARM\ARMCLANG\Bin'
Rebuild target 'STM32G474_RTThread_Safe'
assembling startup_stm32g474xx.s...
compiling idle.c...
compiling clock.c...
compiling board_fault_handlers.c...
compiling irq.c...
compiling system_stm32g4xx.c...
compiling board_clock.c...
compiling board_safety.c...
compiling rt_board.c...
compiling board_uart.c...
compiling board_hrtim.c...
compiling ipc.c...
compiling board_fast_adc.c...
compiling board_cal_store.c...
assembling context_rvds.S...
compiling power_control.c...
compiling object.c...
compiling components.c...
compiling scheduler.c...
compiling thread.c...
compiling timer.c...
compiling cpuport.c...
compiling kservice.c...
compiling main.c...
linking...
Program Size: Code=34730 RO-data=8226 RW-data=4740 ZI-data=9868  
FromELF: creating hex file...
".\Out\STM32G474_RTThread_Safe.axf" - 0 Error(s), 0 Warning(s).
Build Time Elapsed:  00:00:01
============================================================
[FLASH] FLASH 閸涖劌鍙?2026/08/29 21:11:03.10 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
Load "E:\\涓汉瀛︿範\\閫嗗彉鍣╘\閫嗗彉鍣ㄦ帶鍒舵澘\\STM32G474RBT3_RTThread_Safe_v2.3.0_20260829\\STM32G474RBT3_RTThread_Safe_v2.3.0\\Out\\STM32G474_RTThread_Safe.axf" 
Erase Done.Programming Done.Verify OK.Application running ...
Flash Load finished at 21:11:02
============================================================
[ARMCLANG] BUILD 鍛ㄥ叚 2026/08/29 21:15:24.09 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
*** Using Compiler 'V6.24', folder: 'D:\Keil\Keil\ARM\ARMCLANG\Bin'
Rebuild target 'STM32G474_RTThread_Safe'
assembling startup_stm32g474xx.s...
compiling clock.c...
compiling idle.c...
compiling board_clock.c...
compiling board_fast_adc.c...
compiling system_stm32g4xx.c...
compiling board_fault_handlers.c...
compiling board_uart.c...
compiling board_hrtim.c...
compiling rt_board.c...
compiling board_safety.c...
compiling irq.c...
compiling ipc.c...
compiling board_cal_store.c...
assembling context_rvds.S...
compiling power_control.c...
compiling object.c...
compiling components.c...
compiling cpuport.c...
compiling main.c...
compiling scheduler.c...
compiling timer.c...
compiling thread.c...
compiling kservice.c...
linking...
Program Size: Code=34730 RO-data=8226 RW-data=4740 ZI-data=9868  
FromELF: creating hex file...
".\Out\STM32G474_RTThread_Safe.axf" - 0 Error(s), 0 Warning(s).
Build Time Elapsed:  00:00:01
============================================================
[FLASH] FLASH 鍛ㄥ叚 2026/08/29 21:15:37.82 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
Load "E:\\个人学习\\逆变器\\逆变器控制板\\STM32G474RBT3_RTThread_Safe_v2.3.0_20260829\\STM32G474RBT3_RTThread_Safe_v2.3.0\\Out\\STM32G474_RTThread_Safe.axf" 
Erase Done.Programming Done.Verify OK.Application running ...
Flash Load finished at 21:15:37
============================================================
[ARMCLANG] BUILD date=周六 2026/08 time=21:17:48.92 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[ARMCLANG] RAW_LOG=Out/keil_build.log
============================================================
[FLASH] FLASH date=周六 2026/08 time=21:18:02.25 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[FLASH] RAW_LOG=Out/keil_flash.log

## 2026-08-29 21:57 — 修复 HRTIM ADC2 更新源导致的无输出停机

### 最新现场证据

- 21:47–21:49 的串口日志显示常规 ADC 连续有效：`Vin≈75.2 V`、`Vo≈74.1 V`，
  `SAMPLES valid=0x000001FF faults=0`。
- `START 150`/`MPPT ARM LIMITED 150` 能返回 `OK`，说明命令解析、校准和启动前置条件
  已通过；但随后 `CONTROL` 始终为 `fast_seq=0 adc_irq=0`，并记录
  `fast_fault=0x00000010`、`last_stop_fast=0x00000010`，5 ms 快采看门狗按设计关闭输出。
- 因此本次故障不是 PV/母线常规采样错误，也不是电流零点在未输出时阻止启动；根因在
  PWM 同步注入采样触发链没有真正进入 ADC。

### 根因确认

`HRTIM1->sCommonRegs.ADC2R` 是 ADC trigger 2 的预装载寄存器，必须由 `CR1.ADC2USRC`
选择的定时器更新事件转入有效触发寄存器。旧代码只写了 `ADC2R=AD2TAC2`，没有设置
`ADC2USRC`，复位默认更新源为 Master；而采样启动只开启 Timer A (`TACEN`)，所以
有效触发仍未更新，ADC1/ADC2 都不会产生 `JEOS` 中断。

### 本次修复

- `Core/Src/board_hrtim.c` 增加 `HRTIM_CR1_ADC2USRC_0`，明确把 ADC2 trigger 2 的
  更新源设为 Timer A，并在 Timer A/B 软件更新后使用。
- `timing_contract_readback()` 和 `HRTIMDIAG` 的 `adc_trigger` 判定同时检查 `ADC2R`
  和 `CR1.ADC2USRC`，避免仅看到预装载值就误报触发链已就绪。
- 保留 `board_fast_adc.c` 的 CMSIS JEXTSEL 修复和外部触发不写 `JADSTART` 的流程。

### 构建/烧录证据

- Keil ArmClang V6.24：`0 Error(s), 0 Warning(s)`；镜像
  `Code=34758 RO-data=8226 RW-data=4740 ZI-data=9868`。
- 静态检查：`PASS: 37 project entries, 118 vectors, 128-KiB flash algorithm,
  external-protection arming profile, all 12 buffered PWM routes and cross-board nets consistent`。
- 21:57:36 烧录：`Erase Done. Programming Done. Verify OK. Application running`。

### 复位后的验收顺序

1. 手动按一次复位，先发送 `HRTIMDIAG`，确认 `adc_trigger=1`。
2. 发送 `START 150`（或 `MPPT ARM LIMITED 150`），等待 1–2 秒后发送 `CONTROL`。
3. 正常结果应为 `fast_seq`、`adc_irq` 持续递增，`incomplete` 不增加；随后
   `duty_q15` 应从软启动最小值开始上升，`outputs=1` 才表示 PWM 已释放。
4. 若仍为 `fast_seq=0`，请把 `HRTIMDIAG` 和新的 `CONTROL` 完整回传；这时可直接
   根据 `CR1/ADC2R/MCR` 读回值定位剩余寄存器或芯片时钟问题。未确认快采稳定前，
   仍不要提高功率级输入或母线电压。

### 后续日志格式

构建和烧录脚本不再把带本机代码页的 Keil 原始输出复制进主日志，只记录时间、版本、返回码和原始日志路径；这样主日志保持 UTF-8，完整原始输出仍在 `Out/keil_build.log` 和 `Out/keil_flash.log`。

### 静态检查维护

静态检查脚本已同步主日志文件名和 CMSIS 位定义形式的 HRTIM ADC trigger 2 检查；当前检查结果：`PASS: 37 project entries, 118 vectors, 128-KiB flash algorithm, external-protection arming profile, all 12 buffered PWM routes and cross-board nets consistent`。

README、实现状态和 RT-Thread 评估文档中的日志入口也已统一指向 `remade_master.md`。

## 2026-08-29 21:45 — v2.3.0 快速 ADC 无触发根因修复

### 现场证据

- 用户串口日志在执行 `START 150` 后仍显示 `Duty=0`，`CONTROL` 给出
  `fast_seq=0 adc_irq=0 incomplete=3 fast_fault=0x00000010`。
- 这说明不是 MPPT 外环计算把占空比算成了零，而是 HRTIM→ADC 注入触发没有
  产生任何 `JEOS` 中断；5 ms 快采看门狗随后安全停机。

### 根因

- `ADC_JSQR_JEXTSEL_0/1/4` 是 CMSIS 已经放置在 JSQR[6:2] 的位掩码，之前又执行
  `<< ADC_JSQR_JEXTSEL_Pos`，实际写入了错误的 JEXTSEL 值，因此 ADC 没有选择
  HRTIM ADC trigger 2。
- 该错误属于代码寄存器拼接错误，不是板上 Vin/Vo/电流采样值或外部保护造成的。

### 修复

- `Core/Src/board_fast_adc.c` 改为直接组合已移位的 CMSIS 位定义，不再二次左移；
  组合值对应 ST LL 的 `HRTIM_TRG2`（selector 19）。
- 移除外部触发模式下多余的 `JADSTART` 软件启动写入，按 ST
  `HAL_ADCEx_InjectedStart_IT` 的流程只使能 ADC 注入序列并等待下一次 HRTIM 边沿。
- 5 ms 快采停机看门狗和 `last_stop_*` 证据保留；如果触发链再次异常，固件仍会
  先关闭功率输出并记录故障，不会带故障运行。

### 构建/烧录证据

- Keil ArmClang V6.24：`0 Error(s), 0 Warning(s)`；镜像
  `Code=34710 RO-data=8226 RW-data=4740 ZI-data=9868`。
- 静态工程检查：`PASS: 37 project entries, 118 vectors, 128-KiB flash algorithm,
  external-protection arming profile, all 12 buffered PWM routes and cross-board nets consistent`。
- 21:45:04 烧录：`Erase Done. Programming Done. Verify OK. Application running`。

### 下一次验证

1. 用户手动复位后观察心跳，再发送 `STATUS` 和 `CONTROL`；无需先接高压。
2. 发送 `START 150` 后，`CONTROL` 应看到 `fast_seq` 持续递增、`adc_irq` 持续递增、
   `incomplete` 不再增加，且 `last_stop_fast` 不应出现 `0x10`。
3. 在确认快采稳定前仍禁止提高功率级输入和母线电压；本次烧录不会自动解除安全
   互锁或自动给功率级上电。
============================================================
[ARMCLANG] BUILD date=2026/08/29 time=21:44:08.92 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[ARMCLANG] RAW_LOG=Out/keil_build.log
============================================================
[FLASH] FLASH date=2026/08/29 time=21:45:04.28 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[FLASH] RAW_LOG=Out/keil_flash.log
============================================================
[ARMCLANG] BUILD date=2026/08/30 time=14:12:18 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[ARMCLANG] RAW_LOG=Out/keil_build.log
============================================================
[FLASH] FLASH date=2026/08/30 time=14:12:35 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[FLASH] RAW_LOG=Out/keil_flash.log
============================================================

### 2026-08-30 现场 START 150 失败复核 — 诊断快照修复

- 用户现场返回：`START 150` 已在校准有效（`cal=1/current=1/polarity=1/voltage=1`）
  时失败，错误为 `E_MPPT_ARM_LOCKED`，`faults=0x00000210`。其中 `0x10` 为
  `POWER_FAULT_HRTIM_LOCK`，`0x200` 为外部 15-V 反馈不可用提示；后者在外部保护
  已确认的配置下不应阻止 PWM，前者表示物理输出握手未完成。
- 发现诊断缺陷：`board_hrtim_power_arm()` 的 stage-1 前置门失败没有在
  `board_hrtim_force_off()` 前保存快照，导致现场显示 `stage=0,int_hi=0,int_lo=0,
  pad_hi=0,pad_lo=0`，无法知道是 HRTIM 后端、运行互锁还是外部 OE 读回失败。
- 本次修改：保存 stage-1 门控失败位；为启动后 OE/PA2/PA5/PA9/PA10 读回失败增加
  stage-6；串口 `POWER_ARM_READBACK` 与 `HRTIMDIAG` 增加 `gate=` 字段。门控仍保持
  失能优先，不因诊断而放宽保护。
  `gate` 位定义：stage-1 bit0=编译开关、bit1=运行互锁、bit2=HRTIM 后端；
  stage-6 bit0=故障输入，bit1..7 依次为 PA5 请求、OE3 ODR、PA2 ODR、PA5 IDR、
  OE3 IDR、PA2 IDR、HRTIM 输出证据。
- 继续复核发现，`board_safety_request_power_on()` 在调用 HRTIM 前的早期保护门失败也
  会返回 `stage=0`；已补充 stage-1 快照。早期 gate 位 bit8=编译开关、bit9=保护后端、
  bit10=故障输入，低位仍保留原始故障输入掩码。
- Keil ArmClang V6.24 构建通过：`0 Error(s), 0 Warning(s)`；
  `Code=36666 RO-data=8598 RW-data=6788 ZI-data=9972`。
- 待烧录镜像需手动复位后读取 `HRTIMDIAG`，再在低压限流条件下只发送一次
  `START 150`；禁止连续重启功率级。根据返回的 `stage/gate/backend/timing/dll/clock`
  分支处理。
============================================================
[ARMCLANG] BUILD date=2026/08/30 time=14:00:45 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[ARMCLANG] RAW_LOG=Out/keil_build.log
============================================================
[FLASH] FLASH date=2026/08/30 time=14:01:25 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[FLASH] RAW_LOG=Out/keil_flash.log
============================================================

## 2026-08-30 13:06 — START 复位与复位后红灯根因修复

### 现场证据

- `START 150` 后只收到 `ER`，约 2.1 s 后再次输出 RT-Thread 启动横幅；
  随后的 `STATUS` 为 `reset=0x24000000`。其中 `RCC_CSR_IWDGRSTF=0x20000000`
  表明看门狗复位，另有 `PINRSTF=0x04000000` 的复位输入记录。
- Keil 静态调用链显示 `main_thread_entry -> main -> command ->
  command_mppt_arm` 的最大栈深约 2272 B，而原 `RT_MAIN_THREAD_STACK_SIZE=2048` B。
  这会在 START 的校准/读回失败路径发生栈越界，进入 RT-Thread HardFault 处理，
  看门狗随后复位；因此不是串口大小写或 COM5 通信问题。
- 复位后 `STATUS` 的逻辑颜色为 `RGB 0,4,12`（安全关闭应为青蓝），但
  `ws_show()` 因快采样已经运行而主动跳过刷新，LED 保留上一次硬件状态，表现为一直红色。

### 本次修改

- `rtconfig.h`：RT-Thread 主线程栈从 2048 B 增加到 4096 B，仍放置在 32-KiB CCM
  SRAM；fault_guard/telemetry 栈保持 512/384 B。该余量覆盖当前 ArmClang 报告的
  2272-B 最大调用深度，避免 START 路径栈溢出。
- `main.c`：在 `power_control_init()` 启动 100-kHz 注入采样之前刷新 BOOT 和
  SAFE_OFF 颜色；这样不会屏蔽 ADC 中断，也不会让 WS2812 保留复位前的红色。
- `power_control.c`、`board_fast_adc.c`、`board_hrtim.c`：所有 ADC/HRTIM 有界等待
  增加周期性看门狗喂狗，外设不响应时仍返回错误而不是无提示复位。
- `tests/check_project.py`、README 和实现状态记录同步到 4096-B 主线程栈。

### 构建与烧录

- 静态检查：PASS（37 entries，118 vectors，128-KiB flash，外部保护配置）。
- Keil ArmClang V6.24：`0 Error(s), 0 Warning(s)`；
  `Code=36394 RO-data=8578 RW-data=6788 ZI-data=9932`。
- ST-LINK：`Erase Done. Programming Done. Verify OK. Application running`，
  完成时间 `2026-08-30 13:06:13`。
- 本次没有打开或占用 COM5，也没有自动发送 START；需要你复位后再发送一次
  `START 150`，应至少收到完整 `OK ...` 或完整 `ERR ...`，不应再出现只回 `ER` 后复位。

## 2026-08-30 12:47 — 修复 START 校准路径被 IWDG 复位

### 现场证据

- 最新串口记录中，每次发送 `START 150` 后约 2.1 秒重新输出 RT-Thread 启动横幅，
  没有任何 `OK/ERR` 回复；与本固件约 2 秒 IWDG 周期一致。
- 这表明启动在自动电流零点校准的 64 轮 ADC 扫描中阻塞，并非功率级故障。

### 修复

- 新增 `power_control_watchdog_kick()` 回调；主程序用 IWDG 喂狗实现，控制模块保留 weak 空实现以保持分层。
- 自动零点校准每轮扫描前喂狗；单次 ADC 转换仍保持有界超时，不会取消故障保护。
- 这使 `START` 能完成校准并返回真实的 `OK` 或分级 `ERR`；不再以无回显的复位代替故障报告。

### 构建与烧录

- 静态检查通过；Keil ArmClang `0 Error(s), 0 Warning(s)`，镜像
  `Code=36394 RO-data=8578 RW-data=4740 ZI-data=9932`。
- ST-LINK：`Erase Done. Programming Done. Verify OK. Application running`，完成时间 `12:47:36`。
- 本次未发送启动命令，未占用 COM5。

## 2026-08-30 12:42 — 区分“寄存器已请求”与“PA9/PA10 真实 PWM 边沿”

### 现场结论

- 用户单次触发示波器没有捕获到任何 PWM。因此旧版
  `outputs=1` 只能证明软件走到 HRTIM 写寄存器路径，不能作为 MCU
  引脚实际产生 PWM 的证据。
- 12:23 记录中随后出现的 `reason=0x00000002` 是另一个独立的
  `ADC_TIMEOUT` 软件关断原因，但它不能解释“连第一个边沿都没有”。

### 本次修改

- HRTIM Timer A2/B1 的 `OUTxR/DTxR/CHPxR/RSTxR` 改为显式初始化：高有效、
  空闲低电平、无斩波、无死区、无延迟保护，不再依赖复位默认值。
- 启动顺序改为：全输出禁止 → PA9/PA10 AF13 → A2/B1 `OENR` →
  Timer A/B 计数器 → 边沿自检 → 释放 OE3/PA2。该顺序与 ST HRTIM
  waveform 的“先启用输出、再启动计数器”路径一致。
- 新增启动前双重 PWM 边沿自检：
  1. 轮询 HRTIM `O2STAT/O1STAT`，必须同时观察到 A2/B1 高、低两种状态；
  2. 轮询 GPIOA `IDR`，必须同时观察到 PA9/PA10 实际引脚高、低翻转。
  自检时 OE3 仍为高、PA2 inhibit 仍为高，所以不会把测试 PWM
  送入功率级。
- 任一环节失败均立即全关断，`START` 直接返回
  `POWER_ARM_READBACK(stage=...)`：`2=AF/OENR`、`3=计数器`、
  `4=HRTIM内部无边沿`、`5=PA9/PA10引脚无边沿`。
- `HRTIMDIAG` 新增 `HRTIMARM` 行，记录尝试次数、失败阶段、OENR
  读回、内部高/低观测和 PA9/PA10 高/低观测，不再需要通过
  多轮猜测判断 HRTIM 哪一级失败。
- 功率级释放后再读 MCU 引脚实际电平：PA5 必须为高、PC14/OE3
  必须为低、PA2/inhibit 必须为低；任一引脚读回不符都不会返回
  `OK START`。
- 同时修复 12:23 记录的 ADC 冲突：NTC1/NTC2 并入 100 kHz 注入序列，
  快采期间不再用普通轮询转换抢占 ADC1/ADC2，未使用辅助通道超时
  不再误杀功率级。

### 构建与烧录证据

- 静态检查：`PASS: 37 project entries, 118 vectors, 128-KiB flash algorithm,
  external-protection arming profile, all 12 buffered PWM routes and cross-board nets consistent`。
- Keil ArmClang V6.24：`0 Error(s), 0 Warning(s)`；镜像
  `Code=36370 RO-data=8578 RW-data=4740 ZI-data=9932`。
- ST-LINK 后台烧录：`Erase Done. Programming Done. Verify OK. Application running`；
  最终完成时间 `2026-08-30 12:42:30`。
- 本次未打开/占用 COM5，未自动发送 `START`。烧录成功只证明固件写入和运行，
  PA9/PA10 物理 PWM 结果必须以下一次 `START` 返回的边沿自检数据和
  示波器实测共同确认。
[ARMCLANG] BUILD date=2026/08/29 time=21:57:11.13 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[ARMCLANG] RAW_LOG=Out/keil_build.log
============================================================
[FLASH] FLASH date=2026/08/29 time=21:57:36.98 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[FLASH] RAW_LOG=Out/keil_flash.log
============================================================
[ARMCLANG] BUILD date=2026/08/30 time= 0:10:27.81 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[ARMCLANG] RAW_LOG=Out/keil_build.log
============================================================
[FLASH] FLASH date=2026/08/30 time= 0:10:46.74 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[FLASH] RAW_LOG=Out/keil_flash.log

## 2026-08-30 00:10 — 修复 PWM 启动约 2.5 ms 后被软件关闭

### 现场证据与根因

- 示波器确认 PWM 能短暂输出约 2.5 ms，随后被关闭；串口同时记录
  `fast_seq=0`、`adc_irq=0`、`fast_fault=0x00000010`、
  `last_stop_fast=0x00000010`。这两组证据指向同一条软件关断路径：
  HRTIM 已启动，但 ADC1/ADC2 注入组没有产生首个 `JEOS`，5 ms 快采进度看门狗
  调用 `board_fast_adc_trip_sequence_timeout()`，继而关闭 HRTIM、OE3 和 PA2 inhibit。
- 根因是 `Core/Src/board_fast_adc.c` 没有给 ADC1/ADC2 写 `ADC_CR_JADSTART`。
  对 STM32G4，选择外部触发后仍必须写 `JADSTART` 来武装注入组；此时它不会立即
  软件转换，而是等待下一次 HRTIM_TRG2 边沿。ST 官方
  `HAL_ADCEx_InjectedStart_IT()` 同样调用 `LL_ADC_INJ_StartConversion()`。
- 因此 2026-08-29 21:45 日志中“外部触发不应写 JADSTART”的结论错误；本条保留
  旧记录并明确更正，不删除历史证据。

### 本次修改

- ADC2、ADC1 在 HRTIM Timer A 启动前依次置 `JADSTART`，并读回两路武装状态。
- 每次启动前先停止旧 HRTIM 采样并使用 `JADSTP` 清理残留注入状态，防止重启沿用
  上一轮等待状态。
- 新增首周期握手：只有 ADC1 与 ADC2 都完成至少一个共同 HRTIM 触发周期后，
  `board_fast_adc_start()` 才返回成功；握手期间 PA9/PA10 仍为高阻且 OE3/PA2 保持锁定。
  首周期失败会在释放功率输出之前返回 `FAST_ADC_FIRST_CYCLE`，不再先输出数毫秒再停机。
- 成功重启采样后同步刷新 5 ms 看门狗的序列基准和时间基准，避免旧时间戳造成误停。
- `HRTIMDIAG` 增加 `FASTADC` 行：`sequence/irq/starts/start_fail/armed/last_adc1_cr/
  last_adc2_cr/faults/incomplete`，一次命令即可判断是 ADC 武装、HRTIM 触发还是 ISR 合成失败。

### 构建与烧录证据

- 静态检查：`PASS: 37 project entries, 118 vectors, 128-KiB flash algorithm,
  external-protection arming profile, all 12 buffered PWM routes and cross-board nets consistent`。
- Keil ArmClang V6.24：`0 Error(s), 0 Warning(s)`；镜像
  `Code=35206 RO-data=8302 RW-data=4740 ZI-data=9892`。
- ST-LINK：`Erase Done. Programming Done. Verify OK. Application running`，完成时间
  `2026-08-30 00:10:46`。
- 本次未打开或占用 COM5，未发送 `START`/`ARM`，烧录后的固件仍从安全关闭状态启动。

### 低压限流验收标准

1. 手动复位后只发送一次 `HRTIMDIAG`。正常应看到 `FASTADC sequence`、`irq` 已递增，
   `starts>=1`、`start_fail=0`、`faults=0`；运行时 `armed=0x00000003`。
2. 低压、限流、可急停条件下发送 `START 150`。成功响应后再发一次 `HRTIMDIAG`，
   `sequence/irq` 必须继续递增，`power_outputs=1`，不得再次出现 `fast_fault=0x10`。
3. 若首周期仍失败，命令会在 PWM 释放前返回 `FAST_ADC_FIRST_CYCLE`；此时只需回传一条
   `HRTIMDIAG`，不得继续提高输入电压或反复启动。

## 2026-08-30 12:21 — 按真实上电顺序修复 START 假成功与启动后静默关断

### 用户确认的上电顺序

1. 只给 MCU/控制板上电；此时 `Vin/Vo=0 V` 是正常等待状态，不得触发 PV 欠压、
   电流卡死或母线建立超时。
2. 开启外置数字电源，使 MCU 采到约 `Vin=75 V`、`Vo=74 V`；功率 PWM 仍保持关闭。
3. 串口只发送一次 `START 150`，固件在全失能状态完成电流零点校准，然后请求 PA5、
   释放 OE3/PA2 并开启 PA9/PA10 HRTIM PWM。

### 现场证据与根因

- 12:05/12:10 串口记录中的 `FASTADC` 已正常：`sequence/irq` 持续递增、
  `start_fail=0`、`armed=0x00000003`、`faults=0`，所以 00:10 版的 ADC 注入触发修复
  已生效；本次失败不是 ADC1/ADC2 没有触发。
- `START` 返回 OK 后，后续 `CONTROL mode=OFF`、`gate=LOCKED`、`pa5_odr=0`，但应用层
  仍显示 `READY/Fault=0`。根因是运行监督把低占空比软启动期间接近零的电流在约
  100 ms 后误判为 `CURRENT_STUCK`；控制层先关断，随后的 SAFE_OFF 扫描又清掉运行位，
  主状态机没有可靠收到关断原因。
- 旧命令在 HRTIM/OE/PA2/PA5 尚未完成物理寄存器读回前就打印 `OK START`，属于启动
  握手假成功；重复发送 START 还会先执行一次 OFF，反复重置软启动。

### 本次修改

- 0 V 等待阶段维持 SAFE_OFF：PV 欠压、零电流卡死、母线建立超时只在运行请求和
  物理 PWM 输出确认后参与运行监督。
- `CURRENT_STUCK` 增加四重门控：`outputs_enabled=1`、启动空白 500 ms、实际占空比
  不低于 10%、母线仍低于目标 90%；再连续 250 ms 零电流才停机。2.5 A 快速软件 OCP、
  165 V PV OVP、动态母线 OVP和100 kHz快采保护保持不变。
- Boost 不可达由单样本立即关断改为连续 20 ms 确认，避免外置电源建立瞬间的 ADC
  过渡样本导致静默关断；`Vin=75 V, target=150 V` 满足至少 5 V 升压裕量。
- `power_control_set_mode()` 在返回前立即执行首个外环/快速控制步骤，并确认 HRTIM A/B
  计数器、PA9/PA10 AF13、PWM 输出标志、OE3=0、PA2 inhibit=0、PA5=1。任一读回失败
  返回 `ERR ... reason=POWER_ARM_READBACK`，不再报告 OK。
- 成功响应改为包含 `outputs=1 duty_q15=... aux15v=1 gate=ARMED`。相同目标的重复
  START 变为幂等响应 `already=RUNNING`，不再重启软启动。
- 控制层保存的 `last_stop_fault` 被纳入应用层锁存；任何运行期自动停机都会进入
  `FAULT` 并写故障日志，不能再出现 `mode=OFF` 但 `READY/Fault=0`。`FAULT CLEAR`
  成功后才清除该持久快照。

### 构建与烧录证据

- 静态检查：`PASS: 37 project entries, 118 vectors, 128-KiB flash algorithm,
  external-protection arming profile, all 12 buffered PWM routes and cross-board nets consistent`。
- Keil ArmClang V6.24：`0 Error(s), 0 Warning(s)`；镜像
  `Code=35574 RO-data=8394 RW-data=4740 ZI-data=9892`。
- ST-LINK：`Erase Done. Programming Done. Verify OK. Application running`；完成时间
  `2026-08-30 12:21:29`。
- 本次未打开/占用 COM5，未自动发送 `START`，因此只确认了构建、烧录和静态状态机，
  尚未替用户开启功率级。

### 下一次低压限流验证

外置电源稳定在约 75 V 后只发送一次：`START 150`。只有收到
`OK START ... outputs=1 ... aux15v=1 gate=ARMED` 才代表软件已释放功率级；若失败，
不要连续重发，直接回传该错误行以及一条 `CONTROL`，其中 `last_stop_fault` 已能保留
真实关断原因。
============================================================
[ARMCLANG] BUILD date=2026/08/30 time=12:20:50.37 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[ARMCLANG] RAW_LOG=Out/keil_build.log
============================================================
[FLASH] FLASH date=2026/08/30 time=12:21:29.95 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[FLASH] RAW_LOG=Out/keil_flash.log
============================================================
[ARMCLANG] BUILD date=2026/08/30 time=12:29:08.88 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[ARMCLANG] RAW_LOG=Out/keil_build.log
============================================================
[FLASH] FLASH date=2026/08/30 time=12:29:26.16 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[FLASH] RAW_LOG=Out/keil_flash.log
============================================================
[ARMCLANG] BUILD date=2026/08/30 time=12:35:24.39 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[ARMCLANG] RAW_LOG=Out/keil_build.log
============================================================
[ARMCLANG] BUILD date=2026/08/30 time=12:36:41.45 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[ARMCLANG] RAW_LOG=Out/keil_build.log
============================================================
[FLASH] FLASH date=2026/08/30 time=12:36:57.82 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[FLASH] RAW_LOG=Out/keil_flash.log
============================================================
[ARMCLANG] BUILD date=2026/08/30 time=12:42:23.62 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[ARMCLANG] RAW_LOG=Out/keil_build.log
============================================================
[FLASH] FLASH date=2026/08/30 time=12:42:30.16 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[FLASH] RAW_LOG=Out/keil_flash.log
============================================================
[ARMCLANG] BUILD date=2026/08/30 time=12:47:30.24 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[ARMCLANG] RAW_LOG=Out/keil_build.log
============================================================
[FLASH] FLASH date=2026/08/30 time=12:47:36.81 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[FLASH] RAW_LOG=Out/keil_flash.log
============================================================
[ARMCLANG] BUILD date=2026/08/30 time=13:03:32.53 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[ARMCLANG] RAW_LOG=Out/keil_build.log
============================================================
[ARMCLANG] BUILD date=2026/08/30 time=13:05:54.80 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[ARMCLANG] RAW_LOG=Out/keil_build.log
============================================================
[FLASH] FLASH date=2026/08/30 time=13:06:14.09 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[FLASH] RAW_LOG=Out/keil_flash.log

### 2026-08-30 继续复核 — 等待现场复位/串口确认

- 已重新执行 `tests/check_project.py`，结果为 PASS：37 个工程文件、118 个向量、
  128-KiB Flash 算法、12 路缓冲 PWM 和跨板网络一致。
- 已复核现场历史记录：旧镜像在 `START 150` 后返回过短的 `ER` 并产生
  `reset=0x24000000`（IWDGRSTF）；另一次虽然返回 `OK START`，但随后
  `fast_seq=0/adc_irq=0/fast_fault=0x10`，说明启动后的 ADC 同步采样没有运行，
  不能把该旧响应当作功率级已开启。
- 13:06 镜像已完成 Keil 构建和 ST-LINK Verify OK；本轮未打开 COM5、未发送启动命令。
- 下一步必须在手动复位后读取一次 `STATUS`、`HRTIMDIAG`，再在外置 75-V 数字电源稳定时
  只发送一次 `START 150`。重点核对 `OK START ... outputs=1`、`fast_seq` 递增、
  `duty_q15` 非零和 `gate=ARMED`；若失败，保留完整 `ERR` 行和 `last_stop_fast`，
  不重复启动。
============================================================

### 2026-08-30 现场串口诊断 — COM5（用户已授权）

- 手动复位后读取：`STATUS state=SAFE_OFF latch=0 reason=0 raw=0 faults=0`；
  `HRTIMDIAG` 显示 `backend_ready=1 timing=1 sampling=1 power_outputs=0`。
- `FASTADC sequence=11969199 irq=11969199 starts=1 start_fail=0 armed=0x00000003 faults=0`
  且序列持续递增，确认 100-kHz 注入采样和 ADC1/ADC2 中断链路正常。
- 首次发送 `RGB 100,0,0` 等命令格式错误，固件正确协议是直接发送 `100,0,0`；
  改用 `100,0,0`、`0,100,0`、`0,0,100` 均返回 `ACK RGB ...`，最终 `RGB 0,0,100`。
  该过程产生 4 条预期的未知命令记录（`last_err=0x3001`），不影响功率保护。
- 本轮未发送 `START`/`AUX ON`，未释放功率级；需要用户根据三组命令观察实际 LED
  颜色，以确认 WS2812 的物理色序/数据线。串口脚本已关闭，COM5 已释放。
============================================================
============================================================
[ARMCLANG] BUILD date=2026/08/30 time=13:43:02.59 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[ARMCLANG] RAW_LOG=Out/keil_build.log
============================================================
[FLASH] FLASH date=2026/08/30 time=13:44:00.11 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[FLASH] RAW_LOG=Out/keil_flash.log
============================================================
[ARMCLANG] BUILD date=2026/08/30 time=14:00:16.68 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[ARMCLANG] RAW_LOG=Out/keil_build.log
============================================================
[FLASH] FLASH date=2026/08/30 time=14:01:25.49 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[FLASH] RAW_LOG=Out/keil_flash.log
============================================================
[ARMCLANG] BUILD date=2026/08/30 time=14:06:35.56 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[ARMCLANG] RAW_LOG=Out/keil_build.log
============================================================
[FLASH] FLASH date=2026/08/30 time=14:06:52.38 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[FLASH] RAW_LOG=Out/keil_flash.log
============================================================
[ARMCLANG] BUILD date=2026/08/30 time=14:12:10.11 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[ARMCLANG] RAW_LOG=Out/keil_build.log
============================================================
[FLASH] FLASH date=2026/08/30 time=14:12:35.76 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[FLASH] RAW_LOG=Out/keil_flash.log
============================================================
[ARMCLANG] BUILD date=2026/08/30 time=14:35:15.59 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[ARMCLANG] RAW_LOG=Out/keil_build.log

### 2026-08-30 代码直改 — 修复 START 被软件自锁及外部三态门误判

- 本次只修改源码，没有重新读取或改动任何网表文件。
- 根因一：`power_control_set_mode()` 在重新启动 PWM 同步 ADC 之前，把历史
  `POWER_FAULT_HRTIM_LOCK` 当成不可恢复故障检查。一次中止或旧启动失败后，
  后续 `START` 会在真正重试 HRTIM 之前直接返回 `E_MPPT_ARM_LOCKED`。
  现在初始门控只排除这一可恢复状态位，随后由 `resume_fast_sampling()` 的
  ADC 首周期握手重新判定；握手失败仍返回 `-6` 并保持安全关闭。
- 根因二：外部保护模式下，`board_hrtim_power_arm()` 在 OE3/PA2 仍锁定时
  读取 PA9/PA10 引脚边沿。三态门未释放时引脚被隔离，`pad_hi/pad_lo=0`
  不是 HRTIM 没有 PWM 的证据，却会触发 stage=5。外部保护模式现在只要求
  HRTIM 内部 A2/B1 两路高低边沿；PA9/PA10 的引脚读回仍保留在诊断中，
  MCU-FLT 可读模式继续强制执行 pad 自检。
- `MPPT AUTO` 已改为调用统一的 `START 150` 合格启动流程，删除原来遇到
  不存在的 MCU 15-V PGOOD 就必然失败的硬编码分支。外部保护确认模式仍由
  软件 ADC OVP/OCP/OTP、采样看门狗、占空比/母线构建监督和外部锁存共同保护。
- 静态检查：`PASS: 37 project entries, 118 vectors, 128-KiB flash algorithm,
  external-protection arming profile, all 12 buffered PWM routes and cross-board nets consistent`。
- Keil ArmClang V6.24：`0 Error(s), 0 Warning(s)`；镜像
  `Code=36634 RO-data=8494 RW-data=6788 ZI-data=9972`。
- 构建后已按既定流程后台烧录，未占用 COM5，未发送 `START`；ST-LINK 返回
  `Erase Done / Programming Done / Verify OK / Application running`（14:37:32）。
  烧录后应手动复位，
  依次发送一次 `HRTIMDIAG` 和一次 `START 150`，以完整返回的 `stage/gate/int_hi/int_lo`
  判断现场启动结果。
============================================================
[FLASH] FLASH date=2026/08/30 time=14:37:32.56 version=G474-RBT3-RTT-SAFE-2.3.0 rc=0
[FLASH] RAW_LOG=Out/keil_flash.log
