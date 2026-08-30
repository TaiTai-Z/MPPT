# STM32G474RBT3 RT-Thread Safe v2.0.0 Log

Firmware source: `G474-RBT3-RTT-SAFE-2.0.0`

## Static release checks

- RT-Thread Nano 4.1.1 with `RT_USING_LIBC` and static user-main thread.
- Official STM32G474 CMSIS startup and complete 118-entry vector table.
- STM32G474RBTx, 128 KiB Flash algorithm, 96 KiB ordinary SRAM plus explicit
  32 KiB CCM region.
- RT main/idle stacks and CPU-only event log in CCM.
- HRTIM stopped-register readback is separated from clock validation; the
  unmeasured 100 kHz candidate cannot make `backend_ready` true.
- Shared board/power sources and selected RT kernel C sources pass strict host
  syntax analysis; both project XML files parse and every project path exists.
- `tests/check_project.py` passes all control-board and non-synchronous-board
  route checks.

No ArmClang, ST-LINK or target-board result is claimed here. The scripts append
real Keil build/download logs below when executed on the user's PC.
