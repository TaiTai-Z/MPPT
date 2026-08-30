# Vendor source manifest

These files are pinned so the project does not silently change when an online
repository changes.

| Component | Upstream revision | Build status |
|---|---|---|
| ST CMSIS Device G4 v1.2.6 | `626ee412334a5ed2e5b320af5a8d77d69f03a558` | Device headers, startup and system source are built |
| Arm CMSIS-Core | tag `5.9.0` | Core headers are built |
| ST STM32G4 HAL driver | `a6001282dfacfff57e9710250f15e4333b578865` | HRTIM source/header are reference-only and not in the Keil target |

One deliberate board-local configuration exists in the vendored startup file:
`Stack_Size` is raised from ST's template default of 1 KiB to 2 KiB. The supplied
v1.8.1 call graph already reports 736 bytes of known maximum stack use, before
the new diagnostics were added. Vector contents and reset logic are unchanged.

Official upstream locations:

- <https://github.com/STMicroelectronics/cmsis-device-g4>
- <https://github.com/ARM-software/CMSIS_5>
- <https://github.com/STMicroelectronics/stm32g4xx-hal-driver>

The HRTIM HAL implementation is intentionally not compiled. Its translation
unit references the HAL time base, DMA, GPIO, RCC and NVIC services; adding only
`stm32g4xx_hal_hrtim.c/.h` to a register-level project is not a valid minimal
HAL integration. The active board code uses ST CMSIS register definitions in
`Core/Src/board_hrtim.c`.

Licenses are stored beside the corresponding source trees.
