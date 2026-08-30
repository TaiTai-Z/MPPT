#ifndef BOARD_FAST_ADC_H
#define BOARD_FAST_ADC_H

#include <stdint.h>

enum {
    BOARD_FAST_ADC_FAULT_RAIL = 1UL << 0,
    BOARD_FAST_ADC_FAULT_I1_OCP = 1UL << 1,
    BOARD_FAST_ADC_FAULT_I2_OCP = 1UL << 2,
    BOARD_FAST_ADC_FAULT_VBUS_OVP = 1UL << 3,
    BOARD_FAST_ADC_FAULT_SEQUENCE = 1UL << 4,
    BOARD_FAST_ADC_FAULT_PV_OVP = 1UL << 5,
    BOARD_FAST_ADC_FAULT_CURRENT_SENSOR = 1UL << 6
};

typedef struct {
    uint32_t sequence;
    uint32_t irq_count;
    uint32_t incomplete_count;
    uint32_t start_count;
    uint32_t start_failure_count;
    uint32_t armed_mask;
    uint32_t last_start_adc1_cr;
    uint32_t last_start_adc2_cr;
    uint32_t fault_flags;
    uint32_t running;
    uint32_t current_limit_active;
    uint32_t duty_a_q15;
    uint32_t duty_b_q15;
    uint16_t i1_raw;
    uint16_t i2_raw;
    uint16_t ntc1_raw;
    uint16_t ntc2_raw;
    uint16_t vpv_raw;
    uint16_t vbus_raw;
} board_fast_adc_snapshot_t;

uint32_t board_fast_adc_init(void);
uint32_t board_fast_adc_start(void);
void board_fast_adc_stop(void);
uint32_t board_fast_adc_get_snapshot(board_fast_adc_snapshot_t *snapshot);
void board_fast_adc_set_limits(uint16_t i1_zero_raw, uint16_t i2_zero_raw,
                               uint16_t i1_delta_raw, uint16_t i2_delta_raw,
                               uint16_t pv_ovp_raw, uint16_t vbus_ovp_raw,
                               uint32_t current_limit_valid);
void board_fast_adc_set_duty_limit_command(uint32_t duty_q15,
                                           uint16_t i1_operating_delta_raw,
                                           uint16_t i2_operating_delta_raw,
                                           uint32_t enabled);
uint32_t board_fast_adc_fault_flags(void);
void board_fast_adc_clear_fault_flags(void);
void board_fast_adc_trip_sequence_timeout(void);

#endif
