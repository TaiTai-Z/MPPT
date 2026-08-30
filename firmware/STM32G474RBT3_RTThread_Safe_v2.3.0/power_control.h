#ifndef POWER_CONTROL_H
#define POWER_CONTROL_H

#include <stdint.h>

#define POWER_ADC_CHANNEL_COUNT 9U

enum {
    POWER_ADC_I1 = 0U,
    POWER_ADC_I2,
    POWER_ADC_NTC1,
    POWER_ADC_NTC2,
    POWER_ADC_AUX0,
    POWER_ADC_AUX1,
    POWER_ADC_VPV,
    POWER_ADC_VBUS,
    POWER_ADC_AUX2
};

enum {
    POWER_FAULT_ADC_INIT = 1UL << 0,
    POWER_FAULT_ADC_TIMEOUT = 1UL << 1,
    POWER_FAULT_ADC_RAIL = 1UL << 2,
    POWER_FAULT_CALIBRATION_REQUIRED = 1UL << 3,
    POWER_FAULT_HRTIM_LOCK = 1UL << 4,
    POWER_FAULT_PROTECTION_UNVERIFIED = 1UL << 5,
    POWER_FAULT_VBUS_OVP = 1UL << 6,
    POWER_FAULT_OCP = 1UL << 7,
    POWER_FAULT_15V_UVLO = 1UL << 8,
    POWER_FAULT_V15_SENSE_UNAVAILABLE = 1UL << 9,
    POWER_FAULT_AUX_DROP = 1UL << 10,
    POWER_FAULT_FLT_INPUT = 1UL << 11,
    POWER_FAULT_EEV_INPUT = 1UL << 12,
    POWER_FAULT_INTERLOCK = 1UL << 13,
    POWER_FAULT_OCP_I1 = 1UL << 14,
    POWER_FAULT_OCP_I2 = 1UL << 15,
    POWER_FAULT_PV_OVP = 1UL << 16,
    POWER_FAULT_OTP_NTC1 = 1UL << 17,
    POWER_FAULT_OTP_NTC2 = 1UL << 18,
    POWER_FAULT_NTC_SENSOR = 1UL << 19,
    POWER_FAULT_CURRENT_SENSOR = 1UL << 20,
    POWER_FAULT_BOOST_UNREACHABLE = 1UL << 21,
    POWER_FAULT_PV_UVLO = 1UL << 22,
    POWER_FAULT_CURRENT_IMBALANCE = 1UL << 23,
    POWER_FAULT_CURRENT_STUCK = 1UL << 24,
    POWER_FAULT_VBUS_BUILD_TIMEOUT = 1UL << 25,
    POWER_FAULT_DUTY_SATURATION = 1UL << 26,
    POWER_FAULT_POWER_LIMIT = 1UL << 27,
    POWER_FAULT_SAMPLE_STALE = 1UL << 28
};

enum {
    POWER_ERROR_MANUAL_STOP = 0x0101U,
    POWER_ERROR_15V_UVLO = 0x0301U,
    POWER_ERROR_PV_OVP = 0x0302U,
    POWER_ERROR_VBUS_OVP = 0x0303U,
    POWER_ERROR_PHASE_A_OCP = 0x0304U,
    POWER_ERROR_PHASE_B_OCP = 0x0305U,
    POWER_ERROR_OTP_NTC1 = 0x0306U,
    POWER_ERROR_OTP_NTC2 = 0x0307U,
    POWER_ERROR_AUX_DROP = 0x0308U,
    POWER_ERROR_ADC_INIT = 0x0201U,
    POWER_ERROR_ADC_READ = 0x0202U,
    POWER_ERROR_CURRENT_SENSOR = 0x0203U,
    POWER_ERROR_NTC_SENSOR = 0x0204U,
    POWER_ERROR_CALIBRATION = 0x0701U,
    POWER_ERROR_HRTIM_LOCK = 0x0501U,
    POWER_ERROR_HRTIM_FLT = 0x0503U,
    POWER_ERROR_HRTIM_EEV = 0x0504U,
    POWER_ERROR_GATE_READBACK = 0x0505U,
    POWER_ERROR_INTERLOCK = 0x0702U,
    POWER_ERROR_CONFIG = 0x0703U,
    POWER_ERROR_CAL_STORE = 0x0704U,
    POWER_ERROR_BOOST_UNREACHABLE = 0x0705U,
    POWER_ERROR_PV_UVLO = 0x0309U,
    POWER_ERROR_CURRENT_IMBALANCE = 0x030AU,
    POWER_ERROR_CURRENT_STUCK = 0x0205U,
    POWER_ERROR_VBUS_BUILD_TIMEOUT = 0x030BU,
    POWER_ERROR_DUTY_SATURATION = 0x0506U,
    POWER_ERROR_POWER_LIMIT = 0x030CU,
    POWER_ERROR_SAMPLE_STALE = 0x0206U
};

typedef enum {
    POWER_CONTROL_OFF = 0U,
    POWER_CONTROL_CV = 1U,
    POWER_CONTROL_MPPT = 2U
} power_control_mode_t;

#define POWER_CALIBRATION_SCHEMA 0x00020001UL

typedef struct {
    uint32_t schema;
    uint32_t zero_i1_mv;
    uint32_t zero_i2_mv;
    uint32_t gain_i1_mv_per_a;
    uint32_t gain_i2_mv_per_a;
    int32_t polarity_i1;
    int32_t polarity_i2;
    uint32_t pv_gain_ppm;
    uint32_t vbus_gain_ppm;
} power_calibration_t;

typedef struct {
    uint32_t sequence;
    uint32_t valid_mask;
    uint32_t fault_bits;
    uint32_t conversion_count;
    uint16_t raw[POWER_ADC_CHANNEL_COUNT];
    uint16_t pin_mv[POWER_ADC_CHANNEL_COUNT];
} power_sample_t;

typedef struct {
    power_control_mode_t mode;
    uint32_t target_mv;
    uint32_t pv_reference_mv;
    int32_t ia_pin_mv;
    int32_t ib_pin_mv;
    int32_t pv_pin_mv;
    int32_t vbus_pin_mv;
    int32_t pv_current_ma;
    int32_t i1_ma;
    int32_t i2_ma;
    int32_t i1_est_ma;
    int32_t i2_est_ma;
    int32_t pv_power_mw;
    int32_t pv_filtered_mv;
    int32_t pv_current_filtered_ma;
    int32_t pv_power_filtered_mw;
    int32_t pv_mv;
    int32_t vbus_mv;
    int32_t ntc1_cdeg;
    int32_t ntc2_cdeg;
    uint32_t temperature_valid_mask;
    uint32_t duty_q15;
    uint32_t proposed_duty_q15;
    uint32_t sample_sequence;
    uint32_t scan_count;
    uint32_t fast_sample_sequence;
    uint32_t fast_adc_irq_count;
    uint32_t fast_adc_incomplete_count;
    uint32_t fast_adc_fault_flags;
    uint32_t per_cycle_sampling_running;
    uint32_t fast_sample_rate_hz;
    uint32_t adc_backend_ready;
    uint32_t adc_ready_mask;
    uint32_t calibration_valid;
    uint32_t current_calibration_valid;
    uint32_t current_polarity_valid;
    uint32_t voltage_calibration_valid;
    uint32_t current_zero_i1_mv;
    uint32_t current_zero_i2_mv;
    uint32_t current_gain_i1_mv_per_a;
    uint32_t current_gain_i2_mv_per_a;
    int32_t current_polarity_i1;
    int32_t current_polarity_i2;
    uint32_t pv_gain_ppm;
    uint32_t vbus_gain_ppm;
    uint32_t v15_sense_valid;
    uint32_t vbus_ovp_limit_mv;
    uint32_t vbus_dynamic_ovp_limit_mv;
    uint32_t vbus_ovp_raw_threshold;
    uint32_t pv_ovp_limit_mv;
    uint32_t ocp_limit_ma;
    uint32_t uvlo_15v_limit_mv;
    int32_t ntc_otp_trip_cdeg;
    int32_t ntc_otp_recover_cdeg;
    uint32_t ntc1_invalid_count;
    uint32_t ntc2_invalid_count;
    uint32_t softstart_limit_q15;
    uint32_t vbus_limit_active;
    uint32_t boost_reachable;
    uint32_t current_limit_active;
    uint32_t current_imbalance_ms;
    uint32_t current_stuck_ms;
    uint32_t vbus_build_elapsed_ms;
    uint32_t duty_saturation_ms;
    uint32_t power_limit_ms;
    uint32_t mppt_perturb_count;
    uint32_t mppt_invalid_sample_count;
    uint32_t controller_gains_validated;
    uint32_t compare_write_ok;
    uint32_t hrtim_backend_ready;
    uint32_t protection_backend_ready;
    uint32_t external_fault_latched;
    uint32_t outputs_enabled;
    uint32_t aux_enable;
    uint32_t gate_inhibit;
    uint32_t fault_bits;
    uint32_t last_stop_fault_bits;
    uint32_t last_stop_fast_fault_flags;
    uint32_t last_stop_duty_q15;
    uint32_t last_stop_time_ms;
} power_control_status_t;

void power_control_init(void);
void power_control_poll_ms(uint32_t now_ms);
uint32_t power_control_scan_now(void);
void power_control_get_sample(power_sample_t *sample);
void power_control_get_status(power_control_status_t *status);
void power_control_watchdog_kick(void);
/* Main/application safety latch. Non-OFF modes are rejected while set. */
void power_control_set_external_fault_latched(uint32_t latched);
int power_control_set_mode(power_control_mode_t mode);
int power_control_set_target_mv(uint32_t target_mv);
int power_control_set_current_calibration(uint32_t zero_i1_mv,
                                          uint32_t zero_i2_mv,
                                          uint32_t gain_i1_mv_per_a,
                                          uint32_t gain_i2_mv_per_a);
int power_control_set_current_polarity(int32_t polarity_i1,
                                       int32_t polarity_i2);
int power_control_set_voltage_calibration(uint32_t pv_gain_ppm,
                                          uint32_t vbus_gain_ppm);
int power_control_auto_zero_current(uint32_t sample_count,
                                    uint32_t *zero_i1_mv,
                                    uint32_t *zero_i2_mv);
void power_control_clear_current_calibration(void);
void power_control_clear_voltage_calibration(void);
void power_control_clear_all_calibration(void);
int power_control_export_calibration(power_calibration_t *calibration);
int power_control_import_calibration(const power_calibration_t *calibration);
int power_control_clear_faults(void);
const char *power_control_mode_name(power_control_mode_t mode);

#endif
