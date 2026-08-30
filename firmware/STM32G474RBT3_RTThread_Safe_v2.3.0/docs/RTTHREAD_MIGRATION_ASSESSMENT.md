# RT-Thread migration assessment

> 2026-08-29 / v2.3.0更新：本工程已完成下述迁移架构的源码实现和主机
> 编译级/结构检查；v2.2.0有用户提供的ArmClang 6.24零错误/零警告证据，
> 但v2.3.0尚无ArmClang、下载或实板运行证据。当前优先级、
> 栈、ISR边界和验收步骤以根目录`remade_master.md`为准。

## Decision

Migration is technically feasible and the v2.3.0 source target now exists. It
must still pass ARMClang, flash, UART, tick/context-switch and long-duration
target regression before it can replace the target-proven bare v1.8.1 baseline.
The prior failures show defects in the hand-made framework; they do not show a
resource or architecture incompatibility between RT-Thread and STM32G474RBT3.

There is one independent hardware/download gate: the earlier successful image
ended at `0x0801583F`, while failed images first entered the 2 KiB page beginning
at `0x08016000`. Before treating any later RT-Thread result as a kernel result,
use STM32CubeProgrammer to read Option Bytes and explicitly program/verify that
page. A WRP/PCROP/DBANK or physical-page failure cannot be repaired by changing
an RTOS framework.

## Resource evidence

- STM32G474RBT3: 128 KiB Flash, 96 KiB ordinary SRAM plus 32 KiB CCM SRAM.
- Supplied v1.8.1 map: 16.51 KiB ROM and 1.80 KiB RW+ZI in ordinary SRAM.
- RT-Thread's official minimum for Nano is approximately 3 KiB ROM and 1.2 KiB
  RAM. Real use will be larger because every thread needs a stack and the CLI,
  IPC and libc options add code.

Capacity is therefore adequate. The old approximately 88–90 KiB RT-Thread image
was a framework/configuration result, not an unavoidable kernel footprint.

Official references:

- <https://www.rt-thread.io/>
- <https://www.rt-thread.io/document/site/programming-manual/basic/basic/>
- <https://www.st.com/en/microcontrollers-microprocessors/stm32g474rb.html>

## Correct ownership boundary

| Context | Responsibilities | Must not do |
|---|---|---|
| HRTIM/ADC/COMP ISR | PWM compare update, synchronous current sample, cycle-by-cycle trip, timestamped snapshot | Blocking IPC, formatted UART, MPPT search |
| Hardware FLT path | Asynchronous output shutdown | Wait for any CPU instruction or thread |
| High-priority supervisor thread | Consume fault snapshots, latch system state, command safe-off | Implement the primary over-current reaction |
| Control/MPPT thread | Slow voltage loop and MPPT perturbation, typically 1–100 ms rates | Touch OENR/OE#/DIS directly |
| CLI/telemetry thread | Parse commands and format decimated data | Own power-stage state |
| Storage thread | Persist calibrated data/fault records while stopped | Write Flash during critical control windows |

## Why the previous port failed

1. The original custom startup had only the 16 Cortex-M vectors. USART1 IRQ37,
   SysTick/PendSV/SVC integration and future HRTIM/ADC handlers did not have a
   trustworthy complete device startup base.
2. One build omitted `RT_USING_LIBC`, causing the ARMClang `va_list` cascade.
3. Later UART changes alternated between an unverified IRQ chain and polling,
   while the verified bare project was not kept as the BSP source of truth.
4. The framework mixed BSP, scheduler, safety policy and application code,
   making A/B diagnosis difficult.

## Recommended migration sequence

0. **Flash-page gate:** With the power stage disconnected, preserve a recovery
   image, inspect Option Bytes and independently program/verify `0x08016000`.
   Reflash the verified bare recovery image after this test.
1. **Baseline:** Rebuild bare v2.2.0, flash, press NRST and verify `PING`, `PRINT`,
   `UARTDIAG`, `PINMAP`, `HRTIMDIAG`, heartbeat and forced-fault shutdown.
2. **Kernel only:** Add a pinned RT-Thread Nano kernel and the official Cortex-M4
   MDK/ARMClang context port. Enable `RT_USING_LIBC`; create only idle and main.
   Keep UART in the same polling-stash implementation.
3. **Tick proof:** Verify SysTick increments and PendSV switches between two test
   threads for at least one hour while all power outputs remain physically blocked.
4. **BSP adapter:** Wrap existing `power_control_poll` into slow RT threads. Do
   not rewrite working ADC/UART/HRTIM-safe code during this step.
5. **UART upgrade:** Only after vector/context proof, change RX to IRQ or circular
   DMA and retain `UARTDIAG` counters as an A/B observable.
6. **Real-time control:** Introduce HRTIM/ADC ISR control independently of the
   RTOS tick. Pass fixed-size snapshots to threads through a lock-free double
   buffer or ISR-safe mailbox.

The RT-Thread BSP must expose both names for every PWM route: the external
connector label and the MCU-native HRTIM output. For the present
non-synchronous board, external CHE2/CHF1 are native Timer A output 2 on PA9
and Timer B output 1 on PA10. A BSP that selects Timer E/F from the connector
letters is electrically wrong even if it compiles and schedules correctly.

## Acceptance gates

- No missing/duplicate `SVC_Handler`, `PendSV_Handler` or `SysTick_Handler`.
- `USART1_IRQHandler`, ADC and HRTIM handlers resolve to strong symbols when used.
- No power-output register is writable from CLI or ordinary threads.
- Stack watermark measured for every thread; at least 30% worst-case headroom.
- Kernel-aware ISR priority policy documented and checked.
- Image stays below a project warning threshold of 96 KiB Flash, leaving 32 KiB
  for growth and field diagnostics.
- Bare-metal target remains buildable as a known-good recovery image.

The right next RTOS deliverable is therefore a second Keil target sharing this
BSP, not another independent framework archive.
