# MPPT 数字电源控制器

本仓库维护 STM32G474 + RT-Thread 的 MPPT 功率级控制工程。

## 当前基线

- MCU: STM32G474RBT3, 128-KiB Flash, 96-KiB main SRAM plus 32-KiB CCM SRAM.
- Firmware: `STM32G474RBT3_RTThread_Safe_v2.3.0`.
- RT-Thread is vendored under `firmware/STM32G474RBT3_RTThread_Safe_v2.3.0/third_party/rt-thread`.
- Current power-board profile uses external hardware protection. Firmware still
  keeps ADC software OVP/OCP/OTP, sampling watchdog, duty and bus-build limits,
  and starts with all power outputs disabled.

## 仓库目录

- `firmware/STM32G474RBT3_RTThread_Safe_v2.3.0/`: Keil project and complete
  source tree, including CMSIS, RT-Thread, board drivers, tests and build tools.
- `firmware/STM32G474RBT3_RTThread_Safe_v2.3.0/hardware/netlists/`: netlists
  associated with the maintained firmware baseline. The active pair is
  `Netlist_474控制板_2026-08-28.net` 和
  `Netlist_非同步功率板_2026-08-28.net`。旧版功率板和同步半桥网表已移除，避免误用。
- `docs/logs/remade_master.md`: single chronological engineering log. Every
  code, build, flash or bench-test change must be appended here.
- `docs/build-logs/`: raw Keil build and ST-LINK command-line logs.
- `docs/MAINTENANCE.md`: contribution and verification checklist.

## 构建与烧录

Run from the firmware directory in a PowerShell prompt:

```text
tools\check_project.bat
tools\build_keil.bat
tools\flash_keil.bat
```

The scripts use `D:\Keil\Keil\UV4\UV4.exe` when available and run without
opening the Keil GUI. Flashing only programs and resets the MCU; it does not
automatically enable the power stage.

## 串口命令

- `STATUS`, `CONTROL`, `HRTIMDIAG`: diagnostics.
- `MPPT AUTO` or `START 150`: qualified 150-V MPPT start.
- `START 100..400`: qualified start with an explicit bus target.
- `STOP` or `MPPT DISARM`: disable PWM, gate inhibit and auxiliary request.
- `HELP`: firmware command summary.

Before a high-energy test, use a current-limited low-voltage source, verify the
external protection chain and probe PWM with suitable isolation and differential
measurement. Do not treat a successful flash or a command reply as proof that
the power stage is safe to energize.
