#ifndef BOARD_HRTIM_H
#define BOARD_HRTIM_H

#include <stdint.h>

typedef struct {
    uint32_t backend_ready;
    uint32_t timing_configured;
    uint32_t clock_validated;
    uint32_t requested_pwm_hz;
    uint32_t prescaler_code;
    uint32_t protection_ready;
    uint32_t arming_compiled;
    uint32_t profile;
    uint32_t counters_enabled;
    uint32_t output_disable_status;
    uint32_t pwm_pins_high_z;
    uint32_t period_ticks;
    uint32_t compare_ticks;
    uint32_t adc_trigger_configured;
    uint32_t dll_ready;
    uint32_t sampling_running;
    uint32_t effective_hr_clock_mhz;
    uint32_t calculated_pwm_hz;
    uint32_t dma_configured;
    uint32_t deadtime_applicable;
    uint32_t nonsync_che2_timer_output;
    uint32_t nonsync_chf1_timer_output;
    uint32_t phase_shift_ticks;
    uint32_t power_outputs_enabled;
    uint32_t arm_attempts;
    uint32_t arm_fail_stage;
    uint32_t output_enable_readback;
    uint32_t internal_seen_high;
    uint32_t internal_seen_low;
    uint32_t pad_seen_high;
    uint32_t pad_seen_low;
    uint32_t timer_a_output_reg;
    uint32_t timer_b_output_reg;
    uint32_t arm_gate_fail_mask;
} board_hrtim_diag_t;

/* Programs a stopped/read-back timing contract for Timer A output 2 and
 * Timer B output 1, then leaves every counter, output and GPIO AF disabled. */
void board_hrtim_safe_init(void);

/* Idempotent emergency path used by every software safe/fault transition. */
void board_hrtim_force_off(void);

uint32_t board_hrtim_sampling_start(void);
void board_hrtim_sampling_stop(void);
uint32_t board_hrtim_sampling_is_running(void);

/* Writes bounded Timer-A/Timer-B compare shadows while outputs stay disabled.
 * This proves the control-to-CMP handoff without authorizing GPIO AF or OENR. */
uint32_t board_hrtim_set_duty_q15(uint32_t duty_q15);
uint32_t board_hrtim_set_phase_duty_q15(uint32_t duty_a_q15,
                                        uint32_t duty_b_q15);

/* Dormant commissioning backend.  It contains the A2/B1 AF, 180-degree
 * counter phase and OENR path, but returns false unless every compile-time and
 * runtime hardware gate is validated.  It never releases OE#/gate inhibit. */
uint32_t board_hrtim_power_arm(uint32_t runtime_interlocks_ok);
uint32_t board_hrtim_power_outputs_enabled(void);
uint32_t board_hrtim_fault_backend_ready(void);
/* Records a post-HRTIM external gate/readback failure before containment. */
void board_hrtim_mark_arm_failure(uint32_t stage, uint32_t gate_fail_mask);

/* True only after both the stopped timing-register contract and the HRTIM
 * kernel clock have been validated. It still does not authorize arming. */
uint32_t board_hrtim_backend_ready(void);

void board_hrtim_get_diag(board_hrtim_diag_t *diag);

#endif
