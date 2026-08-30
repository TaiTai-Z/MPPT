#!/usr/bin/env python3
"""Structural release checks that do not require Keil or target hardware."""

from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / "STM32G474_RTThread.uvprojx"
OPTIONS = ROOT / "STM32G474_RTThread.uvoptx"
SCATTER = ROOT / "STM32G474_RTThread.sct"
EXPECTED_VERSION = "G474-RBT3-RTT-SAFE-2.3.0"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def project_paths(root: ET.Element) -> list[Path]:
    paths: list[Path] = []
    for node in root.findall(".//FilePath"):
        text = (node.text or "").strip().replace("\\", "/")
        if text.startswith("./"):
            text = text[2:]
        paths.append(ROOT / text)
    return paths


def parse_nets(text: str) -> dict[str, list[str]]:
    nets: dict[str, list[str]] = {}
    pattern = re.compile(r"^\(\r?\n([^\r\n]+)\r?\n(.*?)^\)\r?$",
                         re.MULTILINE | re.DOTALL)
    for match in pattern.finditer(text):
        name = match.group(1).strip()
        members = [line.strip() for line in match.group(2).splitlines()
                   if line.strip()]
        nets[name] = members
    return nets


def parse_values(text: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for match in re.finditer(r"^\[\r?\n(.*?)^\]\r?$", text,
                             re.MULTILINE | re.DOTALL):
        lines = [line.strip() for line in match.group(1).splitlines()]
        fields = {lines[index]: lines[index + 1]
                  for index in range(len(lines) - 1)
                  if lines[index] in {"DESIGNATOR", "Value"}}
        if "DESIGNATOR" in fields and "Value" in fields:
            values[fields["DESIGNATOR"]] = fields["Value"]
    return values


def require_members(nets: dict[str, list[str]], net_name: str,
                    tokens: tuple[str, ...], context: str) -> None:
    body = "\n".join(nets.get(net_name, []))
    require(all(token in body for token in tokens),
            f"{context}: {net_name}")


def main() -> int:
    xml_root = ET.parse(PROJECT).getroot()
    paths = project_paths(xml_root)
    missing = [str(path.relative_to(ROOT)) for path in paths if not path.exists()]
    require(not missing, f"missing project files: {missing}")

    project_text = PROJECT.read_text(encoding="utf-8")
    require("<Define>STM32G474xx;APP_USE_RTTHREAD;HSE_VALUE=8000000</Define>" in project_text,
            "RT-Thread compiler defines missing")
    require("<Device>STM32G474RBTx</Device>" in project_text,
            "wrong Keil device selection")
    require("IROM(0x08000000,0x00020000)" in project_text,
            "project IROM is not the RBT3 128 KiB range")
    require("IRAM(0x20000000,0x00018000)" in project_text,
            "project IRAM is not the 96 KiB SRAM range")
    require("<Size>0x80000</Size>" not in project_text,
            "stale 512-KiB ROM metadata remains in the RBT3 project")
    require("startup_stm32g474xx.s" in project_text,
            "official G474 startup missing")
    require("startup_stm32g474.c" not in project_text,
            "incomplete legacy startup still selected")
    require("stm32g4xx_hal_hrtim.c" not in project_text,
            "reference-only HRTIM HAL was accidentally added to the build")

    options_text = OPTIONS.read_text(encoding="utf-8")
    require("STM32G47x-8x_128.FLM" in options_text,
            "128 KiB STM32G47x-8x flash algorithm missing")
    require("-FS08000000 -FL020000" in options_text,
            "flash algorithm range is not 0x08000000/128 KiB")
    scatter_text = SCATTER.read_text(encoding="utf-8")
    require("LR_IROM1 0x08000000 0x00020000" in scatter_text,
            "scatter load region is not 128 KiB")
    require("RW_IRAM1 0x20000000 0x00018000" in scatter_text,
            "scatter SRAM region is not 96 KiB")
    require("RW_CCMRAM 0x10000000 0x00008000" in scatter_text and
            "*(.ccmram)" in scatter_text,
            "separate 32 KiB CCM region is missing")

    startup = (ROOT / "Drivers/CMSIS/Device/ST/STM32G4xx/Source/Templates/"
                      "startup_stm32g474xx.s").read_text(encoding="utf-8")
    vector_match = re.search(r"^__Vectors\s+DCD.*?(?=^__Vectors_End\b)",
                             startup, re.MULTILINE | re.DOTALL)
    require(vector_match is not None, "vector table body not found")
    vector_body = vector_match.group(0)
    vector_count = len(re.findall(r"\bDCD\s+", vector_body))
    require(vector_count == 118, f"expected 118 vectors, found {vector_count}")
    require(re.search(r"^Stack_Size\s+EQU\s+0x800;", startup,
                      re.MULTILINE) is not None,
            "startup main stack must be 2 KiB")
    for handler in ("SVC_Handler", "PendSV_Handler", "SysTick_Handler",
                    "USART1_IRQHandler", "ADC1_2_IRQHandler",
                    "HRTIM1_FLT_IRQHandler", "HRTIM1_TIMF_IRQHandler"):
        require(handler in startup, f"critical vector missing: {handler}")

    config = (ROOT / "Core/Inc/board_config.h").read_text(encoding="utf-8")
    for macro in ("BOARD_HARDWARE_FLT_CONFIRMED",
                  "BOARD_V15_FEEDBACK_CONFIRMED",
                  "BOARD_SYNC_PINMAP_CONFIRMED",
                  "BOARD_THERMAL_LIMITS_CONFIRMED",
                  "BOARD_PV_LIMIT_CONFIRMED",
                  "BOARD_CURRENT_POLARITY_CONFIRMED",
                  "BOARD_CONTROL_GAINS_VALIDATED",
                  "BOARD_V15_PGOOD_RUNTIME_IMPLEMENTED",
                  "BOARD_FLT_RUNTIME_POLARITY_VALIDATED"):
        require(re.search(rf"#define\s+{macro}\s+0U\b", config) is not None,
                f"safety gate must remain zero: {macro}")
    require(re.search(r"#define\s+BOARD_POWER_OUTPUT_ARMING_ENABLE\s+1U\b", config),
            "external-protection profile must compile power arming")
    require(re.search(r"#define\s+BOARD_EXTERNAL_PROTECTION_CONFIRMED\s+1U\b", config),
            "external-protection profile confirmation missing")
    require(EXPECTED_VERSION in config, "firmware version mismatch")
    for clock_contract in ("BOARD_HSE_HZ                   8000000UL",
                           "BOARD_TARGET_SYSCLK_HZ       170000000UL",
                           "BOARD_HRTIM_PRESCALER_CODE        0U",
                           "BOARD_HRTIM_PERIOD_TICKS          54399UL",
                           "BOARD_PER_CYCLE_ADC_ENABLE         1U"):
        require(clock_contract in config,
                f"170-MHz/100-kHz contract missing: {clock_contract}")
    pll_hz = (8_000_000 // 2) * 85 // 2
    hrtim_hz = pll_hz * 32
    require(pll_hz == 170_000_000,
            "HSE/PLLM/PLLN/PLLR arithmetic is not 170 MHz")
    period_reg = 54_399
    period_counts = period_reg + 1
    require(period_reg <= 0xFFFF and hrtim_hz % period_counts == 0 and
            hrtim_hz // period_counts == 100_000,
            "HRTIM x32/PER arithmetic is not exactly 100 kHz")
    adc_clock_hz = pll_hz // 4
    adc_three_channel_cycles_x10 = 3 * (475 + 125)
    require(adc_three_channel_cycles_x10 * 1_000_000 <
            10 * adc_clock_hz * 10,
            "three injected ADC channels do not fit inside one 10-us cycle")
    expected_pwm_macros = (
        "BOARD_PWM_CHA1_PORT_ID BOARD_GPIO_PORT_B_ID",
        "BOARD_PWM_CHA1_PIN 12U",
        "BOARD_PWM_CHA1_TIMER_ID BOARD_HRTIM_TIMER_C_ID",
        "BOARD_PWM_CHA1_OUTPUT 1U",
        "BOARD_PWM_CHA2_PIN 13U",
        "BOARD_PWM_CHA2_TIMER_ID BOARD_HRTIM_TIMER_C_ID",
        "BOARD_PWM_CHA2_OUTPUT 2U",
        "BOARD_PWM_CHB1_PIN 14U",
        "BOARD_PWM_CHB1_TIMER_ID BOARD_HRTIM_TIMER_D_ID",
        "BOARD_PWM_CHB2_PIN 15U",
        "BOARD_PWM_CHB2_TIMER_ID BOARD_HRTIM_TIMER_D_ID",
        "BOARD_PWM_CHC1_PORT_ID BOARD_GPIO_PORT_C_ID",
        "BOARD_PWM_CHC1_PIN 6U",
        "BOARD_PWM_CHC1_TIMER_ID BOARD_HRTIM_TIMER_F_ID",
        "BOARD_PWM_CHC2_PIN 7U",
        "BOARD_PWM_CHC2_TIMER_ID BOARD_HRTIM_TIMER_F_ID",
        "BOARD_PWM_CHD1_PIN 8U",
        "BOARD_PWM_CHD1_TIMER_ID BOARD_HRTIM_TIMER_E_ID",
        "BOARD_PWM_CHD2_PIN 9U",
        "BOARD_PWM_CHD2_TIMER_ID BOARD_HRTIM_TIMER_E_ID",
        "BOARD_PWM_CHE1_PORT_ID BOARD_GPIO_PORT_A_ID",
        "BOARD_PWM_CHE1_PIN 8U",
        "BOARD_PWM_CHE1_TIMER_ID BOARD_HRTIM_TIMER_A_ID",
        "BOARD_PWM_CHE2_PIN 9U",
        "BOARD_PWM_CHE2_TIMER_ID BOARD_HRTIM_TIMER_A_ID",
        "BOARD_PWM_CHE2_OUTPUT 2U",
        "BOARD_PWM_CHF1_PIN 10U",
        "BOARD_PWM_CHF1_TIMER_ID BOARD_HRTIM_TIMER_B_ID",
        "BOARD_PWM_CHF1_OUTPUT 1U",
        "BOARD_PWM_CHF2_PIN 11U",
        "BOARD_PWM_CHF2_TIMER_ID BOARD_HRTIM_TIMER_B_ID",
    )
    for macro_text in expected_pwm_macros:
        require(macro_text in config,
                f"audited PWM mapping missing: {macro_text}")
    require("BOARD_NONSYNC_PWM_OE_PIN OE3_PIN" in config,
            "non-synchronous outputs must be behind OE3#/PC14")

    safety = (ROOT / "Core/Src/board_safety.c").read_text(encoding="utf-8")
    for required in ("gpio_read(GPIOA, AUX_ENABLE_PIN) == 0UL",
                     "gpio_read(GPIOC, OE3_PIN) != 0UL",
                     "gpio_read(GPIOA, GATE_INHIBIT_PIN) != 0UL"):
        require(required in safety,
                f"physical AUX/OE3/PA2 start readback missing: {required}")

    for script in (ROOT / "tools/build_keil.bat", ROOT / "tools/flash_keil.bat"):
        script_text = script.read_text(encoding="utf-8")
        require(EXPECTED_VERSION in script_text,
                f"version mismatch in {script.name}")
        require("remade_master.md" in script_text,
                f"maintenance history path missing in {script.name}")
    require('"%UV4%" -r "%PROJECT%"' in
            (ROOT / "tools/build_keil.bat").read_text(encoding="utf-8"),
            "build script must perform a full Rebuild")

    hrtim = (ROOT / "Core/Src/board_hrtim.c").read_text(encoding="utf-8")
    require("uint32_t board_hrtim_power_arm" in hrtim and
            "HRTIM1->sCommonRegs.OENR = NONSYNC_OUTPUT_ENABLE_MASK" in hrtim and
            "BOARD_POWER_OUTPUT_ARMING_ENABLE == 0U" in hrtim and
            "board_hrtim_fault_backend_ready() == 0UL" in hrtim and
            "__attribute__((weak)) uint32_t board_hrtim_fault_backend_ready" in hrtim,
            "guarded dual-phase HRTIM arming backend is incomplete")
    require("HRTIM1->sCommonRegs.ODISR = HRTIM_OUTPUT_ALL" in hrtim,
            "all-output disable write missing")
    require("timer_a->SETx2R = HRTIM_SET2R_PER" in hrtim and
            "timer_b->SETx1R = HRTIM_SET1R_PER" in hrtim,
            "stopped Timer A2/B1 timing contract missing")
    require("HRTIM1->sCommonRegs.ADC2R = HRTIM_ADC2R_AD2TAC2" in hrtim and
            "HRTIM_ADC2R_AD2TAC2 | HRTIM_ADC2R_AD2TBC2" not in hrtim,
            "HRTIM trigger must occur once per Timer-A period")
    require("BOARD_HRTIM_PRESCALER_CODE & HRTIM_TIMCR_CK_PSC" in hrtim,
            "HRTIM prescaler is not written/read back explicitly")
    require("board_clock_is_170mhz()" in hrtim and
            "HRTIM_ISR_DLLRDY" in hrtim and
            "HRTIM_MCR_TACEN" in hrtim and
            "if (diag->clock_validated != 0UL)" in hrtim,
            "runtime clock/DLL/sampling validation missing")
    require("timer_b->CNTxR = (BOARD_HRTIM_PERIOD_TICKS + 1UL) / 2UL" in hrtim and
            "HRTIM_MCR_TACEN | HRTIM_MCR_TBCEN" in hrtim and
            "gpio_pin_to_af13(GPIOA, BOARD_NONSYNC_CHE2_PIN)" in hrtim and
            "gpio_pin_to_af13(GPIOA, BOARD_NONSYNC_CHF1_PIN)" in hrtim,
            "A2/B1 180-degree interleaved backend is incomplete")
    for required in ("timer_a->OUTxR = 0UL", "timer_b->OUTxR = 0UL",
                     "HRTIM_TIMISR_O2STAT", "HRTIM_TIMISR_O1STAT",
                     "GPIOA->IDR & (1UL << BOARD_NONSYNC_CHE2_PIN)",
                     "GPIOA->IDR & (1UL << BOARD_NONSYNC_CHF1_PIN)",
                     "arm_fail_stage = 4UL", "arm_fail_stage = 5UL"):
        require(required in hrtim,
                f"pre-release HRTIM/pad edge self-test missing: {required}")
    clock_text = (ROOT / "Core/Src/board_clock.c").read_text(encoding="utf-8")
    for required in ("RCC_PLLCFGR_PLLSRC_HSE", "85UL << RCC_PLLCFGR_PLLN_Pos",
                     "FLASH_ACR_LATENCY_4WS", "PWR_CR5_R1MODE",
                     "RCC_CR_CSSON", "BOARD_CLOCK_ERROR_CSS_ENABLE",
                     "RCC_PLLCFGR_PLLR_Msk"):
        require(required in clock_text, f"clock safety step missing: {required}")
    fast_adc = (ROOT / "Core/Src/board_fast_adc.c").read_text(encoding="utf-8")
    for required in ("FAST_ADC_JEXTSEL_HRTIM_TRG2",
                     "ADC_JSQR_JEXTSEL_4 | ADC_JSQR_JEXTSEL_1 | ADC_JSQR_JEXTSEL_0",
                      "ADC_JSQR_JEXTEN_0", "ADC1_2_IRQHandler",
                      "ADC1->CR |= ADC_CR_JADSTART",
                      "ADC2->CR |= ADC_CR_JADSTART",
                      "FAST_ADC_FIRST_CYCLE_TIMEOUT_LOOPS",
                       "ADC1->JDR1", "ADC2->JDR2", "ADC1->JDR3",
                       "ADC2->JDR3", "ADC_ISR_JQOVF",
                     "BOARD_FAST_ADC_FAULT_SEQUENCE", "++snapshot_lock",
                     "board_fast_adc_trip_sequence_timeout",
                     "board_safety_force_power_off"):
        require(required in fast_adc, f"per-cycle ADC contract missing: {required}")
    for required in ("board_fast_adc_set_duty_limit_command",
                     "FAST_DUTY_LIMIT_DEC_Q15",
                     "board_hrtim_set_phase_duty_q15"):
        require(required in fast_adc,
                f"cycle-synchronous current limiter missing: {required}")
    require("power_control_watchdog_kick();" in
            (ROOT / "power_control.c").read_text(encoding="utf-8") and
            "void power_control_watchdog_kick(void)" in
            (ROOT / "main.c").read_text(encoding="utf-8"),
            "long-running calibration path must service watchdog")
    duty_function = re.search(r"static uint32_t duty_to_ticks.*?^}", hrtim,
                              re.MULTILINE | re.DOTALL)
    require(duty_function is not None and
            "uint64_t" not in duty_function.group(0) and
            ">> 15" in duty_function.group(0),
            "100-kHz duty path still uses avoidable 64-bit division")

    main_text = (ROOT / "main.c").read_text(encoding="utf-8")
    require("PINMAP" in main_text and
            "CHE2<-PA9/TA2" in main_text and
            "CHF1<-PA10/TB1" in main_text,
            "runtime pin-map diagnostic is absent or stale")
    require("power_control_poll_ms(now_ms)" in main_text and
            "if (dwt_ok) { power_control_poll" not in main_text,
            "sampling/protection still depends on DWT")
    require('",Iin:"' in main_text and '",Iout:"' in main_text and
            '",I1:"' not in main_text and '",I2:"' not in main_text,
            "FireWater current field names are incompatible")
    heartbeat = re.search(r"static void print_heartbeat\(void\).*?^}",
                          main_text, re.MULTILINE | re.DOTALL)
    require(heartbeat is not None and "print_state_frame();" not in heartbeat.group(0),
            "heartbeat must be exactly one FireWater line")
    require("power_control_set_mode(POWER_CONTROL_MPPT)" in main_text,
            "MPPT ARM LIMITED still selects CV")
    require("power_control_clear_all_calibration" in main_text,
            "calibration clear does not clear all calibration")
    require("power_control_set_external_fault_latched(1UL)" in main_text and
            "power_control_set_external_fault_latched(0UL)" in main_text,
            "external fault latch is not mirrored into the control backend")
    ws_show = re.search(r"static void ws_show\(void\).*?^}",
                        main_text, re.MULTILINE | re.DOTALL)
    require(ws_show is not None and
            "board_hrtim_sampling_is_running() != 0UL" in ws_show.group(0),
            "WS2812 interrupt masking can corrupt the 100-kHz ADC sequence")

    power_text = (ROOT / "power_control.c").read_text(encoding="utf-8")
    power_header = (ROOT / "power_control.h").read_text(encoding="utf-8")
    require("POWER_ERROR_OCP =" not in power_header and
            "POWER_ERROR_PROTECTION =" not in power_header,
            "legacy error-code aliases still duplicate diagnostic values")
    require("BOARD_CONTROL_TARGET_MAX_MV" in power_text and
            "450000UL" not in power_text,
            "control target can still exceed the 415 V OVP margin")
    require(power_text.count("BOARD_POWER_OUTPUT_ARMING_ENABLE == 0U") >= 2,
            "control mode/duty path is not explicitly gated by arming policy")
    require("board_safety_force_off();" in power_text and
            "POWER_FAULT_ADC_TIMEOUT" in power_text and
            "POWER_FAULT_ADC_RAIL" in power_text,
            "unified software containment path missing")
    require("update_calibration_state" in power_text and
            "voltage_calibration_valid" in power_text,
            "calibration_valid has no real completion path")
    require("ADC_CCR_CKMODE_0 | ADC_CCR_CKMODE_1" in power_text and
            "board_fast_adc_get_snapshot" in power_text,
            "ADC HCLK/4 or fast snapshot integration missing")
    require("elapsed_ms = now_ms - previous_fast_rate_ms" in power_text and
            "(uint64_t)delta * 1000ULL" in power_text,
            "fast-sample rate is not normalized by actual elapsed time")
    require("FAST_ADC_STALL_TIMEOUT_MS 5UL" in power_text and
            "board_fast_adc_trip_sequence_timeout();" in power_text and
            "last_fast_progress_ms" in power_text,
            "fast ADC no-progress watchdog is missing")
    require("i == POWER_ADC_NTC1" in power_text and
            "i == POWER_ADC_NTC2" in power_text and
            "adc_map[i].adc == ADC1 || adc_map[i].adc == ADC2" in power_text,
            "running NTC/aux scans can still collide with injected ADC")
    require("POWER_FAULT_BOOST_UNREACHABLE" in power_text and
            "enforce_boost_reachability" in power_text and
            "external_fault_latched != 0UL" in power_text,
            "boost reachability/external fault gate is missing")
    require("BOARD_CURRENT_STUCK_BLANKING_MS" in power_text and
            "control.outputs_enabled != 0UL" in power_text and
            "BOARD_CURRENT_STUCK_DUTY_Q15" in power_text,
            "zero-current supervision can still trip before physical startup")
    require("control_outer_step();" in power_text and
            "control_fast_step();" in power_text and
            "return -7;" in power_text,
            "START does not synchronously confirm the physical PWM path")
    require("status.last_stop_fault_bits" in main_text and
            "effective = status.fault_bits | status.last_stop_fault_bits" in main_text,
            "contained runtime faults can still disappear before application latching")
    for required in ("POWER_FAULT_PV_UVLO", "POWER_FAULT_CURRENT_IMBALANCE",
                     "POWER_FAULT_CURRENT_STUCK",
                     "POWER_FAULT_VBUS_BUILD_TIMEOUT",
                     "POWER_FAULT_DUTY_SATURATION", "POWER_FAULT_POWER_LIMIT",
                     "POWER_FAULT_SAMPLE_STALE", "mppt_perturb_count",
                     "previous_mppt_sample_sequence",
                     "BOARD_CURRENT_GAIN_MIN_MV_PER_A",
                     "BOARD_VOLTAGE_GAIN_MIN_PPM"):
        require(required in power_text or required in config,
                f"runtime control/protection improvement missing: {required}")
    cal_store = (ROOT / "Core/Src/board_cal_store.c").read_text(encoding="utf-8")
    for required in ("W25Q_JEDEC_ID", "STORE_SLOT0_ADDRESS",
                     "crc32_bytes", "board_cal_store_save",
                     "board_cal_store_load", "FAULT_SLOT_BASE_ADDRESS",
                     "board_fault_store_append", "board_fault_store_read_recent"):
        require(required in cal_store, f"calibration persistence contract missing: {required}")

    uart_text = (ROOT / "Core/Src/board_uart.c").read_text(encoding="utf-8")
    require("rt_thread_mdelay(1)" in main_text and
            "void USART1_IRQHandler(void)" in main_text and
            "USART_CR1_RXNEIE" in uart_text and
            "UART_RX_CAPACITY 256UL" in uart_text and
            "UART_TX_CAPACITY 4096UL" in uart_text,
            "RT-Thread UART burst-safe receive path missing")
    require("BOARD_RX_DRAIN_BUDGET_BYTES" in main_text and
            "board_uart_write_atomic" in main_text and
            "tx_frame_drop" in uart_text,
            "bounded RX drain or atomic TX frame policy missing")
    rtconfig = (ROOT / "rtconfig.h").read_text(encoding="utf-8")
    require("#define RT_USING_LIBC" in rtconfig and
            "#define RT_USING_USER_MAIN" in rtconfig,
            "required RT-Thread ARMClang configuration missing")
    require("RT_MAIN_THREAD_PRIORITY        6" in rtconfig and
            "RT_MAIN_THREAD_STACK_SIZE      4096" in rtconfig,
            "reviewed RT main priority/stack missing")
    for required in ("RT_FAULT_GUARD_PRIORITY 3U",
                     "RT_TELEMETRY_PRIORITY 20U", "rt_start_workers",
                     "owner=main single_writer=1", "rt_stack_unused_bytes",
                     "rt_thread_idle_gethandler", "RTSTACK main_free="):
        require(required in main_text,
                f"reviewed RT scheduling contract missing: {required}")
    components = (ROOT / "third_party/rt-thread/src/components.c").read_text(
        encoding="utf-8")
    idle = (ROOT / "third_party/rt-thread/src/idle.c").read_text(
        encoding="utf-8")
    require('RT_SECTION(".ccmram")' in components and
            'RT_SECTION(".ccmram")' in idle,
            "RT main/idle stacks are not explicitly placed in CCM")
    rt_board = (ROOT / "Core/Src/rt_board.c").read_text(encoding="utf-8")
    require("rt_tick_increase();" in rt_board and
            "board_safety_force_off();" in rt_board and
            "rt_hw_exception_install" in rt_board,
            "RT board tick or exception containment missing")
    require("SysTick_Config(board_clock_get_hz() / RT_TICK_PER_SECOND)" in
            rt_board,
            "RT SysTick does not follow the actual PLL/fallback clock")
    require("NVIC_SetPriority(USART1_IRQn, 5U)" in uart_text and
            "BOARD_FAST_ADC_IRQ_PRIORITY        2U" in config,
            "reviewed ADC/UART interrupt priority contract changed")
    for irq in ("ADC1_2_IRQn", "USART1_IRQn", "SysTick_IRQn", "PendSV_IRQn"):
        require(f"NVIC_GetPriority({irq})" in main_text,
                f"RTDIAG does not read back runtime priority: {irq}")
    context = (ROOT / "third_party/rt-thread/libcpu/arm/cortex-m4/"
                      "context_rvds.S").read_text(encoding="utf-8")
    require("NVIC_PENDSV_PRI EQU     0xFFFF0000" in context,
            "PendSV/SysTick are not kept at the lowest exception priority")
    require("context_rvds.S" in project_text and
            "third_party\\rt-thread\\src\\components.c" in project_text,
            "RT-Thread kernel/port is not in the Keil project")
    require((ROOT / "remade_master.md").exists(), "remade_master.md handoff log missing")

    safety_text = (ROOT / "Core/Src/board_safety.c").read_text(encoding="utf-8")
    for required in ("board_hrtim_force_off();", "OE1_PIN", "OE2_PIN",
                     "OE3_PIN", "GATE_INHIBIT_PIN", "AUX_ENABLE_PIN",
                     "board_safety_request_power_on", "board_v15_pgood_read"):
        require(required in safety_text, f"physical safe-off element missing: {required}")

    control_text = (ROOT / "hardware/netlists/"
                           "Netlist_474控制板_2026-08-28.net").read_text(
                               encoding="utf-8")
    control = parse_nets(control_text)
    control_values = parse_values(control_text)
    require(control_values.get("X1") == "8MHz" and
            control_values.get("C8") == "12pF" and
            control_values.get("C9") == "12pF",
            "8-MHz HSE crystal/load network changed")
    require("Load Capacitance\n10pF" in control_text and
            "Equivalent Series Resistance(ESR)\n250Ω" in control_text,
            "HSE crystal CL/ESR metadata changed; redo oscillator audit")

    # The external PWM labels are U11 Y-side nets. MCU-native HRTIM labels are
    # U11 A-side nets. Check both sides of every buffer route so an innocent
    # looking connector-name edit cannot silently select the wrong timer.
    pwm_routes = {
        "CHA1": ("MCU_CHA1", ("U3-34", "U11-47"), ("U11-2", "U6-2")),
        "CHA2": ("MCU_CHA2", ("U3-35", "U11-46"), ("U11-3", "U6-3")),
        "CHB1": ("MCU_CHB1", ("U3-36", "U11-44"), ("U11-5", "U6-4")),
        "CHB2": ("MCU_CHB2", ("U3-37", "U11-43"), ("U11-6", "U6-5")),
        "CHC1": ("MCU_CHC1", ("U3-38", "U11-41"), ("U11-8", "U6-6")),
        "CHC2": ("MCU_CHC2", ("U3-39", "U11-40"), ("U11-9", "U6-7")),
        "CHD1": ("MCU_CHD1", ("U3-40", "U11-38"), ("U11-11", "U6-8")),
        "CHD2": ("MCU_CHD2", ("U3-41", "U11-37"), ("U11-12", "U6-9")),
        "CHE1": ("MCU_CHE1", ("U3-42", "U11-36"), ("U11-13", "U6-10")),
        "CHE2": ("MCU_CHE2", ("U3-43", "U11-35"), ("U11-14", "U6-11")),
        "CHF1": ("MCU_CHF1", ("U3-44", "U11-33"), ("U11-16", "U6-12")),
        "CHF2": ("MCU_CHF2", ("U3-45", "U11-32"), ("U11-17", "U6-13")),
    }
    for external, (native, native_tokens, external_tokens) in pwm_routes.items():
        require_members(control, native, native_tokens,
                        "control-board native PWM route changed")
        require_members(control, external, external_tokens,
                        "control-board external PWM route changed")

    for net_name, tokens in {
        "PWM_OE1_N": ("U3-26", "U11-1", "R13-1"),
        "PWM_OE2_N": ("U3-4", "U11-48", "R12-1"),
        "PWM_OE3_N": ("U3-3", "U11-25", "R14-1"),
        "MCU_TX": ("U3-59", "PB6", "U4-9", "RXD"),
        "MCU_RX": ("U3-60", "PB7", "U4-8", "TXD"),
        "SPI_SCK": ("U3-56", "PB3", "U1-6", "U2-6"),
        "SPI_MISO": ("U3-57", "PB4", "U1-2", "U2-2"),
        "SPI_MOSI": ("U3-58", "PB5", "U1-5", "U2-5"),
        "SPI_CS_FLASH": ("U3-62", "PB9", "U1-1"),
        "SPI_CS_SRAM": ("U3-55", "PD2", "U2-1"),
        "4_ADC1_3": ("U3-14", "PA2", "U6-30"),
        "GPIO_1": ("U3-19", "PA5", "U6-38"),
    }.items():
        require_members(control, net_name, tokens,
                        "control-board peripheral route changed")

    adc_routes = {
        "0_ADC2_6": ("U3-8", "PC0", "U6-34"),
        "1_ADC2_7": ("U3-9", "PC1", "U6-33"),
        "2_ADC1_1": ("U3-12", "PA0", "U6-32"),
        "3_ADC1_2": ("U3-13", "PA1", "U6-31"),
        "5_ADC1_4": ("U3-17", "PA3", "U6-25"),
        "6_ADC2_3": ("U3-20", "PA6", "U6-26"),
        "7_ADC2_4": ("U3-21", "PA7", "U6-27"),
        "8_ADC2_5": ("U3-22", "PC4", "U6-28"),
        "9_ADC3_1": ("U3-25", "PB1", "U6-29"),
    }
    for net_name, tokens in adc_routes.items():
        require_members(control, net_name, tokens,
                        "control-board ADC route changed")

    for resistor in ("R12", "R13", "R14"):
        require(control_values.get(resistor, "").upper() == "10K",
                f"{resistor} OE# pull-up is no longer 10K")
    require(control_values.get("R11", "").upper() == "4.7K",
            "BOOT0 pull-down is no longer 4.7K")
    require_members(control, "Type_5V", ("USB1-A4B9", "U5-3", "U4-7"),
                    "USB-derived 5V power route changed")
    require_members(control, "V3.3", ("U5-2", "U3-64", "U6-19", "U6-39"),
                    "control-board 3.3V power route changed")
    require("PARTTYPE\nSTM32G474RET3" in control_text and
            "Manufacturer Part\nSTM32G474RBT3" in control_text,
            "known RBT3/RET3 EDA metadata conflict changed; re-audit it")

    nonsync_text = (ROOT / "hardware/netlists/"
                           "Netlist_非同步功率板_2026-08-28.net").read_text(
                               encoding="utf-8")
    nonsync = parse_nets(nonsync_text)
    for net_name in ("FLT_1", "FLT_2", "FLT_3", "FLT_4", "FLT_5",
                     "FLT_6", "EEV_1", "EEV_2"):
        require(len(nonsync.get(net_name, [])) == 1 and
                nonsync[net_name][0].startswith("U9-"),
                f"hardware audit changed: {net_name} is no longer connector-only")
    for net_name, tokens in {
        "GPIO_1": ("U5-6", "U5-7", "U9-6"),
        "CHE2": ("R19-1", "U9-21"),
        "CHF1": ("R12-1", "U9-23"),
        "4_ADC1_3": ("R2-1", "R8-1", "U9-22"),
        "$1N515": ("R19-2", "U24-6"),
        "$1N527": ("R12-2", "U1-6"),
        "$1N505": ("R2-2", "U24-5"),
        "$1N517": ("R8-2", "U1-5"),
    }.items():
        require_members(nonsync, net_name, tokens,
                        "non-synchronous hardware mapping changed")

    for net_name, tokens in {
        "2_ADC1_1": ("R49-1", "U9-18"),
        "3_ADC1_2": ("R47-1", "U9-20"),
        "0_ADC2_6": ("R58-1", "U9-14"),
        "1_ADC2_7": ("R67-1", "U9-16"),
        "5_ADC1_4": ("R81-1", "U9-32"),
        "6_ADC2_3": ("R82-1", "U9-30"),
    }.items():
        require_members(nonsync, net_name, tokens,
                        "non-synchronous ADC source route changed")


    # Cross-board nets: connector names are the join key. In particular, the
    # external CHE2/CHF1 names must not be mistaken for native HRTIM E/F.
    require_members(control, "CHE2", ("U6-11", "U11-14"),
                    "control side CHE2 changed")
    require_members(nonsync, "CHE2", ("U9-21", "R19-1"),
                    "power side CHE2 changed")
    require_members(control, "CHF1", ("U6-12", "U11-16"),
                    "control side CHF1 changed")
    require_members(nonsync, "CHF1", ("U9-23", "R12-1"),
                    "power side CHF1 changed")
    require_members(nonsync, "4_ADC1_3", ("U9-22", "R2-1", "R8-1"),
                    "shared gate-inhibit route changed")
    require_members(nonsync, "GPIO_1", ("U9-6", "U5-6", "U5-7"),
                    "auxiliary-enable route changed")
    require_members(nonsync, "3V3", ("U9-4", "U9-37"),
                    "power-board 3.3V connector route changed")

    sync_text = (ROOT / "hardware/netlists/"
                        "Netlist_同步半桥小板_2026-08-28.net").read_text(
                            encoding="utf-8")
    sync = parse_nets(sync_text)
    sync_values = parse_values(sync_text)
    require(all(token in "\n".join(sync.get("EN", []))
                for token in ("R7-2", "R8-1", "H1-5")),
            "daughterboard EN/DIS connector mapping changed")
    require(all(token in "\n".join(sync.get("$1N45", []))
                for token in ("U2-5", "R7-1", "C9-2")),
            "daughterboard UCC21550 DIS mapping changed")
    require(sync_values.get("R6", "").upper() == "10K",
            "daughterboard dead-time resistor is no longer 10K")

    print(f"PASS: {len(paths)} project entries, {vector_count} vectors, "
          "128-KiB flash algorithm, external-protection arming profile, all 12 buffered PWM "
          "routes and cross-board nets consistent")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, ET.ParseError, OSError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
